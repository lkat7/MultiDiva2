#include "pause_guard.h"
#include "memory.h"
#include "../core/log.h"

#include <atomic>
#include <cstring>
#include <detours.h>
#include <intrin.h>
#include <mutex>
#include <windows.h>

namespace md::pause {

// The pause menu is its own task (TaskPauseManagerSwitch, static object). Its ctrl (0x14065BE90) is a state
// machine on +0x70: in state 0 (closed) it checks the pause button (input button 0xA0, Esc / Start) and opens
// the menu, which then asks pv_game::ctrl to stop the song. The guard answers "not pressed" to that one
// button check. See docs/re-notes.md.
namespace addr {
constexpr uintptr_t ButtonTapped = 0x1402AB260;      // bool (input*, int button), shared by the whole game
constexpr uintptr_t PauseButtonCheckRet = 0x14065BF8C; // return address of the menu's pause button check
constexpr uintptr_t PauseMenuState = 0x14CC0CA90 + 0x70; // int: 0 = menu closed
} // namespace addr

using ButtonFn = bool(__fastcall*)(void* input, int button);
static ButtonFn s_buttonOriginal = nullptr;

static std::atomic<bool> s_guard{false};
static std::atomic<bool> s_forced{false};
static std::mutex s_mutex;
static State s_state;
static uint32_t s_firstRequest = 0; // 0 = no pending first request

void SetGuardActive(bool active) { s_guard = active; }
void SetForced(bool forced)
{
    if (s_forced.exchange(forced) != forced)
        Log("pause guard: test mode %s", forced ? "on" : "off");
}
bool IsForced() { return s_forced; }
bool GuardActive() { return s_guard || s_forced; }

static bool MenuOpen()
{
    int32_t state = 0;
    return SafeRead(addr::PauseMenuState, state) && state != 0;
}

State GetState()
{
    std::lock_guard lock(s_mutex);
    State st = s_state;
    st.paused = MenuOpen();
    return st;
}

void Reset()
{
    std::lock_guard lock(s_mutex);
    s_state = State{};
    s_firstRequest = 0;
}

// Returns whether this press of the pause button may open the menu.
static bool AllowPause()
{
    std::lock_guard lock(s_mutex);
    uint32_t now = GetTickCount();
    uint32_t since = now - s_firstRequest;
    if (!s_firstRequest || since > ConfirmWindowMs) {
        s_firstRequest = now;
        s_state.hint = Hint::Blocked;
        s_state.hintTick = now;
        return false;
    }
    if (since < ConfirmAfterMs) {
        s_state.hint = Hint::TooSoon;
        return false;
    }
    s_firstRequest = 0;
    s_state.hint = Hint::None;
    return true;
}

// Focus loss (alt-tab), the Steam overlay and similar make the song's per-frame code call the pause manager's
// "request pause" (interface vtable slot 7, 0x14065C780, sets the menu's open flag at iface+0x4C9). During a
// round those requests are dropped: the slot is swapped in the vtable (data, no code patch).
constexpr uintptr_t InterfaceVtable = 0x140CC2548;
constexpr int RequestPauseSlot = 7;
constexpr uintptr_t RequestPause = 0x14065C780;
using RequestPauseFn = void(__fastcall*)(void* iface, bool flag);
static RequestPauseFn s_requestOriginal = nullptr;

static void __fastcall RequestPauseHook(void* iface, bool flag)
{
    if (s_guard || s_forced) {
        static DWORD s_lastLog = 0;
        if (GetTickCount() - s_lastLog > 2000) {
            s_lastLog = GetTickCount();
            Log("pause guard: system pause request (alt-tab, overlay...) ignored");
        }
        return;
    }
    s_requestOriginal(iface, flag);
}

static bool __fastcall ButtonHook(void* input, int button)
{
    bool pressed = s_buttonOriginal(input, button);
    if (!pressed || (uintptr_t)_ReturnAddress() != addr::PauseButtonCheckRet || !(s_guard || s_forced))
        return pressed;
    bool allow = AllowPause();
    Log("pause guard: pause pressed (guard %d, test %d) -> %s", (int)s_guard.load(), (int)s_forced.load(),
        allow ? "allowed" : "blocked");
    return allow;
}

void Install()
{
    // lea r8, [rip - 0x2E7]; jmp ...
    static const uint8_t expected[] = { 0x4C, 0x8D, 0x05, 0x19, 0xFD, 0xFF, 0xFF, 0xE9 };
    // call ButtonTapped right before the return address
    static const uint8_t expectedCall = 0xE8;
    uint8_t head[sizeof(expected)] = {};
    uint8_t call = 0;
    int32_t rel = 0;
    bool ok = SafeRead(addr::ButtonTapped, head) && SafeRead(addr::PauseButtonCheckRet - 5, call) &&
              SafeRead(addr::PauseButtonCheckRet - 4, rel) && call == expectedCall &&
              addr::PauseButtonCheckRet + rel == addr::ButtonTapped && memcmp(head, expected, sizeof(head)) == 0;
    if (!ok) {
        Log("pause guard: game code differs from v1.03, not installed");
        return;
    }
    s_buttonOriginal = (ButtonFn)addr::ButtonTapped;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG attach = DetourAttach((void**)&s_buttonOriginal, (void*)ButtonHook);
    LONG commit = DetourTransactionCommit();
    if (attach != NO_ERROR || commit != NO_ERROR) {
        Log("pause guard: hook FAILED (attach=%ld commit=%ld)", attach, commit);
        return;
    }
    Log("pause guard: installed");


    void** slot = (void**)(InterfaceVtable + RequestPauseSlot * sizeof(void*));
    uintptr_t current = 0;
    DWORD old;
    if (SafeRead((uintptr_t)slot, current) && current == RequestPause
        && VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        s_requestOriginal = (RequestPauseFn)current;
        *slot = (void*)RequestPauseHook;
        VirtualProtect(slot, sizeof(void*), old, &old);
        Log("pause guard: system pause requests guarded");
    } else {
        Log("pause guard: request-pause slot is %llX, not guarded", (unsigned long long)current);
    }
}

} // namespace md::pause
