#include "multiplayer.h"
#include "../core/base64.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../game/launch.h"
#include "../game/pause_guard.h"
#include "../game/session.h"
#include "../game/songs.h"
#include "../game/steam.h"
#include "../game/sync_start.h"
#include "../game/titles.h"
#include "../net/host.h"
#include "../net/ws_client.h"
#include "../ui/own_avatar.h"

#include <algorithm>
#include <cstring>
#include <windows.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace md::mp {

static constexpr int ProtocolVersion = 1;
static constexpr size_t MaxChatLines = 100;
static constexpr DWORD ScoreIntervalMs = 100;
static constexpr DWORD StartTimeoutMs = 15000;
// A start that came in elsewhere waits until song select has been up this long (and has a highlighted song):
// starting in its first frames, before it has set itself up, loads the song into a black screen.
static constexpr DWORD SongSelectSettleMs = 1000;
static DWORD s_songSelectSince = 0; // 0: not on song select

static State s;
// Never destroyed: at process exit the destructor would wait on network threads under the loader lock.
static net::WsClient& s_ws = *new net::WsClient;
static net::WsClient::Status s_lastStatus = net::WsClient::Status::Disconnected;

static launch::Block s_startBlock;
static DWORD s_startReceivedAt = 0;
static DWORD s_lastScoreSent = 0;
static bool s_finishSent = false;
static bool s_hostStartSent = false; // we sent `start` and wait for its echo
static bool s_titleSent = false;     // host: the round's title went out (with `start` or later)
static std::string s_haveReported;   // song (pv/difficulty/edition) we last reported having or not
static int s_hoverSent = -2;         // host: last highlighted song sent, -2 = none yet
static DWORD s_hoverSentAt = 0;
static constexpr DWORD HoverIntervalMs = 150;
static void ReportHave();

// Loading barrier: hold on NOW LOADING until the server says everyone has loaded.
static constexpr DWORD HoldSafetyMs = 45000; // release on our own if the server never answers
static bool s_loadedSent = false;
static bool s_goReceived = false;
static DWORD s_holdSince = 0;

static void ArmBarrier()
{
    sync::SetHoldStep(sync::BarrierStep);
    s_loadedSent = s_goReceived = false;
    s.countdown = 0;
}

static void DisarmBarrier()
{
    s.countdown = 0;
    sync::Release(); // releases a held load and disarms the hold
}

const State& Get() { return s; }
bool IsHost() { return s.inRoom && s.hostId == s.myId; }

const Player* Me()
{
    for (const Player& p : s.players)
        if (p.id == s.myId)
            return &p;
    return nullptr;
}

static void AddChat(ChatLine::Kind kind, std::string name, std::string text)
{
    s.chat.push_back({ std::move(name), std::move(text), kind });
    while (s.chat.size() > MaxChatLines)
        s.chat.pop_front();
}

static void Send(const char* type, json data = json::object())
{
    // While reconnecting the socket is dead or not yet greeted: anything but the hello would be lost or refused.
    if (s.reconnecting && strcmp(type, "hello") != 0)
        return;
    s_ws.Send(json{ { "type", type }, { "data", std::move(data) } }.dump());
}

static std::wstring Widen(const std::string& s8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s8.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s8.c_str(), -1, out.data(), n);
    return out;
}

struct Endpoint {
    std::string host;
    int port;
    std::string path;
    bool secure;
};

// Accepts "host" (uses the port setting), "ws://host[:port][/path]" or "wss://host[:port][/path]".
// wss without a port defaults to 443, which is what tunnels like trycloudflare.com serve.
static Endpoint ParseServer(std::string server, int port)
{
    Endpoint ep{ "", port, "/ws", false };
    if (server.rfind("wss://", 0) == 0) {
        ep.secure = true;
        ep.port = 443;
        server = server.substr(6);
    } else if (server.rfind("ws://", 0) == 0) {
        ep.port = 80;
        server = server.substr(5);
    } else {
        ep.host = server;
        return ep;
    }
    size_t slash = server.find('/');
    if (slash != std::string::npos) {
        std::string path = server.substr(slash);
        if (path.size() > 1)
            ep.path = path;
        server = server.substr(0, slash);
    }
    size_t colon = server.find(':');
    if (colon != std::string::npos) {
        ep.port = atoi(server.c_str() + colon + 1);
        server = server.substr(0, colon);
    }
    ep.host = server;
    return ep;
}

static bool s_connectWhenHosted = false;

// "multidiva.example.com" / "https://..." -> "wss://multidiva.example.com"
static std::string NormalizeWss(std::string url)
{
    for (const char* prefix : { "https://", "http://", "wss://", "ws://" })
        if (url.rfind(prefix, 0) == 0)
            url = url.substr(strlen(prefix));
    while (!url.empty() && (url.back() == '/' || url.back() == ' '))
        url.pop_back();
    return url.empty() ? url : "wss://" + url;
}

// Room to join as soon as the server says welcome ("" = stay in the lobby and show its room list).
static std::string s_joinOnWelcome;

// A dropped connection in a room is retried for a while: the token from `welcome` lets the new connection take over
// our old place (id, score, round) as long as the server still holds it (it waits about a minute).
static std::string s_resumeToken;
static DWORD s_reconnectUntil = 0;
static DWORD s_nextAttempt = 0;
static int s_attempts = 0;
static json s_finishResult; // what we reported at the end of the round, to report again if the drop lost it
constexpr DWORD ReconnectWindowMs = 55000;

void HostAndConnect() { HostAndConnect(GetConfig().room); }

void HostAndConnect(const std::string& joinRoom)
{
    s_joinOnWelcome = joinRoom;
    const Config& cfg = GetConfig();
    host::Options options;
    if (!cfg.tunnelToken.empty() && !cfg.tunnelUrl.empty()) {
        options.permanentUrl = NormalizeWss(cfg.tunnelUrl);
        options.token = cfg.tunnelToken;
    }
    host::Start(options);
    s_connectWhenHosted = true;
}

static Endpoint s_endpoint{ "", 0, "/ws", false }; // of the current connection

std::string ServerUrl()
{
    return std::string(s_endpoint.secure ? "wss://" : "ws://") + s_endpoint.host + ":" + std::to_string(s_endpoint.port)
        + s_endpoint.path;
}

std::string AvatarUrl(int playerId)
{
    // Served next to the WebSocket: ws://host:port/ws -> http://host:port/avatar/<id>.
    std::string base = s_endpoint.path;
    if (base.size() >= 2 && base.compare(base.size() - 2, 2, "ws") == 0)
        base.erase(base.size() - 2);
    if (base.empty() || base.back() != '/')
        base += '/';
    return std::string(s_endpoint.secure ? "https://" : "http://") + s_endpoint.host + ":" + std::to_string(s_endpoint.port)
        + base + "avatar/" + std::to_string(playerId);
}

// The last address we connected to (a drop rejoins it, even when the config has changed meanwhile).
static std::string s_lastServer;
static int s_lastPort = 0;

static void ConnectTo(const std::string& server, int port)
{
    const Config& cfg = GetConfig();
    s_lastServer = server;
    s_lastPort = port;
    s_resumeToken.clear(); // a connection chosen by the player starts a new session
    Endpoint ep = ParseServer(server, port);
    s_endpoint = ep;
    s = State{};
    s.conn = Conn::Connecting;
    Log("mp: connecting to %s://%s:%d%s as %s", ep.secure ? "wss" : "ws", ep.host.c_str(), ep.port, ep.path.c_str(),
        cfg.name.c_str());
    s_ws.Connect(Widen(ep.host), (uint16_t)ep.port, Widen(ep.path), ep.secure);
}

void Connect() { Connect(GetConfig().room); }

void Connect(const std::string& joinRoom)
{
    s_joinOnWelcome = joinRoom;
    ConnectTo(GetConfig().server, GetConfig().port);
}

void Rejoin()
{
    std::string room = s.rejoinRoom;
    s_joinOnWelcome = room;
    ConnectTo(s_lastServer, s_lastPort);
}

void Disconnect()
{
    DisarmBarrier();
    s_ws.Close();
    s = State{};
}

void JoinRoom(const std::string& room)
{
    s.roomNotice.clear();
    Send("join", { { "room", room } });
}
void SetReady(bool ready) { Send("ready", { { "ready", ready } }); }
void DismissResults() { s.showResults = false; }

static void ClearRoom();

void LeaveRoom()
{
    Send("leave");
    ClearRoom();
}

void GiveHost(int playerId) { Send("host", { { "id", playerId } }); }
void Kick(int playerId) { Send("kick", { { "id", playerId } }); }

// Out of the room (left or kicked): forget its state. A song in progress goes on as a solo play.
static void ClearRoom()
{
    s.inRoom = false;
    s.players.clear();
    s.pick.reset();
    s.inRound = s.startPending = false;
    s.hover = s.focus = -1;
    s.hoverTitle.clear();
    s.hoverDifficulty = s.hoverEdition = -1;
    s_hostStartSent = false;
    s_haveReported.clear();
    s_hoverSent = -2;
    DisarmBarrier();
}

void SendChat(const std::string& text)
{
    if (!text.empty())
        Send("chat", { { "text", text } });
}

// ---- incoming ----

static void OnRoom(const json& d)
{
    const int prevHover = s.hover, prevDiff = s.hoverDifficulty, prevEdition = s.hoverEdition;
    s.inRoom = true;
    s.roomName = d.value("name", "");
    s.roomState = d.value("state", "lobby");
    s.hostId = d.value("host", -1);
    s.players.clear();
    for (const json& p : d.value("players", json::array()))
        s.players.push_back({ p.value("id", 0), p.value("name", ""), p.value("ready", false), p.value("playing", false),
            p.value("loaded", false), strtoull(p.value("steam", "").c_str(), nullptr, 10), p.value("avatar", false),
            p.value("missing", false), p.value("away", false), p.value("wins", 0) });
    s.pick.reset();
    if (d.contains("pick") && d["pick"].is_object()) {
        const json& p = d["pick"];
        s.pick = Pick{ p.value("pv", 0), p.value("difficulty", 0), p.value("edition", 0), p.value("title", "") };
    }
    s.hover = d.value("hover", -1);
    s.hoverTitle = d.value("hoverTitle", "");
    s.hoverDifficulty = d.value("hoverDifficulty", -1);
    s.hoverEdition = d.value("hoverEdition", -1);
    s.focus = d.value("focus", -1);
    s.rounds = d.value("rounds", 0);
    ReportHave();

    // The host browses song select: play the game's own cursor and difficulty sounds, so guests hear it happen.
    // Only guests in the lobby and not in a song, and not faster than a few times a second.
    static DWORD s_lastBrowseSound = 0;
    if (!IsHost() && GetConfig().browseSounds && s.roomState == "lobby" && !s.inRound && !g_session.inSong && s.hover >= 0
        && GetTickCount() - s_lastBrowseSound > 60) {
        if (s.hover != prevHover && prevHover >= 0) { // not for the first highlight we hear about (joining, a new round)
            launch::PlayGameSound("se_ft_music_selector_select_01");
            s_lastBrowseSound = GetTickCount();
        } else if (s.hover == prevHover && s.hoverDifficulty >= 0 && (s.hoverDifficulty != prevDiff || s.hoverEdition != prevEdition)) {
            launch::PlayGameSound("se_ft_music_selector_difficulty_change_01");
            s_lastBrowseSound = GetTickCount();
        }
    }
}

// Tells the room whether we have the song in focus, once per song (and chart, for the pick).
static void ReportHave()
{
    if (s.focus < 0)
        return;
    bool isPick = s.pick && s.pick->pv == s.focus;
    std::string key = std::to_string(s.focus) + (isPick ? "/" + std::to_string(s.pick->difficulty) + "/" + std::to_string(s.pick->edition) : "");
    if (key == s_haveReported)
        return;
    s_haveReported = key;
    bool ok = isPick ? songs::Has(s.focus, s.pick->difficulty, s.pick->edition) : songs::Has(s.focus);
    Send("have", { { "pv", s.focus }, { "ok", ok } });
    if (!ok)
        Log("mp: we don't have pv %d (%s)", s.focus, key.c_str());
}

static void OnStart(const json& d)
{
    std::vector<uint8_t> bytes;
    if (!Base64Decode(d.value("block", ""), bytes) || bytes.size() != sizeof(launch::Block::bytes)) {
        AddChat(ChatLine::Error, "", "Received an invalid song from the server.");
        return;
    }
    s.scores.clear();
    s.scoresAt = 0;
    s.showResults = false;
    s_finishSent = false;
    s.finishedSelf = false;
    s_lastScoreSent = 0;

    if (s_hostStartSent) {
        // Echo of our own start: the host's song is already loading.
        s_hostStartSent = false;
        s.inRound = true;
        return; // the barrier was armed when we sent the start
    }
    memcpy(s_startBlock.bytes, bytes.data(), bytes.size());
    // Starting a song we don't have would hang or crash the game: sit this round out instead.
    if (!songs::Has(s_startBlock.PvId(), s_startBlock.Difficulty(), s_startBlock.Edition())) {
        Send("have", { { "pv", s_startBlock.PvId() }, { "ok", false } });
        AddChat(ChatLine::System, "", "You don't have this song (or this chart), so you sit this round out.");
        Log("mp: start for pv %d diff %d, which we don't have: not playing", s_startBlock.PvId(), s_startBlock.Difficulty());
        return;
    }
    ArmBarrier();
    s.startPending = true;
    s_startReceivedAt = GetTickCount();
    Log("mp: start received, pv=%d diff=%d", s_startBlock.PvId(), s_startBlock.Difficulty());
}

static void OnScores(const json& d)
{
    s.scoresAt = GetTickCount();
    s.scores.clear();
    for (const json& p : d.value("players", json::array()))
        s.scores.push_back({ p.value("id", 0), p.value("name", ""), p.value("score", 0), p.value("combo", 0),
            p.value("completion", 0.0), p.value("finished", false), p.value("failed", false), p.value("loading", false),
            p.value("lag", false) });
}

static void OnResults(const json& d)
{
    s.results.clear();
    for (const json& p : d.value("players", json::array())) {
        const json& r = p.value("result", json::object());
        FinalResult f;
        f.id = p.value("id", 0);
        f.place = p.value("place", 0);
        f.name = p.value("name", "");
        f.score = r.value("score", 0);
        f.grade = r.value("grade", 0);
        f.maxCombo = r.value("maxCombo", 0);
        f.cool = r.value("cool", 0);
        f.fine = r.value("fine", 0);
        f.safe = r.value("safe", 0);
        f.sad = r.value("sad", 0);
        f.worst = r.value("worst", 0);
        f.completion = r.value("completion", 0.0);
        f.failed = r.value("failed", false);
        s.results.push_back(std::move(f));
    }
    s.showResults = true;
    s.inRound = s.startPending = false;
    DisarmBarrier();
}

static void Handle(const std::string& raw)
{
    json msg = json::parse(raw, nullptr, false);
    if (msg.is_discarded() || !msg.is_object())
        return;
    std::string type = msg.value("type", "");
    const json& d = msg.contains("data") && msg["data"].is_object() ? msg["data"] : json::object();

    if (type == "welcome") {
        int id = d.value("id", -1);
        s_resumeToken = d.value("token", "");
        s.serverVersion = d.value("server", "");
        if (s.reconnecting) {
            s.reconnecting = false;
            s.connLostAt = 0;
            if (id == s.myId) {
                Log("mp: reconnected, resumed as player %d", id);
                AddChat(ChatLine::System, "", "Reconnected.");
                // Things we said while the socket was dead are gone; the server ignores repeats.
                if (s_finishSent && s.inRound && !s_finishResult.is_null())
                    Send("finish", { { "result", s_finishResult } });
                if (s_loadedSent && !s_goReceived)
                    Send("loaded");
                return;
            }
            // The server no longer holds our place (too long, or it restarted): back into the room as a new player.
            Log("mp: reconnected as a new player %d, rejoining %s", id, s.roomName.c_str());
            std::string room = s.roomName;
            s.myId = id;
            ClearRoom();
            JoinRoom(room);
            return;
        }
        s.myId = id;
        s.conn = Conn::Online;
        Log("mp: online as player %d", s.myId);
        if (!s_joinOnWelcome.empty())
            JoinRoom(s_joinOnWelcome);
    } else if (type == "room") {
        OnRoom(d);
    } else if (type == "rooms") {
        s.rooms.clear();
        if (d.contains("rooms") && d["rooms"].is_array())
            for (const json& r : d["rooms"])
                if (r.is_object())
                    s.rooms.push_back({ r.value("name", ""), r.value("host", ""), r.value("players", 0), r.value("playing", false) });
    } else if (type == "chat") {
        AddChat(d.value("system", false) ? ChatLine::System : ChatLine::Player, d.value("name", ""), d.value("text", ""));
    } else if (type == "start") {
        OnStart(d);
    } else if (type == "scores") {
        OnScores(d);
    } else if (type == "results") {
        OnResults(d);
    } else if (type == "countdown") {
        // 3, 2, 1 before `go` (0.5 s apart). Only drawn and heard by players who are in the round.
        s.countdown = d.value("n", 0);
        s.countdownAt = GetTickCount();
        if (s.inRound && s.countdown > 0 && GetConfig().countdownSound) {
            // 3 and 2: the song cursor sound; 1: the sound of entering the song.
            if (s.countdown == 1)
                launch::PlayStartSound();
            else
                launch::PlayGameSound("se_ft_music_selector_select_01");
        }
    } else if (type == "go") {
        s.countdown = 0;
        s_goReceived = true;
        Log("mp: go");
    } else if (type == "kicked") {
        Log("mp: kicked from room %s", d.value("room", "").c_str());
        ClearRoom();
        s.roomNotice = "The host removed you from the room.";
    } else if (type == "error") {
        std::string message = d.value("message", "error");
        if (s_hostStartSent) {
            // The server refused our round start (e.g. someone wasn't ready): the host plays solo.
            s_hostStartSent = false;
            DisarmBarrier();
            message = "Round not started, playing solo: " + message;
        }
        if (s.conn != Conn::Online) {
            // Refused before welcome (old version, no name): back to the connect screen with the reason.
            s_ws.Close();
            s = State{};
            s.connError = "The server refused to connect: " + message;
            return;
        }
        if (!s.inRoom)
            s.roomNotice = message; // e.g. a full room: shown where the room is joined (chat isn't on screen)
        AddChat(ChatLine::Error, "", message);
    }
}

// ---- game side ----

static json ScoreJson()
{
    const GameState& g = g_session.state;
    return {
        { "score", g.liveScore },
        { "combo", g.liveCombo },
        { "completion", g.completion },
    };
}

static void SendFinish(bool fromResultScreen)
{
    if (s_finishSent)
        return;
    s_finishSent = true;
    s.finishedSelf = true;

    const GameState& g = g_session.state;
    json result;
    if (fromResultScreen && g.resultOk) {
        result = {
            { "score", g.score }, { "combo", g.maxCombo }, { "completion", g.resultCompletion },
            { "cool", g.cool }, { "fine", g.fine }, { "safe", g.safe }, { "sad", g.sad }, { "worst", g.worst },
            { "grade", g.grade }, { "maxCombo", g.maxCombo }, { "failed", g.grade == (int32_t)FinalGrade::Failed },
        };
    } else {
        result = ScoreJson();
        result["grade"] = 0;
        result["maxCombo"] = 0;
        result["failed"] = true;
    }
    s_finishResult = result;
    Send("finish", { { "result", result } });
    Log("mp: finish sent (%s)", fromResultScreen ? "result screen" : "quit");
}

void OnGameEvent(const GameEvent& e)
{
    if (s.conn != Conn::Online || !s.inRoom)
        return;

    switch (e.type) {
    case GameEventType::PvGameInit: {
        // Anyone but the host starting a song on their own plays it solo: the room never hears of it.
        if (!e.overridden && !IsHost() && !s.inRound && !s.startPending)
            AddChat(ChatLine::System, "", "You started a song on your own: it's a solo play, not part of the room's round.");
        // The host started a song in their own game: that launch is the round start.
        if (e.overridden || !IsHost() || s.roomState != "lobby")
            break;
        launch::Block block;
        if (!launch::LastLaunch(block))
            break;
        Send("start", {
            { "block", Base64Encode(block.bytes, sizeof(block.bytes)) },
            { "pv", block.PvId() }, { "difficulty", block.Difficulty() }, { "edition", block.Edition() },
            { "title", titles::Find(block.PvId()) },
        });
        s_titleSent = !titles::Find(block.PvId()).empty();
        s_hostStartSent = true;
        ArmBarrier(); // our own load takes seconds, so the hold is in place long before step 3
        Log("mp: host launched pv=%d, starting round", block.PvId());
        break;
    }
    case GameEventType::ResultShown:
        if (s.inRound)
            SendFinish(true);
        break;
    case GameEventType::SongEnded:
        if (s.inRound)
            SendFinish(false); // quit before the result screen (no-op if already sent)
        break;
    default:
        break;
    }
}

void Tick()
{
    // Off unless the checks below find us playing a round (also when offline).
    pause::SetGuardActive(false);

    songs::Tick();

    // Miku Bot keeps us company in one room only.
    if (!s.inRoom && host::BotRunning())
        host::StopBot();

    // Host: share the song highlighted in song select, so everyone can check they have it (rate-limited
    // while scrolling; the last highlight always goes out).
    if (s.conn == Conn::Online && s.inRoom && IsHost() && s.roomState == "lobby") {
        int hovered = songs::Hovered();
        // The chart selected with left/right counts as part of the highlight: a change of difficulty alone is sent.
        int32_t diff = -1, edition = -1;
        if (hovered >= 0 && !songs::SelectedChart(diff, edition))
            diff = edition = -1;
        int hoverKey = hovered < 0 ? -1 : hovered * 16 + (diff + 1) * 2 + (edition > 0 ? 1 : 0);
        if (hoverKey != s_hoverSent && GetTickCount() - s_hoverSentAt >= HoverIntervalMs) {
            json msg = { { "pv", hovered }, { "title", hovered >= 0 ? titles::Find(hovered) : "" } };
            if (diff >= 0) {
                msg["difficulty"] = diff;
                msg["edition"] = edition;
            }
            Send("hover", std::move(msg));
            s_hoverSent = hoverKey;
            s_hoverSentAt = GetTickCount();
        }
    } else {
        s_hoverSent = -2; // becoming host again sends the current highlight
    }

    // Connection status changes. A connect that fails within a frame goes Disconnected -> Connecting ->
    // Disconnected between two ticks, so a disconnected socket while we think we're connecting counts too.
    net::WsClient::Status status = s_ws.GetStatus();
    if (s.reconnecting && status == net::WsClient::Status::Disconnected && s_lastStatus == status
        && (int)(GetTickCount() - s_nextAttempt) >= 0) {
        Log("mp: reconnecting (attempt %d)", s_attempts + 1);
        s_ws.Connect(Widen(s_endpoint.host), (uint16_t)s_endpoint.port, Widen(s_endpoint.path), s_endpoint.secure);
        s_lastStatus = net::WsClient::Status::Connecting;
        status = s_ws.GetStatus();
    }
    if (status == net::WsClient::Status::Disconnected && s.conn == Conn::Connecting)
        s_lastStatus = net::WsClient::Status::Connecting;
    if (status != s_lastStatus) {
        if (status == net::WsClient::Status::Connected) {
            uint64_t steamId = steam::MySteamId();
            json hello = { { "name", GetConfig().name }, { "version", ProtocolVersion },
                { "steam", steamId ? std::to_string(steamId) : "" },
                { "resumable", true } }; // we reconnect with the token: the server may hold our slot after a drop
            if (const std::string& avatar = ui::OwnAvatarBase64(); !avatar.empty())
                hello["avatar"] = avatar; // avatar.png from the mod folder, for players without (or instead of) Steam's
            if (s.reconnecting)
                hello["resume"] = s_resumeToken;
            Send("hello", std::move(hello));
        } else if (status == net::WsClient::Status::Disconnected && s.conn != Conn::Offline) {
            std::string err = s_ws.LastError();
            bool wasOnline = s.conn == Conn::Online;
            std::string room = s.inRoom ? s.roomName : std::string();
            Log("mp: disconnected (%s)", err.empty() ? "closed" : err.c_str());
            DWORD now = GetTickCount();
            if (s.reconnecting && (int)(s_reconnectUntil - now) > 0) {
                s_nextAttempt = now + std::min<DWORD>(1000u << std::min(s_attempts++, 3), 8000); // 1, 2, 4, 8, 8... s
            } else if (!s.reconnecting && wasOnline && s.inRoom && !s_resumeToken.empty()) {
                // Keep the room and the round as they are and try to get our place back.
                s.reconnecting = true;
                s.connLostAt = now;
                s_reconnectUntil = now + ReconnectWindowMs;
                s_attempts = 0;
                s_nextAttempt = now + 1000;
                AddChat(ChatLine::System, "", "Connection lost, reconnecting...");
            } else {
                s_resumeToken.clear();
                s = State{};
                if (wasOnline) {
                    s.connError = "Connection to the server was lost.";
                    s.rejoinRoom = room;
                    s.connLostAt = GetTickCount();
                } else {
                    s.connError = "Couldn't reach the server. Check the address, or ask the host if their server is running.";
                }
            }
        }
        s_lastStatus = status;
    }

    // "Host a server": join our own server locally (no tunnel round trip) once it's up.
    // The server is confirmed running once hosting moves past StartingServer; no need to wait for the tunnel.
    if (s_connectWhenHosted) {
        host::Phase phase = host::GetStatus().phase;
        if (phase == host::Phase::Downloading || phase == host::Phase::StartingTunnel || phase == host::Phase::Ready) {
            s_connectWhenHosted = false;
            if (s.conn == Conn::Offline)
                ConnectTo("127.0.0.1", host::LocalPort);
        } else if (phase == host::Phase::Failed || phase == host::Phase::Off) {
            s_connectWhenHosted = false;
        }
    }

    std::vector<std::string> inbox;
    s_ws.Poll(inbox);
    for (const std::string& m : inbox)
        Handle(m);

    if (s.conn != Conn::Online)
        return;

    if (!launch::InSongSelect())
        s_songSelectSince = 0;
    else if (!s_songSelectSince)
        s_songSelectSince = GetTickCount() | 1;

    // Auto-start once the player is on song select (and it has settled).
    if (s.startPending) {
        if (s_songSelectSince) {
            // The highlight comes from the song select watch; don't depend on it forever if that watch failed.
            DWORD up = GetTickCount() - s_songSelectSince;
            bool settled = up >= SongSelectSettleMs && (songs::Hovered() >= 0 || up >= 3 * SongSelectSettleMs);
            if (settled && launch::StartNow(s_startBlock, GetConfig().ownOutfits)) {
                s.startPending = false;
                s.inRound = true;
                Log("mp: auto-started pv=%d", s_startBlock.PvId());
            }
        } else if (GetTickCount() - s_startReceivedAt > StartTimeoutMs) {
            s.startPending = false;
            DisarmBarrier();
            AddChat(ChatLine::Error, "", "Missed the round start: be on the song select screen when the host starts.");
            Send("finish", { { "result", { { "score", 0 }, { "failed", true } } } });
        }
    }

    // Host: share the song title once our game reveals it (first time this PV is played here).
    if (s.inRound && IsHost() && !s_titleSent && s.pick) {
        std::string title = titles::Find(s.pick->pv);
        if (!title.empty()) {
            Send("title", { { "title", title } });
            s_titleSent = true;
        }
    }

    // Loading barrier.
    if (s.inRound) {
        if (sync::IsHolding()) {
            if (!s_loadedSent) {
                s_loadedSent = true;
                s_holdSince = GetTickCount();
                Send("loaded");
                Log("mp: loaded, waiting for the others");
            }
            if (s_goReceived) {
                DisarmBarrier();
                Log("mp: released");
            } else if (GetTickCount() - s_holdSince > HoldSafetyMs) {
                DisarmBarrier();
                AddChat(ChatLine::Error, "", "The server never started the round, starting on our own.");
            }
        } else if (s_goReceived && sync::HoldStep() >= 0) {
            // `go` came before we finished loading (someone timed out): don't hold at all.
            DisarmBarrier();
        }
    }

    // No casual pausing while the song plays in a round; the pause menu itself stays usable.
    pause::SetGuardActive(s.inRound && g_session.inSong && sync::ReadState().outer == 1 && !pause::GetState().paused);

    // Live score reporting.
    if (s.inRound && g_session.inSong && !s_finishSent && GetTickCount() - s_lastScoreSent >= ScoreIntervalMs) {
        s_lastScoreSent = GetTickCount();
        Send("score", ScoreJson());
    }
}

} // namespace md::mp
