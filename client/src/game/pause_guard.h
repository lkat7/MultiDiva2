#pragma once
#include <cstdint>

// No casual pausing during a multiplayer round: the first pause request only shows a hint, a second
// one between 1 and 5 seconds later really pauses (the others keep playing).
//
// Works on the pause menu's own check of the pause button (Esc / controller Start), so a refused press never
// opens the menu; the menu itself still works normally.
namespace md::pause {

void Install();

// Set every frame: true while this player is in a multiplayer round and the song is playing.
void SetGuardActive(bool active);
void SetForced(bool forced); // Ctrl+F8 debug: guard on outside multiplayer too, for testing alone

// Research (Ctrl+F8 with research=1): log the return address and button id of every button the game reports as
// tapped, to find which call checks song select's confirm button (a non-host's solo start could be blocked there).
void SetButtonLog(bool on);
// The game's input object, as seen in the last button check; null before the first one. Its first bytes are the
// button bitsets (ReDIVA InputState: Tapped, Released, Down...), for finding which bit is a held button.
const void* InputObject();
bool IsForced();
bool GuardActive(); // in a multiplayer round (or the Ctrl+F8 test mode), song playing and not paused

enum class Hint { None, Blocked, TooSoon };

struct State {
    Hint hint = Hint::None;
    uint32_t hintTick = 0; // GetTickCount() of the request that showed the hint
    bool paused = false;   // the pause menu is open right now (read from the game)
};
State GetState();

// A song (re)started or ended: forget any pause in progress (retry/quit from the pause menu
// don't send an unpause event).
void Reset();

constexpr uint32_t ConfirmAfterMs = 1000; // a second request sooner than this is ignored
constexpr uint32_t ConfirmWindowMs = 5000; // later than this starts over

} // namespace md::pause
