#include "filetrace.h"
#include "watch.h"
#include "../core/log.h"

#include <atomic>
#include <cstring>
#include <windows.h>
#include <tlhelp32.h>

namespace md::filetrace {

static std::atomic<bool> s_active{false};
static std::atomic<int> s_logged{0};
static std::atomic<int> s_chains{0};
static PVOID s_handler = nullptr;
static uintptr_t s_createFile = 0;

constexpr int MaxLines = 300; // opens logged (a mod folder is probed for every file, so one song makes dozens)
constexpr int MaxChains = 5;  // full call chains logged (they are the same for every probe)

static LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 4) || ctx->Rip != s_createFile) // not our DR2
        return EXCEPTION_CONTINUE_SEARCH;
    ctx->Dr6 &= ~4ull;
    ctx->EFlags |= 0x10000; // RF: execute the instruction without breaking on it again

    // CreateFileW(lpFileName = rcx, ...): the path, lower-cased ASCII.
    char path[260] = {};
    __try {
        const wchar_t* w = (const wchar_t*)ctx->Rcx;
        for (int i = 0; i < 259 && w[i]; i++)
            path[i] = (char)(w[i] < 128 ? (w[i] >= 'A' && w[i] <= 'Z' ? w[i] + 32 : w[i]) : '?');
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    for (char* p = path; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (!strstr(path, "sound/song") && !strstr(path, ".ogg"))
        return EXCEPTION_CONTINUE_EXECUTION;

    int n = s_logged.fetch_add(1);
    if (n < MaxLines) {
        Log("filetrace #%d: thread %lu opens %s", n + 1, GetCurrentThreadId(), path);
        if (s_chains.fetch_add(1) < MaxChains) {
            char tag[32];
            wsprintfA(tag, "filetrace #%d", n + 1);
            watch::LogCallChain(ctx, tag);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (or clears, address 0) DR2 as an execute breakpoint on every thread but the calling one.
static DWORD WINAPI Apply(LPVOID address)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 te = { sizeof(te) };
    DWORD set = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId())
            continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t)
            continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                ctx.Dr2 = (DWORD64)address;
                // L2 enable (bit 4); RW2 = 00 (execute) and LEN2 = 00 (bits 24-27 cleared).
                ctx.Dr7 = address ? (ctx.Dr7 & ~0xF000030ull) | 0x10 : ctx.Dr7 & ~0xF000030ull;
                if (SetThreadContext(t, &ctx))
                    set++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return set;
}

// From a helper thread, so the calling (game) thread gets the breakpoint too.
static DWORD ApplyFromHelper(uintptr_t address)
{
    HANDLE t = CreateThread(nullptr, 0, Apply, (LPVOID)address, 0, nullptr);
    DWORD set = 0;
    if (t) {
        WaitForSingleObject(t, 5000);
        GetExitCodeThread(t, &set);
        CloseHandle(t);
    }
    return set;
}

void Start()
{
    Stop();
    HMODULE kb = GetModuleHandleW(L"KernelBase.dll");
    s_createFile = kb ? (uintptr_t)GetProcAddress(kb, "CreateFileW") : 0;
    if (!s_createFile) {
        Log("filetrace: KernelBase!CreateFileW not found");
        return;
    }
    s_logged = 0;
    s_chains = 0;
    s_handler = AddVectoredExceptionHandler(1, OnException);
    DWORD set = ApplyFromHelper(s_createFile);
    s_active = set > 0;
    Log("filetrace: breakpoint on CreateFileW (%llX) set on %lu thread(s): highlight songs now", (unsigned long long)s_createFile,
        set);
}

void Stop()
{
    if (!s_active && !s_handler)
        return;
    ApplyFromHelper(0);
    if (s_handler)
        RemoveVectoredExceptionHandler(s_handler);
    s_handler = nullptr;
    s_active = false;
    Log("filetrace: stopped after %d matching open(s)", s_logged.load());
}

bool Active() { return s_active; }

} // namespace md::filetrace
