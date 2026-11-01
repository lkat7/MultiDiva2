#include "watch.h"
#include "../core/log.h"

#include <atomic>
#include <cstring>
#include <windows.h>
#include <tlhelp32.h>

namespace md::watch {

// Real game code (section names are scrambled): the section holding task_pv_game_init_pv and the other hooks.
constexpr uintptr_t CodeBegin = 0x140001000;
constexpr uintptr_t CodeEnd = 0x140B24A00;

static std::atomic<bool> s_active{false};
static std::atomic<int> s_hits{0};
static int s_maxHits = 0;
static uintptr_t s_address = 0;
static PVOID s_handler = nullptr;
static void (*s_callback)(void*, int32_t) = nullptr;

void SetHitCallback(void (*callback)(void*, int32_t)) { s_callback = callback; }

static bool InGameCode(uintptr_t a) { return a >= CodeBegin && a < CodeEnd; }

static LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 1)) // not our DR0
        return EXCEPTION_CONTINUE_SEARCH;
    ctx->Dr6 = 0;

    int hit = s_hits.fetch_add(1);
    if (hit < s_maxHits) {
        int32_t value = 0;
        __try {
            value = *(volatile int32_t*)s_address;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        // Rip is the instruction after the write. Return addresses: game code pointers found on the stack.
        char stack[512];
        int len = 0;
        int found = 0;
        __try {
            for (uintptr_t* p = (uintptr_t*)ctx->Rsp; found < 10 && p < (uintptr_t*)ctx->Rsp + 0x200; p++) {
                if (InGameCode(*p)) {
                    len += wsprintfA(stack + len, " %08X%08X", (DWORD)(*p >> 32), (DWORD)*p); // wsprintf has no %llX
                    found++;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        stack[len] = 0;
        Log("watch #%d: thread %lu wrote %d after %llX; stack:%s", hit + 1, GetCurrentThreadId(), value,
            (unsigned long long)ctx->Rip, stack);
        if (s_callback)
            s_callback(ctx, value);
        // The registers and the code just before Rip, to read where the written value came from (a load from some
        // object's field: the base register's value gives that object's address).
        auto q = [](DWORD64 v) { return (unsigned long long)v; };
        Log("watch #%d: rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX rbp=%llX", hit + 1, q(ctx->Rax), q(ctx->Rbx),
            q(ctx->Rcx), q(ctx->Rdx), q(ctx->Rsi), q(ctx->Rdi), q(ctx->Rbp));
        Log("watch #%d: r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX", hit + 1, q(ctx->R8), q(ctx->R9),
            q(ctx->R10), q(ctx->R11), q(ctx->R12), q(ctx->R13), q(ctx->R14), q(ctx->R15));
        uint8_t code[80] = {};
        bool ok = true;
        __try {
            memcpy(code, (const void*)(ctx->Rip - sizeof(code)), sizeof(code));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (ok) {
            char hex[sizeof(code) * 2 + 1];
            for (size_t i = 0; i < sizeof(code); i++)
                wsprintfA(hex + i * 2, "%02X", code[i]);
            Log("watch #%d: code before %llX: %s", hit + 1, q(ctx->Rip), hex);
        }
    }
    if (hit + 1 >= s_maxHits)
        ctx->Dr7 &= ~1ull; // enough: disarm on this thread (Stop clears the others)
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (or clears, address 0) DR0 as a 4-byte write watch on every thread but the calling one.
static int SetOnAllThreads(uintptr_t address)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 te = { sizeof(te) };
    int set = 0;
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
                ctx.Dr0 = address;
                // L0 enable, RW0 = 01 (write), LEN0 = 11 (4 bytes).
                ctx.Dr7 = address ? (ctx.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18) : ctx.Dr7 & ~0xF0003ull;
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

// The thread work runs on a helper thread, so the game thread (which calls Start) gets the watch too.
static DWORD WINAPI Apply(LPVOID address) { return (DWORD)SetOnAllThreads((uintptr_t)address); }

static int ApplyFromHelper(uintptr_t address)
{
    HANDLE t = CreateThread(nullptr, 0, Apply, (LPVOID)address, 0, nullptr);
    if (!t)
        return 0;
    WaitForSingleObject(t, 5000);
    DWORD set = 0;
    GetExitCodeThread(t, &set);
    CloseHandle(t);
    return (int)set;
}

bool Start(uintptr_t address, int maxHits)
{
    Stop();
    s_address = address;
    s_maxHits = maxHits;
    s_hits = 0;
    s_handler = AddVectoredExceptionHandler(1, OnException);
    int set = ApplyFromHelper(address);
    s_active = set > 0;
    Log("watch: write watch on %llX set on %d thread(s)", (unsigned long long)address, set);
    return s_active;
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
    Log("watch: stopped after %d hit(s)", s_hits.load());
}

bool Active() { return s_active; }
int Hits() { return s_hits; }

} // namespace md::watch
