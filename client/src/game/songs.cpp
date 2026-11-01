#include "songs.h"
#include "launch.h"
#include "memory.h"
#include "../core/log.h"

#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <windows.h>
#include <tlhelp32.h>


namespace md::songs {

namespace addr {
// pv_db: PvDb() returns the database, PvEntry(db, pv) a pointer to the song's entry pointer (null when the
// song isn't installed; virtualized, but song select's start builder relies on the null check too).
// PvChart(entry, difficulty, edition) returns the chart or null (edition 2 = either).
constexpr uintptr_t PvDb = 0x140580490;
constexpr uintptr_t PvEntry = 0x140581F50;
constexpr uintptr_t PvChart = 0x1404BCA70;
const uint8_t PvDbBytes[] = { 0x48, 0x8D, 0x05 };                    // lea rax, [rip+...]; ret
const uint8_t PvEntryBytes[] = { 0xE9 };                             // jmp into the protector
const uint8_t PvChartBytes[] = { 0x48, 0x63, 0xC2, 0x48, 0x8D, 0x14, 0x40 }; // movsxd rax, edx; lea rdx, [rax+rax*2]
// Song select's per-frame "load the highlighted song's outfit choices" thunk: rcx = song list,
// highlighted pv = int16 at +0xA88, then jumps to the loader (0x1401EAD50).
constexpr uintptr_t HighlightThunk = 0x1406DF6F0;
constexpr size_t HighlightPv = 0xA88;
const uint8_t HighlightThunkBytes[] = { 0x48, 0x83, 0xEC, 0x28, 0x0F, 0xBF, 0x91, 0x88, 0x0A, 0x00, 0x00 };
} // namespace addr

using PvDbFn = void*(__fastcall*)();
using PvEntryFn = void**(__fastcall*)(void* db, int32_t pv);
using PvChartFn = void*(__fastcall*)(void* entry, int32_t difficulty, int32_t edition);

static std::atomic<int32_t> s_highlighted{-1};
static std::atomic<uintptr_t> s_listObject{0}; // the song list object seen by the highlight hook (rcx)
static std::atomic<bool> s_listWatch{false};

template <size_t N> static bool CodeIs(uintptr_t address, const uint8_t (&bytes)[N])
{
    uint8_t check[N];
    return SafeRead(address, check) && memcmp(check, bytes, N) == 0;
}

bool Has(int32_t pv, int32_t difficulty, int32_t edition)
{
    static const bool codeOk = CodeIs(addr::PvDb, addr::PvDbBytes) && CodeIs(addr::PvEntry, addr::PvEntryBytes)
        && CodeIs(addr::PvChart, addr::PvChartBytes);
    if (!codeOk)
        return true; // unknown game version: never claim a song is missing
    if (pv < 0)
        return false;
    void* db = ((PvDbFn)addr::PvDb)();
    void** found = ((PvEntryFn)addr::PvEntry)(db, pv);
    void* entry = nullptr;
    if (!found || !SafeRead((uintptr_t)found, entry) || !entry)
        return false;
    if (difficulty < 0)
        return true;
    return ((PvChartFn)addr::PvChart)(entry, difficulty, edition) != nullptr;
}

// Patching the thunk crashes the game as soon as song select opens (the copy protection checks that code),
// so it is watched with a hardware execute breakpoint (debug register DR1) instead: no game byte changes.
// The handler reads the highlighted pv from rcx and resumes; the watch tool (watch.cpp) uses DR0.
static LONG CALLBACK OnBreakpoint(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP
        || (uintptr_t)info->ExceptionRecord->ExceptionAddress != addr::HighlightThunk)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* ctx = info->ContextRecord;
    int16_t pv = -1;
    if (SafeRead(ctx->Rcx + addr::HighlightPv, pv))
        s_highlighted = pv;
    s_listObject = ctx->Rcx;
    ctx->Dr6 &= ~2ull;
    ctx->EFlags |= 0x10000; // RF: run the instruction without breaking on it again
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets DR1 = execute breakpoint on the thunk on every thread but the calling one.
static DWORD WINAPI ArmThreads(LPVOID)
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
                ctx.Dr1 = addr::HighlightThunk;
                // L1 enable (bit 2); RW1 = 00 (execute) and LEN1 = 00 (bits 20-23 cleared).
                ctx.Dr7 = (ctx.Dr7 & ~0xF0000Cull) | 4;
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

int32_t Hovered() { return launch::InSongSelect() ? s_highlighted.load() : -1; }

static void* Entry(int32_t pv)
{
    void* db = ((PvDbFn)addr::PvDb)();
    void** found = ((PvEntryFn)addr::PvEntry)(db, pv);
    void* entry = nullptr;
    return found && SafeRead((uintptr_t)found, entry) ? entry : nullptr;
}

// Reads an MSVC std::string (16-byte buffer or pointer, then size and capacity) at `at`, if it looks like one.
static bool ReadStdString(uintptr_t at, std::string& out)
{
    struct { union { char buf[16]; const char* ptr; }; uint64_t size, cap; } s;
    if (!SafeRead(at, s) || s.size == 0 || s.size > 256 || s.cap < s.size || s.cap > 4096)
        return false;
    char text[257] = {};
    if (s.cap < 16)
        memcpy(text, s.buf, (size_t)s.size);
    else if (!SafeCopy(text, s.ptr, (size_t)s.size))
        return false;
    for (size_t i = 0; i < s.size; i++)
        if ((unsigned char)text[i] < 0x20)
            return false;
    out.assign(text, (size_t)s.size);
    return true;
}

static bool CodeOk()
{
    static const bool ok = CodeIs(addr::PvDb, addr::PvDbBytes) && CodeIs(addr::PvEntry, addr::PvEntryBytes)
        && CodeIs(addr::PvChart, addr::PvChartBytes);
    return ok;
}

// pv_db_pv, in ReDIVA's order: id (int32) +0x00, date +0x04, song_name (std::string, 32 bytes) +0x08, song_name_reading
// +0x28, remix_parent +0x48, bpm +0x4C, song_file_name +0x50 (the strings match docs/re-notes.md).
constexpr size_t EntryBpm = 0x4C;
// pv_db_pv_difficulty: difficulty +0x00, edition +0x04, attribute (type + bitset<128>, 24 bytes) +0x08,
// script_file_name (std::string) +0x20, level (pv_level, half stars: 0x0F = 7.5) +0x40. NOT confirmed on Mega Mix+:
// the debug panel's chart dump is how to check it.
constexpr size_t ChartLevel = 0x40;

static void* Chart(void* entry, int32_t difficulty, int32_t edition)
{
    return entry ? ((PvChartFn)addr::PvChart)(entry, difficulty, edition) : nullptr;
}

float Level(int32_t pv, int32_t difficulty, int32_t edition)
{
    static std::map<uint64_t, float> cache;
    uint64_t key = (uint64_t)(uint32_t)pv << 8 | (uint32_t)(difficulty & 15) << 4 | (uint32_t)(edition & 15);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    float stars = -1;
    int32_t half = -1;
    bool read = false;
    if (CodeOk() && pv >= 0 && difficulty >= 0 && Has(pv, difficulty, edition)) {
        void* chart = Chart(Entry(pv), difficulty, edition);
        read = chart && SafeRead((uintptr_t)chart + ChartLevel, half);
        if (read && half >= 1 && half <= 20)
            stars = half / 2.0f;
    }
    // The layout is from the Arcade game's decompilation: the first lookups go to the normal log so a player's log
    // shows whether it holds on this version.
    static int logged = 0, loggedRejected = 0;
    if (Has(pv, difficulty, edition) && (logged < 12 || (stars <= 0 && loggedRejected < 12))) {
        logged++;
        if (stars <= 0)
            loggedRejected++;
        Log("songs: level of pv %d diff %d edition %d: %s (raw %d at chart+0x%zX) -> %s", pv, difficulty, edition,
            read ? "read" : "unreadable", half, ChartLevel, stars > 0 ? "accepted" : "rejected, not shown");
    }
    cache[key] = stars;
    return stars;
}

int32_t Bpm(int32_t pv)
{
    static std::map<int32_t, int32_t> cache;
    auto it = cache.find(pv);
    if (it != cache.end())
        return it->second;
    int32_t bpm = -1;
    int32_t v = -1;
    bool read = false;
    if (CodeOk() && pv >= 0 && Has(pv)) {
        if (void* entry = Entry(pv)) {
            read = SafeRead((uintptr_t)entry + EntryBpm, v);
            if (read && v >= 30 && v <= 400)
                bpm = v;
        }
    }
    static int logged = 0;
    if (logged < 6 && pv >= 0 && Has(pv)) {
        logged++;
        Log("songs: bpm of pv %d: %s (raw %d at entry+0x%zX) -> %s", pv, read ? "read" : "unreadable", v, EntryBpm,
            bpm > 0 ? "accepted" : "rejected, not shown");
    }
    cache[pv] = bpm;
    return bpm;
}

void DumpChart(int32_t pv, int32_t difficulty, int32_t edition)
{
    if (!CodeOk() || !Has(pv))
        return;
    void* entry = Entry(pv);
    void* chart = Chart(entry, difficulty, edition);
    Log("songs: pv %d diff %d edition %d: entry %p chart %p, level %.1f bpm %d", pv, difficulty, edition, entry, chart,
        Level(pv, difficulty, edition), Bpm(pv));
    auto dump = [](const char* what, void* base, size_t bytes) {
        for (size_t off = 0; base && off < bytes; off += 16) {
            int32_t w[4] = {};
            if (!SafeRead((uintptr_t)base + off, w))
                break;
            Log("songs:   %s +0x%03zX: %d %d %d %d", what, off, w[0], w[1], w[2], w[3]);
        }
    };
    dump("entry", entry, 0x80);
    dump("chart", chart, 0x100);
}

std::string Title(int32_t pv)
{
    // pv_db entries are 0x3E0 bytes; +0x08 is the song name as song select shows it (the game's language),
    // +0x28 a second copy. Cached: the database doesn't change while the game runs.
    static std::map<int32_t, std::string> cache;
    auto it = cache.find(pv);
    if (it != cache.end())
        return it->second;
    std::string title;
    if (Has(pv))
        if (void* entry = Entry(pv))
            ReadStdString((uintptr_t)entry + 0x08, title);
    if (!title.empty())
        cache[pv] = title;
    return title;
}

void DumpEntryStrings(int32_t pv)
{
    void* entry = Entry(pv);
    Log("songs: pv %d entry %p", pv, entry);
    if (!entry)
        return;
    for (size_t off = 0; off < 0x1000; off += 8) {
        std::string text;
        if (ReadStdString((uintptr_t)entry + off, text))
            Log("songs:   +0x%03zX \"%s\"", off, text.c_str());
    }
}

static bool s_installed = false;

// From a helper thread, so the calling (game) thread gets the breakpoint too.
static DWORD Arm()
{
    HANDLE t = CreateThread(nullptr, 0, ArmThreads, nullptr, 0, nullptr);
    DWORD set = 0;
    if (t) {
        WaitForSingleObject(t, 5000);
        GetExitCodeThread(t, &set);
        CloseHandle(t);
    }
    return set;
}

void Install()
{
    if (!CodeIs(addr::HighlightThunk, addr::HighlightThunkBytes)) {
        Log("songs: song select code differs from v1.03, no highlight tracking");
        return;
    }
    AddVectoredExceptionHandler(1, OnBreakpoint);
    s_installed = true;
    Log("songs: highlight breakpoint set on %lu thread(s)", Arm());
}

// One region of the research watch: int32s that flip between two values 0..4 are logged with their offset, and every
// change inside [allFrom, allTo) is logged whatever its value (the field may be a byte, a short or a bigger enum).
static void WatchRegion(const char* name, uintptr_t base, size_t size, size_t allFrom, size_t allTo,
    std::vector<int32_t>& prev, uintptr_t& prevBase)
{
    std::vector<int32_t> cur(size / 4, 0x7FFFFFFF);
    if (!base)
        return;
    bool any = false;
    for (size_t off = 0; off < size; off += 0x100) // chunks: the object may be shorter than the span, or have holes
        any |= SafeCopy(cur.data() + off / 4, (const void*)(base + off), 0x100);
    if (!any)
        return;
    if (prevBase != base || prev.size() != cur.size()) {
        prev = cur;
        prevBase = base;
        Log("songs: watching %s at %llX (%zu bytes)", name, (unsigned long long)base, size);
        return;
    }
    static DWORD windowStart = 0;
    static int lines = 0;
    DWORD now = GetTickCount();
    if (now - windowStart > 1000)
        windowStart = now, lines = 0;
    for (size_t i = 0; i < cur.size(); i++) {
        if (cur[i] == prev[i] || cur[i] == 0x7FFFFFFF || prev[i] == 0x7FFFFFFF)
            continue;
        bool small = cur[i] >= 0 && cur[i] <= 4 && prev[i] >= 0 && prev[i] <= 4;
        bool near = i * 4 >= allFrom && i * 4 < allTo;
        if ((small || near) && lines++ < 200) // at most 200 lines a second: animation counters can be noisy
            Log("songs: %s +0x%03zX: %d -> %d (0x%X -> 0x%X)%s", name, i * 4, prev[i], cur[i], (unsigned)prev[i],
                (unsigned)cur[i], small ? " *" : "");
    }
    prev = std::move(cur);
}

void SetListWatch(bool on)
{
    if (s_listWatch.exchange(on) != on)
        Log("songs: song select watch %s", on ? "on" : "off");
}

static void WatchSongSelect()
{
    static std::vector<int32_t> listPrev, recordPrev;
    static uintptr_t listBase = 0, recordBase = 0;
    static int32_t lastPv = -2;
    if (!launch::InSongSelect())
        return;
    int32_t pv = s_highlighted;
    if (pv != lastPv) {
        lastPv = pv;
        Log("songs: highlight is now pv %d", pv);
    }
    // The highlighted pv is the int16 at list+0xA88: state next to it is the likely home of the selected difficulty.
    WatchRegion("list", s_listObject, 0x1800, 0xA00, 0xB00, listPrev, listBase);
    WatchRegion("record", 0x141750890, 0x600, 0, 0x100, recordPrev, recordBase);
}

void Tick()
{
    if (s_listWatch)
        WatchSongSelect();
    // Threads created after Install have no breakpoint: on song select without a highlight seen yet,
    // arm again every 2 s.
    static DWORD lastArm = 0;
    if (s_installed && s_highlighted < 0 && launch::InSongSelect() && GetTickCount() - lastArm > 2000) {
        lastArm = GetTickCount();
        Log("songs: no highlight seen on song select, breakpoint re-set on %lu thread(s)", Arm());
    }
}

} // namespace md::songs
