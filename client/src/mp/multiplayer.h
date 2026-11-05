#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "../game/events.h"

// Online play: talks to the MultiDiva server (docs/protocol.md) and drives the game
// (auto-start, score reporting). Everything here runs on the main thread from OnFrame.
namespace md::mp {

enum class Conn { Offline, Connecting, Online };

struct Player {
    int id = 0;
    std::string name;
    bool ready = false;
    bool playing = false;
    bool loaded = false; // reached the loading barrier this round
    uint64_t steamId = 0; // 0 = not on Steam (no avatar)
    bool hasAvatar = false; // custom avatar on the server (AvatarUrl), for players without a Steam one
    bool missing = false;   // reported not having the song in focus (State::focus)
    bool away = false;      // lost their connection; the server holds their place for about a minute
    int wins = 0;           // rounds won in this room (session scoreboard)
};

struct Pick {
    int pv = 0, difficulty = 0, edition = 0;
    std::string title;
};

struct LiveScore {
    int id = 0;
    std::string name;
    int score = 0, combo = 0;
    double completion = 0;
    bool finished = false;
    bool failed = false; // finished by failing or quitting
    bool loading = false; // still on the loading screen, before the round was released
    bool lag = false;     // no score update for 2 s since the release (slow game or lost connection)
};

struct FinalResult {
    int id = 0, place = 0;
    std::string name;
    int score = 0, grade = 0, maxCombo = 0;
    int cool = 0, fine = 0, safe = 0, sad = 0, worst = 0;
    double completion = 0;
    bool failed = false;
};

// An open room on the server, for the room list shown before joining one.
struct RoomInfo {
    std::string name, host;
    int players = 0;
    bool playing = false; // a round is in progress
};

struct ChatLine {
    std::string name, text;
    enum Kind { Player, System, Error } kind = Player;
};

struct State {
    Conn conn = Conn::Offline;
    std::string connError;
    int myId = -1;
    std::string serverVersion; // release of the server we're connected to ("" = an older server that doesn't say)

    bool inRoom = false;
    std::string roomName;
    std::string roomState; // "lobby" / "playing"
    int hostId = -1;
    std::optional<Pick> pick;
    int hover = -1; // pv the host has highlighted in song select, -1 = none
    std::string hoverTitle; // its title from the host's game (for songs this player doesn't have)
    int hoverDifficulty = -1; // the chart the host selected for it: 0..3, -1 = unknown
    int hoverEdition = -1;     // 1 = the Extra chart
    int focus = -1; // pv everyone reports on: the hovered one, else the pick
    std::string mode = "score"; // how the room ranks players: "score" or "accuracy" (the completion percentage)
    int rounds = 0; // finished rounds with two or more players in this room (what Player::wins counts out of)
    std::string toast;         // a short notice drawn over the game (blocked start, new host...), see ToastAt
    uint32_t toastAt = 0;      // GetTickCount() when it was set (it shows for a few seconds)
    int countdown = 0;       // 3, 2, 1 while the round is being released, else 0
    uint32_t countdownAt = 0; // GetTickCount() when that number arrived
    std::vector<Player> players;

    std::deque<ChatLine> chat;
    std::vector<LiveScore> scores;  // live leaderboard of the current round
    uint32_t scoresAt = 0;          // GetTickCount() of the last `scores` message
    std::vector<FinalResult> results; // last round
    bool showResults = false;

    bool inRound = false;      // this player is part of the round being played
    bool finishedSelf = false; // we reported our result (failed, quit or finished) and the others may still be playing
    bool startPending = false; // `start` received, waiting to reach song select
    std::string roomNotice;    // why we are out of the room (kicked), shown on the join screen
    std::vector<RoomInfo> rooms; // the server's open rooms while we're connected but not in one
    std::string rejoinRoom; // the connection dropped while in this room (Rejoin() goes back)
    uint32_t connLostAt = 0; // GetTickCount() of the drop (the banner shows for a few seconds)
    bool reconnecting = false; // the connection dropped in a room and we keep retrying with our session token
};

const State& Get();
bool IsHost();
const Player* Me();

void Connect();   // uses GetConfig() server/port/name, joins GetConfig().room
void Connect(const std::string& joinRoom); // "" = stay in the lobby (room list)
void HostAndConnect(); // start hosting (net/host.h) and join our own server once it runs
void HostAndConnect(const std::string& joinRoom);
void Disconnect();
void Rejoin(); // after a drop: the same server again, and back into State::rejoinRoom
void JoinRoom(const std::string& room);
void LeaveRoom();
void SetReady(bool ready);
void SendChat(const std::string& text);
void GiveHost(int playerId); // host only
void Kick(int playerId);     // host only
void DismissResults();
void SetMode(const std::string& mode); // host only, lobby only: "score" or "accuracy"
std::string ServerUrl();             // ws(s)://host:port/path of the current connection
std::string AvatarUrl(int playerId); // custom avatar of a player on the current server (Player::hasAvatar)

void Tick();                          // once per frame
void OnGameEvent(const GameEvent& e); // from the session tick

} // namespace md::mp
