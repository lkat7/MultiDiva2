#pragma once
#include "../mp/multiplayer.h"

#include <array>
#include <cstdint>
#include <string>

// Full-screen multiplayer screens drawn with the game's own sprites and font (ui/game_sprites): the room, and the
// screen before it (connect / host / room list). Each is an invisible full-screen ImGui window (so the mouse and
// typing go to us), returns what the user clicked, and leaves doing it to the caller (ui/multiplayer_ui), which
// keeps these screens free of networking (tools/ui-preview renders them with made-up state).
namespace md::ui {

// What the room screen shows besides mp::State.
struct RoomView {
    std::string status; // the round's state in a line (who's missing the song, waiting for N players...)
    enum class Tone { Normal, Good, Bad } statusTone = Tone::Normal;
    bool botRunning = false;
    bool ownOutfits = true;
    bool showLeaderboard = true, showGaps = true;
    bool browseSounds = true;
    // The song on the title plate: the one the host has highlighted in song select (live), else the last pick.
    std::string songTitle;  // "" = none: `songHint` instead
    int songDifficulty = -1; // 0..4 (4 = Extra Extreme) for the chip, -1 = none (a highlight has no difficulty)
    float songLevel = -1;    // stars of that chart (7.5...), -1 = unknown
    // For a highlight: the charts the song has, by difficulty 0..3 and the extra chart (4). -1 = no such chart,
    // 0 = it exists (level unknown), else its stars.
    std::array<float, 5> charts = { -1, -1, -1, -1, -1 };
    int selectedChart = -1; // which of `charts` the host has selected (0..3, 4 = the Extra chart), -1 = unknown
    int songBpm = -1;        // tempo, -1 = unknown
    std::string songHint;
    int songPv = -1; // its jacket, -1 = none
    std::string inviteUrl; // we host this server: the address to share (Copy invite in the header)
    bool inviteCopied = false;
};

// The colour a difficulty is shown in (0..4, 4 = Extra Extreme), ABGR like IM_COL32.
unsigned int DifficultyColor(int difficulty);

struct RoomAction {
    enum Kind { None, ToggleReady, PickSong, Leave, GiveHost, Kick, Chat, ToggleBot, ToggleOutfits, ToggleLeaderboard, ToggleGaps, ToggleBrowseSounds,
        CopyInvite } kind = None;
    int player = -1;  // GiveHost, Kick
    std::string text; // Chat
};

RoomAction DrawRoomScene(const mp::State& s, const RoomView& view);

// What the connect screen shows and edits besides mp::State. `name` and `server` are edited in place.
struct ConnectView {
    std::string* name = nullptr;
    std::string* server = nullptr;
    std::string serverShown; // the address we're connected to
    uint64_t steamId = 0;    // own picture
    enum class HostPhase { Off, Busy, Ready, Failed } hostPhase = HostPhase::Off;
    std::string hostMessage, hostUrl;
    bool permanent = false; // a permanent tunnel address is set up
    bool copied = false;    // "Copied!" right after Copy
    std::string notice;     // e.g. "type a name first"
    std::string joinRoom;   // out: the room clicked (JoinRoom)
    std::string rejoinRoom; // the connection dropped while in this room: offer to go back
};

enum class ConnectAction { None, Save, Connect, JoinRoom, CreateRoom, Disconnect, Host, StopHosting, CopyAddress, AddressSetup,
    Rejoin };

ConnectAction DrawConnectScene(const mp::State& s, ConnectView& v);

// Font sizes of the old ImGui scene fonts at a screen height (baked into the atlas at load; the screens now use the
// game's font and only fall back to these until it has been seen loaded).
float SceneTitlePx(float screenHeight);
float SceneButtonPx(float screenHeight);
float SceneTextPx(float screenHeight);
float SceneSmallPx(float screenHeight);

} // namespace md::ui
