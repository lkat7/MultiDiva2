#include "multiplayer_ui.h"
#include "avatars.h"
#include "bug_report.h"
#include "game_sprites.h"
#include "glyphs.h"
#include "input_block.h"
#include "leaderboard.h"
#include "pause_hint.h"
#include "room_scene.h"
#include "overlay.h"
#include "../core/config.h"
#include "../core/version.h"
#include "../game/launch.h"
#include "../game/memory.h"
#include "../game/pause_guard.h"
#include "../game/session.h"
#include "../game/songs.h"
#include "../game/steam.h"
#include "../game/sync_start.h"
#include "../game/titles.h"
#include "../mp/multiplayer.h"
#include "../net/host.h"
#include "../net/report.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <windows.h>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <map>

namespace md::ui {

static const ImVec4 Accent(0.22f, 0.77f, 0.73f, 1.0f); // Miku teal
static const ImVec4 Dim(0.62f, 0.62f, 0.66f, 1.0f);
static const ImVec4 Bad(1.0f, 0.42f, 0.42f, 1.0f);
static const ImVec4 Gold(1.0f, 0.82f, 0.30f, 1.0f);

static const char* DifficultyName(int d)
{
    static const char* names[] = { "Easy", "Normal", "Hard", "Extreme", "Ex Extreme" };
    return d >= 0 && d < 5 ? names[d] : "?";
}

static const char* GradeName(int g)
{
    static const char* names[] = { "Not Clear", "Cheap", "Standard", "Great", "Excellent", "Perfect" };
    return g >= 0 && g < 6 ? names[g] : "?";
}

// The game truncates percentages to 2 decimals instead of rounding (98.949 shows as 98.94).
static double TruncatePercent(double p)
{
    return std::floor(p * 100.0 + 1e-6) / 100.0;
}

// "Roshin Yukai (Extreme)": the server's title, else our own cache, else the PV id.
static std::string PickName(const mp::Pick& p)
{
    std::string title = !p.title.empty() ? p.title : titles::Describe(p.pv);
    return title + " (" + (p.edition ? "Extra " : "") + DifficultyName(p.difficulty) + ")";
}

// "Extreme  9.5   BPM 160" for the pick, with the difficulty's colour; the level and tempo only when the game's
// database gave a sane value (songs::Level / songs::Bpm).
static std::string PickDetail(const mp::Pick& p, ImVec4& color)
{
    int d = p.edition ? 4 : p.difficulty;
    color = ImGui::ColorConvertU32ToFloat4(DifficultyColor(d));
    std::string text = DifficultyName(d);
    float level = songs::Level(p.pv, p.difficulty, p.edition);
    if (level > 0) {
        char lv[24];
        snprintf(lv, sizeof(lv), "  %.1f", level);
        text += lv;
    }
    int bpm = songs::Bpm(p.pv);
    if (bpm > 0)
        text += "   BPM " + std::to_string(bpm);
    return text;
}

// The charts a song has, by difficulty 0..3 and the extra chart (4): -1 = none, 0 = exists (level unknown), else stars.
// Which charts exist comes from the game's song database like the "missing song" check does; levels need songs::Level.
static std::array<float, 5> SongCharts(int32_t pv)
{
    static std::map<int32_t, std::array<float, 5>> cache; // the song database doesn't change while the game runs
    auto it = cache.find(pv);
    if (it != cache.end())
        return it->second;
    std::array<float, 5> out = { -1, -1, -1, -1, -1 };
    for (int i = 0; i < 5; i++) {
        int diff = i < 4 ? i : 3, edition = i < 4 ? 0 : 1;
        if (!songs::Has(pv, diff, edition))
            continue;
        float level = songs::Level(pv, diff, edition);
        if (level == 0)
            continue; // an empty slot (the game greys it out): the song doesn't have this chart
        out[i] = level > 0 ? level : 0;
    }
    return cache[pv] = out;
}

// The song's cover (the game's own jacket, loaded on demand) as a square of `size` pixels at the cursor, or a plain
// grey square while it isn't loaded or the song isn't installed. Takes the square's place in the layout.
static void DrawJacket(int32_t pv, float size)
{
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size, a.y + size);
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (const sprites::Sprite* jacket = pv >= 0 ? sprites::Jacket(pv) : nullptr)
        sprites::Draw(dl, *jacket, a, b);
    else
        dl->AddRectFilled(a, b, IM_COL32(60, 62, 70, 255), 4);
}

static void InputString(const char* label, std::string& value, size_t max)
{
    char buf[128];
    strncpy_s(buf, value.c_str(), _TRUNCATE);
    if (ImGui::InputText(label, buf, max < sizeof(buf) ? max : sizeof(buf)))
        value = buf;
}

static void DrawHostingBar();

// ---- lobby auto-fit ----
// The lobby sizes itself to its content. Items that stretch to the window's width (tables, the chat, -1
// widths) would make the measured content as wide as the window, so they are drawn inside a Stretchy scope
// that leaves them out and states what they really need instead.
namespace fit {

// Restores the window's content extent after stretching items, then adds what they need at minimum.
struct Stretchy {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImVec2 savedMax = window->DC.CursorMaxPos;
    float startY = window->DC.CursorPos.y;
    float needWidth, needHeight; // < 0: measure that axis normally
    Stretchy(float width, float height = -1) : needWidth(width), needHeight(height) {}
    ~Stretchy()
    {
        ImVec2& max = window->DC.CursorMaxPos;
        float left = window->DC.CursorStartPos.x;
        max.x = needWidth < 0 ? max.x : ImMax(savedMax.x, left + needWidth);
        max.y = needHeight < 0 ? max.y : ImMax(savedMax.y, startY + needHeight);
    }
};

// Outer size the content wants, measured at the end of the window.
ImVec2 Measure()
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImVec2 content(window->DC.CursorMaxPos.x - window->DC.CursorStartPos.x, window->DC.CursorMaxPos.y - window->DC.CursorStartPos.y);
    ImVec2 pad = ImGui::GetStyle().WindowPadding;
    return ImVec2(std::ceil(content.x + pad.x * 2), std::ceil(content.y + pad.y * 2 + window->TitleBarHeight()));
}

} // namespace fit

static void DrawConnect(const mp::State& s)
{
    Config& cfg = GetConfig();
    bool connecting = s.conn == mp::Conn::Connecting;
    if (connecting)
        ImGui::BeginDisabled();

    ImGui::SetNextItemWidth(320);
    InputString("Name", cfg.name, 25);
    ImGui::SetNextItemWidth(320);
    InputString("Server", cfg.server, 128);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An IP or host name, or a full address like wss://name.trycloudflare.com");
    // A ws:// or wss:// address carries its own port.
    bool url = cfg.server.rfind("ws://", 0) == 0 || cfg.server.rfind("wss://", 0) == 0;
    if (!url) {
        ImGui::SetNextItemWidth(320);
        ImGui::InputInt("Port", &cfg.port, 0);
    }
    ImGui::SetNextItemWidth(320);
    InputString("Room", cfg.room, 33);

    if (ImGui::Button(connecting ? "Connecting..." : "Connect", ImVec2(320, 0)) && !cfg.name.empty()) {
        SaveConfig();
        mp::Connect();
    }
    if (connecting)
        ImGui::EndDisabled();

    if (!s.connError.empty())
        ImGui::TextColored(Bad, "%s", s.connError.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(Accent, "Host a game");
    host::Status hs = host::GetStatus();
    if (hs.phase == host::Phase::Off || hs.phase == host::Phase::Failed) {
        ImGui::TextColored(Dim, "Runs a server on this PC with a free Cloudflare tunnel, so friends\n"
                                "can join from anywhere. The first time it downloads cloudflared (~60 MB).");
        if (hs.phase == host::Phase::Failed)
            ImGui::TextColored(Bad, "%s", hs.message.c_str());
        bool permanent = !cfg.tunnelToken.empty() && !cfg.tunnelUrl.empty();
        if (ImGui::Button(permanent ? "Host on my permanent address" : "Host a server", ImVec2(320, 0)) && !cfg.name.empty()) {
            SaveConfig();
            mp::HostAndConnect();
        }

        // Optional fixed address through the user's own Cloudflare tunnel.
        if (ImGui::TreeNode("Permanent address (optional, needs a Cloudflare tunnel)")) {
            ImGui::TextColored(Dim, "Without these, every hosting session gets a new random address.");
            ImGui::SetNextItemWidth(320);
            char url[256];
            strncpy_s(url, cfg.tunnelUrl.c_str(), _TRUNCATE);
            if (ImGui::InputTextWithHint("Address", "wss://multidiva.example.com", url, sizeof(url)))
                cfg.tunnelUrl = url;
            if (ImGui::IsItemDeactivatedAfterEdit())
                SaveConfig();
            ImGui::SetNextItemWidth(320);
            char token[512];
            strncpy_s(token, cfg.tunnelToken.c_str(), _TRUNCATE);
            if (ImGui::InputTextWithHint("Tunnel token", "paste from the Cloudflare dashboard", token, sizeof(token),
                    ImGuiInputTextFlags_Password))
                cfg.tunnelToken = token;
            if (ImGui::IsItemDeactivatedAfterEdit())
                SaveConfig();
            ImGui::TextColored(Dim, "The token stays on this PC (multidiva.ini). Don't share it.");
            ImGui::TreePop();
        }
    } else {
        DrawHostingBar();
    }
}

// Status of our hosted server; the public address with a Copy button once the tunnel is up.
static void DrawHostingBar()
{
    host::Status hs = host::GetStatus();
    if (hs.phase == host::Phase::Off)
        return;
    if (hs.phase == host::Phase::Ready) {
        // The address is long and only needs copying: shown on hover, so it never stretches the window.
        ImGui::TextColored(Dim, "Hosting: friends join with your invite address.");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", hs.publicUrl.c_str());
        ImGui::SameLine();
        static DWORD s_copiedAt = 0;
        if (ImGui::SmallButton(GetTickCount() - s_copiedAt < 1500 ? "Copied!" : "Copy invite")) {
            ImGui::SetClipboardText(hs.publicUrl.c_str());
            s_copiedAt = GetTickCount();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop hosting"))
            host::Stop();
    } else if (hs.phase == host::Phase::Failed) {
        ImGui::TextColored(Bad, "%s", hs.message.c_str());
    } else {
        ImGui::TextColored(Dim, "%s", hs.message.c_str());
        if (hs.phase == host::Phase::Downloading) {
            fit::Stretchy stretchy(240);
            ImGui::ProgressBar(hs.progress, ImVec2(-1, 0));
        }
    }
}

// A player's picture at `size`, or their initial on a colored square when they have none.
static void DrawAvatar(const mp::Player* p, const std::string& name, float size)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 b(a.x + size, a.y + size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ID3D11ShaderResourceView* tex = p ? PlayerAvatar(*p) : nullptr) {
        dl->AddImageRounded((ImTextureID)tex, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, size * 0.18f);
    } else {
        dl->AddRectFilled(a, b, IM_COL32(70, 90, 110, 255), size * 0.18f);
        char initial[2] = { name.empty() ? '?' : (char)toupper((unsigned char)name[0]), 0 };
        ImVec2 sz = ImGui::CalcTextSize(initial);
        dl->AddText(ImVec2(a.x + (size - sz.x) * 0.5f, a.y + (size - sz.y) * 0.5f), IM_COL32_WHITE, initial);
    }
    ImGui::Dummy(ImVec2(size, size));
}

static const mp::Player* FindPlayer(const mp::State& s, int id)
{
    for (const mp::Player& p : s.players)
        if (p.id == id)
            return &p;
    return nullptr;
}

static void DrawPlayers(const mp::State& s)
{
    const bool roundRunning = s.roomState == "playing";
    const float avatar = ImGui::GetFrameHeight();
    const ImGuiStyle& style = ImGui::GetStyle();
    // The table stretches to the window: it needs its widest name plus the two fixed columns.
    float widest = ImGui::CalcTextSize("song in progress").x;
    for (const mp::Player& p : s.players)
        widest = ImMax(widest, ImGui::CalcTextSize((p.name + " (you)").c_str()).x);
    fit::Stretchy stretchy(avatar + style.ItemSpacing.x + widest + 60 + 120 + style.CellPadding.x * 6);
    if (!ImGui::BeginTable("players", 3, ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("role", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 120);
    for (const mp::Player& p : s.players) {
        ImGui::PushID(p.id);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawAvatar(&p, p.name, avatar);
        ImGui::SameLine();
        // The whole row is a selectable so the host can right-click anyone else.
        std::string label = p.id == s.myId ? p.name + " (you)" : p.name;
        ImGui::PushStyleColor(ImGuiCol_Text, p.id == s.myId ? Accent : ImVec4(1, 1, 1, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
        ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, avatar));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (mp::IsHost() && p.id != s.myId && ImGui::BeginPopupContextItem("player")) {
            ImGui::TextColored(Dim, "%s", p.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Give host"))
                mp::GiveHost(p.id);
            if (ImGui::MenuItem("Kick"))
                mp::Kick(p.id);
            ImGui::EndPopup();
        }
        if (mp::IsHost() && p.id != s.myId && ImGui::IsItemHovered() && !ImGui::IsPopupOpen("player"))
            ImGui::SetTooltip("Right-click: give host, kick");
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        if (p.id == s.hostId)
            ImGui::TextColored(Gold, "host");
        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        if (p.away)
            ImGui::TextColored(Gold, "reconnecting");
        else if (p.missing && !p.playing)
            ImGui::TextColored(Bad, "no song");
        else if (p.playing)
            ImGui::TextColored(Accent, "playing");
        else if (roundRunning)
            ImGui::TextColored(Dim, "song in progress"); // joined mid-round: plays the next one
        else if (p.id == s.hostId || p.ready)
            ImGui::TextColored(Accent, "ready");
        else
            ImGui::TextColored(Dim, "not ready");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void DrawRoundStatus(const mp::State& s)
{
    if (s.startPending) {
        ImGui::TextColored(Gold, "Round starting! Go to the song select screen.");
        return;
    }
    if (s.roomState == "playing") {
        const mp::Player* me = mp::Me();
        std::string song = s.pick ? PickName(*s.pick) : "a song";
        if (me && me->playing)
            ImGui::TextColored(Accent, "Round in progress: %s", song.c_str());
        else if (me && me->missing)
            ImGui::TextColored(Bad, "The others are playing %s, which you don't have. You'll join the next round.", song.c_str());
        else
            ImGui::TextColored(Dim, "The others are playing %s: you'll join the next round.", song.c_str());
        return;
    }

    // What the host is looking at in song select, and who doesn't have it.
    if (s.hover >= 0) {
        // Our own song list's name, else the host's (when we don't have the song), else "PV n".
        std::string title = titles::Find(s.hover);
        if (title.empty())
            title = !s.hoverTitle.empty() ? s.hoverTitle : titles::Describe(s.hover);
        // The cover on the left, the song's lines to its right.
        DrawJacket(s.hover, ImGui::GetFrameHeightWithSpacing() * 3);
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (mp::IsHost())
            ImGui::TextColored(Dim, "Highlighted: %s", title.c_str());
        else
            ImGui::TextColored(Dim, "The host is looking at: %s", title.c_str());
        // Its charts, each in its difficulty's colour (with the level when the game's database gives it).
        static const char* names[] = { "Easy", "Normal", "Hard", "Extreme", "Extra" };
        std::array<float, 5> charts = SongCharts(s.hover);
        int selected = s.hoverDifficulty < 0 ? -1 : s.hoverEdition == 1 ? 4 : s.hoverDifficulty;
        for (int i = 0; i < 5; i++) {
            if (charts[i] < 0)
                continue;
            // Only the selected chart keeps its colour; the others are grey (all coloured while the selection is unknown).
            ImVec4 color = selected < 0 || i == selected ? ImGui::ColorConvertU32ToFloat4(DifficultyColor(i)) : Dim;
            const char* open = i == selected ? "[" : "", *close = i == selected ? "]" : ""; // the host's selected chart
            if (charts[i] > 0)
                ImGui::TextColored(color, "%s%s %.1f%s", open, names[i], charts[i], close);
            else
                ImGui::TextColored(color, "%s%s%s", open, names[i], close);
            ImGui::SameLine();
        }
        int bpm = songs::Bpm(s.hover);
        if (bpm > 0)
            ImGui::TextColored(Dim, "BPM %d", bpm);
        else
            ImGui::NewLine();
        ImGui::EndGroup();
    }
    std::string missing;
    int missingCount = 0;
    for (const mp::Player& p : s.players)
        if (p.missing) {
            missing += (missingCount++ ? ", " : "") + p.name;
        }
    if (missingCount)
        ImGui::TextColored(Bad, "%s %s this song and would sit it out.", missing.c_str(),
            missingCount == 1 ? "doesn't have" : "don't have");

    int notReady = 0;
    for (const mp::Player& p : s.players)
        if (p.id != s.hostId && !p.ready && !p.missing)
            notReady++;

    if (mp::IsHost()) {
        if (notReady == 0)
            ImGui::TextColored(Accent, "Everyone is ready: start any song in song select to start the round.");
        else
            ImGui::TextColored(Dim, "Waiting for %d player%s to get ready.", notReady, notReady == 1 ? "" : "s");

        // No one to play with: a bot player that joins this room and follows your score.
        if (host::BotRunning()) {
            if (ImGui::Button("Remove Miku Bot"))
                host::StopBot();
        } else if (ImGui::Button("Add Miku Bot")) {
            host::StartBot(mp::ServerUrl(), s.roomName);
        }
        ImGui::SameLine();
        ImGui::TextColored(Dim, "A practice opponent that stays close to your score.");
    } else {
        const mp::Player* me = mp::Me();
        bool ready = me && me->ready;
        if (ImGui::Checkbox("Ready", &ready))
            mp::SetReady(ready);
        ImGui::SameLine();
        ImGui::TextColored(Dim, ready ? "Stay on song select: the song starts by itself." : "Tick when you're on song select.");
    }

    if (s.pick) {
        DrawJacket(s.pick->pv, ImGui::GetFrameHeightWithSpacing() * 2);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(Dim, "Last song: %s", PickName(*s.pick).c_str());
        ImVec4 detailColor;
        std::string detail = PickDetail(*s.pick, detailColor);
        ImGui::TextColored(detailColor, "%s", detail.c_str());
        ImGui::EndGroup();
    }
}

static void DrawChat(const mp::State& s)
{
    // The chat takes whatever the window has left; it asks for a readable minimum.
    float inputHeight = ImGui::GetFrameHeightWithSpacing();
    fit::Stretchy stretchy(360, 140 + inputHeight);
    if (ImGui::BeginChild("chat", ImVec2(0, -inputHeight), true)) {
        for (const mp::ChatLine& line : s.chat) {
            switch (line.kind) {
            case mp::ChatLine::System: ImGui::TextColored(Dim, "* %s", line.text.c_str()); break;
            case mp::ChatLine::Error: ImGui::TextColored(Bad, "! %s", line.text.c_str()); break;
            default:
                ImGui::TextColored(Accent, "%s:", line.name.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", line.text.c_str());
            }
        }
        // Stick to the bottom unless the user scrolled up.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    static char input[301];
    ImGui::SetNextItemWidth(-60);
    bool entered = ImGui::InputText("##chat", input, sizeof(input), ImGuiInputTextFlags_EnterReturnsTrue);
    // Enter closes the field; take focus back right away so several messages can be typed in a row.
    if (entered)
        ImGui::SetKeyboardFocusHere(-1);
    ImGui::SameLine();
    bool clicked = ImGui::Button("Send", ImVec2(-1, 0));
    if ((entered || clicked) && input[0]) {
        mp::SendChat(input);
        input[0] = '\0';
    }
}

// ---- the full-screen screens (F8): connect / room list, then the room (ui/room_scene) ----

static bool s_classic = true; // the ImGui window; Ctrl+F8 switches to the full-screen screens (work in progress)
static bool s_fullScreen = false; // the last DrawLobby drew a full-screen screen
static int s_screenFrame = -1;    // ImGui frame it was last drawn in
static bool s_addressSetup = false;

void SetClassicLobby(bool classic) { s_classic = classic; }
bool ClassicLobby() { return s_classic; }
bool LobbyIsFullScreen() { return s_fullScreen; }

// The round's state in one line for the room screen (what the old window's DrawRoundStatus says).
static std::string RoomStatus(const mp::State& s, RoomView::Tone& tone)
{
    tone = RoomView::Tone::Normal;
    if (s.startPending) {
        tone = RoomView::Tone::Good;
        return "Round starting! Go to song select.";
    }
    std::string song = s.pick ? PickName(*s.pick) : "a song";
    if (s.roomState == "playing") {
        const mp::Player* me = mp::Me();
        if (me && me->playing)
            return "Round in progress: " + song;
        tone = me && me->missing ? RoomView::Tone::Bad : RoomView::Tone::Normal;
        return me && me->missing ? "The others are playing " + song + ", which you don't have." :
                                   "The others are playing " + song + ": you'll join the next round.";
    }
    std::string missing;
    int missingCount = 0;
    for (const mp::Player& p : s.players)
        if (p.missing)
            missing += (missingCount++ ? ", " : "") + p.name;
    std::string title;
    if (s.hover >= 0) {
        title = titles::Find(s.hover);
        if (title.empty())
            title = !s.hoverTitle.empty() ? s.hoverTitle : titles::Describe(s.hover);
    }
    if (missingCount) {
        tone = RoomView::Tone::Bad;
        return missing + (missingCount == 1 ? " doesn't have " : " don't have ") + (title.empty() ? "this song" : title)
            + " and would sit it out.";
    }
    int notReady = 0;
    for (const mp::Player& p : s.players)
        if (p.id != s.hostId && !p.ready && !p.missing)
            notReady++;
    if (mp::IsHost()) {
        if (notReady) {
            char line[96];
            snprintf(line, sizeof(line), "Waiting for %d player%s to get ready.", notReady, notReady == 1 ? "" : "s");
            return line;
        }
        tone = RoomView::Tone::Good;
        return "Everyone is ready: pick a song and play it to start the round.";
    }
    const mp::Player* me = mp::Me();
    return me && me->ready ? "Ready! Stay on song select: the song starts by itself." : "Click Ready when you're on song select.";
}

// "wss://name.example.com:443/ws" -> "name.example.com".
static std::string ShortServer(std::string url)
{
    if (size_t p = url.find("://"); p != std::string::npos)
        url.erase(0, p + 3);
    if (size_t p = url.find_first_of(":/"); p != std::string::npos)
        url.erase(p);
    return url;
}

// The one-time permanent tunnel setup, as a small ImGui window over the screen (rarely used).
static void DrawAddressSetup()
{
    if (!s_addressSetup)
        return;
    Config& cfg = GetConfig();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Permanent address", &s_addressSetup, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::TextColored(Dim, "Optional: host on your own fixed address through a Cloudflare tunnel.\n"
                            "Without these, every hosting session gets a new random address.");
    ImGui::SetNextItemWidth(420);
    char url[256];
    strncpy_s(url, cfg.tunnelUrl.c_str(), _TRUNCATE);
    if (ImGui::InputTextWithHint("Address", "wss://multidiva.example.com", url, sizeof(url)))
        cfg.tunnelUrl = url;
    if (ImGui::IsItemDeactivatedAfterEdit())
        SaveConfig();
    ImGui::SetNextItemWidth(420);
    char token[512];
    strncpy_s(token, cfg.tunnelToken.c_str(), _TRUNCATE);
    if (ImGui::InputTextWithHint("Tunnel token", "paste from the Cloudflare dashboard", token, sizeof(token),
            ImGuiInputTextFlags_Password))
        cfg.tunnelToken = token;
    if (ImGui::IsItemDeactivatedAfterEdit())
        SaveConfig();
    ImGui::TextColored(Dim, "The token stays on this PC (multidiva.ini). Don't share it.");
    if (ImGui::Button("Done", ImVec2(120, 0))) {
        SaveConfig();
        s_addressSetup = false;
    }
    ImGui::End();
}

static void DrawScreens(bool* open)
{
    const mp::State& s = mp::Get();
    Config& cfg = GetConfig();
    // Esc hides the screen (and never reaches the game).
    WantEscape();
    if (EscapePressed() && !s_addressSetup) {
        *open = false;
        return;
    }

    if (s.inRoom) {
        RoomView view;
        view.status = RoomStatus(s, view.statusTone);
        view.botRunning = host::BotRunning();
        view.ownOutfits = cfg.ownOutfits;
        view.showLeaderboard = cfg.showLeaderboard;
        view.showGaps = cfg.showGaps;
        // The plate follows the host's highlight in song select, else shows the last song played.
        view.songPv = s.hover >= 0 ? s.hover : s.pick ? s.pick->pv : -1;
        if (s.hover >= 0) {
            view.songTitle = titles::Find(s.hover);
            if (view.songTitle.empty())
                view.songTitle = !s.hoverTitle.empty() ? s.hoverTitle : titles::Describe(s.hover);
            view.songBpm = songs::Bpm(s.hover);
            view.charts = SongCharts(s.hover);
            if (s.hoverDifficulty >= 0) { // the chart the host has selected with left/right
                view.selectedChart = s.hoverEdition == 1 ? 4 : s.hoverDifficulty;
                view.songDifficulty = view.selectedChart; // the plate's difficulty chip follows it
                view.songLevel = std::max(view.charts[view.selectedChart], -1.0f);
            }
        } else if (s.pick) {
            view.songTitle = !s.pick->title.empty() ? s.pick->title : titles::Describe(s.pick->pv);
            view.songDifficulty = s.pick->edition ? 4 : s.pick->difficulty;
            view.songLevel = songs::Level(s.pick->pv, s.pick->difficulty, s.pick->edition);
            view.songBpm = songs::Bpm(s.pick->pv);
        } else {
            view.songHint = mp::IsHost() ? "Open song select to pick a song" : "The host is picking a song...";
        }
        static DWORD s_inviteCopiedAt = 0;
        host::Status hs = host::GetStatus();
        if (hs.phase == host::Phase::Ready)
            view.inviteUrl = hs.publicUrl;
        view.inviteCopied = GetTickCount() - s_inviteCopiedAt < 1500;
        RoomAction a = DrawRoomScene(s, view);
        switch (a.kind) {
        case RoomAction::ToggleReady:
            if (const mp::Player* me = mp::Me())
                mp::SetReady(!me->ready);
            break;
        case RoomAction::PickSong:
            *open = false; // song select is behind the screen: playing a song starts the round
            break;
        case RoomAction::Leave:
            mp::LeaveRoom();
            break;
        case RoomAction::GiveHost:
            mp::GiveHost(a.player);
            break;
        case RoomAction::Kick:
            mp::Kick(a.player);
            break;
        case RoomAction::Chat:
            mp::SendChat(a.text);
            break;
        case RoomAction::ToggleBot:
            if (host::BotRunning())
                host::StopBot();
            else
                host::StartBot(mp::ServerUrl(), s.roomName);
            break;
        case RoomAction::CopyInvite:
            ImGui::SetClipboardText(view.inviteUrl.c_str());
            s_inviteCopiedAt = GetTickCount();
            break;
        case RoomAction::ToggleLeaderboard:
            cfg.showLeaderboard = !cfg.showLeaderboard;
            SaveConfig();
            break;
        case RoomAction::ToggleGaps:
            cfg.showGaps = !cfg.showGaps;
            SaveConfig();
            break;
        case RoomAction::ToggleOutfits:
            cfg.ownOutfits = !cfg.ownOutfits;
            SaveConfig();
            break;
        case RoomAction::None:
            break;
        }
        return;
    }

    static std::string s_notice;
    static DWORD s_copiedAt = 0;
    ConnectView v;
    v.name = &cfg.name;
    v.server = &cfg.server;
    v.steamId = steam::MySteamId();
    host::Status hs = host::GetStatus();
    // Hosting, we're connected to our own server locally: show the address friends use instead.
    v.serverShown = ShortServer(hs.phase == host::Phase::Ready ? hs.publicUrl : mp::ServerUrl());
    v.hostPhase = hs.phase == host::Phase::Off ? ConnectView::HostPhase::Off
        : hs.phase == host::Phase::Ready     ? ConnectView::HostPhase::Ready
        : hs.phase == host::Phase::Failed    ? ConnectView::HostPhase::Failed
                                             : ConnectView::HostPhase::Busy;
    v.hostMessage = hs.message;
    if (hs.phase == host::Phase::Downloading) {
        char pct[48];
        snprintf(pct, sizeof(pct), "  (%d%%)", (int)(hs.progress * 100));
        v.hostMessage += pct;
    }
    v.hostUrl = hs.publicUrl;
    v.permanent = !cfg.tunnelToken.empty() && !cfg.tunnelUrl.empty();
    v.copied = GetTickCount() - s_copiedAt < 1500;
    v.notice = s_notice;
    v.rejoinRoom = s.rejoinRoom;

    switch (DrawConnectScene(s, v)) {
    case ConnectAction::Save:
        SaveConfig();
        break;
    case ConnectAction::Connect:
        if (cfg.name.empty() || cfg.server.empty()) {
            s_notice = cfg.name.empty() ? "Type a name first." : "Type the server address the host gave you.";
            break;
        }
        s_notice.clear();
        SaveConfig();
        mp::Connect(""); // stay in the lobby: the server's rooms are listed
        break;
    case ConnectAction::JoinRoom:
        mp::JoinRoom(v.joinRoom);
        break;
    case ConnectAction::CreateRoom: {
        // "<name>'s room", numbered if that's taken.
        std::string base = cfg.name + "'s room", name = base;
        auto taken = [&](const std::string& n) {
            for (const mp::RoomInfo& r : s.rooms)
                if (_stricmp(r.name.c_str(), n.c_str()) == 0)
                    return true;
            return false;
        };
        for (int i = 2; taken(name); i++)
            name = base + " " + std::to_string(i);
        mp::JoinRoom(name);
        break;
    }
    case ConnectAction::Disconnect:
        mp::Disconnect();
        break;
    case ConnectAction::Host:
        if (cfg.name.empty()) {
            s_notice = "Type a name first.";
            break;
        }
        s_notice.clear();
        SaveConfig();
        mp::HostAndConnect(""); // the rooms + the address to share; Create room when ready
        break;
    case ConnectAction::StopHosting:
        host::Stop();
        break;
    case ConnectAction::CopyAddress:
        ImGui::SetClipboardText(hs.publicUrl.c_str());
        s_copiedAt = GetTickCount();
        break;
    case ConnectAction::Rejoin:
        s_notice.clear();
        mp::Rejoin();
        break;
    case ConnectAction::AddressSetup:
        s_addressSetup = true;
        break;
    case ConnectAction::None:
        break;
    }
    DrawAddressSetup();
}

void DrawLobby(bool* open)
{
    s_fullScreen = !s_classic;
    if (!s_classic) {
        s_screenFrame = ImGui::GetFrameCount();
        DrawScreens(open);
        return;
    }
    const mp::State& s = mp::Get();
    ImGui::SetNextWindowSize(ImVec2(460, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 120), ImGuiCond_FirstUseEver);

    // Fit to the content whenever what it needs changes (a new screen, hosting started, a long name...).
    // In between, a size the user dragged to stays. In a room the chat soaks up extra height, so the
    // window only grows taller there, never shrinks.
    // While the game is minimized (alt-tab) the screen size reads tiny: never fit to that, and fit again
    // once the screen is back to its real size.
    static ImVec2 s_wanted(0, 0), s_applied(0, 0), s_appliedScreen(0, 0);
    const ImVec2 screen = ImGui::GetMainViewport()->Size;
    bool screenOk = screen.x >= 640 && screen.y >= 480;
    bool changed = std::fabs(s_wanted.x - s_applied.x) > 2 || std::fabs(s_wanted.y - s_applied.y) > 2
        || screen.x != s_appliedScreen.x || screen.y != s_appliedScreen.y;
    if (const ImGuiWindow* w = ImGui::FindWindowByName("MultiDiva2 (F8)"); w && screenOk && changed && s_wanted.x > 0) {
        float height = s.inRoom ? ImMax(w->Size.y, s_wanted.y) : s_wanted.y;
        ImGui::SetNextWindowSize(ImVec2(ImMin(s_wanted.x, screen.x - 20), ImMin(height, screen.y - 20)));
        s_applied = s_wanted;
        s_appliedScreen = screen;
    }
    if (!ImGui::Begin("MultiDiva2 (F8)", open)) {
        ImGui::End();
        return;
    }

    if (s.conn != mp::Conn::Online) {
        DrawConnect(s);
    } else if (!s.inRoom) {
        ImGui::Text("Connected as %s", GetConfig().name.c_str());
        ImGui::TextColored(Dim, "Client %s   Server %s", MULTIDIVA_VERSION,
            s.serverVersion.empty() ? "unknown (older)" : s.serverVersion.c_str());
        if (!s.roomNotice.empty())
            ImGui::TextColored(Bad, "%s", s.roomNotice.c_str());
        Config& cfg = GetConfig();
        ImGui::SetNextItemWidth(220);
        InputString("Room", cfg.room, 33);
        if (ImGui::Button("Join room") && !cfg.room.empty()) {
            SaveConfig();
            mp::JoinRoom(cfg.room);
        }
        ImGui::SameLine();
        if (ImGui::Button("Disconnect"))
            mp::Disconnect();
    } else {
        DrawHostingBar();
        std::string room = "Room: " + s.roomName;
        ImGui::TextColored(Accent, "%s", room.c_str());
        // Right-align the two buttons (they need the room name's width plus their own).
        const ImGuiStyle& style = ImGui::GetStyle();
        float buttons = ImGui::CalcTextSize("Leave").x + ImGui::CalcTextSize("Disconnect").x
            + style.FramePadding.x * 4 + style.ItemSpacing.x;
        float textW = ImGui::CalcTextSize(room.c_str()).x + style.ItemSpacing.x * 3;
        {
            fit::Stretchy stretchy(textW + buttons);
            ImGui::SameLine(ImMax(ImGui::GetWindowContentRegionMax().x - buttons, ImGui::GetCursorStartPos().x + textW));
            if (ImGui::SmallButton("Leave"))
                mp::LeaveRoom();
            ImGui::SameLine();
            if (ImGui::SmallButton("Disconnect"))
                mp::Disconnect();
        }
        if (s.reconnecting)
            ImGui::TextColored(Gold, "Connection lost, reconnecting...");
        ImGui::Separator();
        DrawPlayers(s);
        ImGui::Separator();
        DrawRoundStatus(s);
        ImGui::Separator(); // status above, options below
        Config& cfg = GetConfig();
        if (ImGui::Checkbox("Live leaderboard", &cfg.showLeaderboard))
            SaveConfig();
        ImGui::SameLine();
        if (ImGui::Checkbox("Score gaps", &cfg.showGaps))
            SaveConfig();
        ImGui::SameLine();
        ImGui::TextColored(Dim, "(F9 hides everything)");
        if (ImGui::Checkbox("My own outfits", &cfg.ownOutfits))
            SaveConfig();
        ImGui::SameLine();
        ImGui::TextColored(Dim, cfg.ownOutfits ? "Your Customize choices for each song." : "Same outfits as the host.");
        if (report::Enabled()) {
            const char* label = "Report a bug";
            float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
            fit::Stretchy stretchy(-1);
            ImGui::SameLine(ImMax(ImGui::GetWindowContentRegionMax().x - w, ImGui::GetCursorPosX()));
            if (ImGui::SmallButton(label))
                OpenBugReport();
        }
        ImGui::Separator();
        DrawChat(s);
    }
    if (!s.inRoom && report::Enabled()) {
        ImGui::Spacing();
        if (ImGui::SmallButton("Report a bug"))
            OpenBugReport();
    }
    s_wanted = fit::Measure();
    ImGui::End();
}

static void DrawResults(const mp::State& s)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::Begin("Round results", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (s.pick) {
        ImGui::TextColored(Accent, "%s", PickName(*s.pick).c_str());
        ImVec4 detailColor;
        std::string detail = PickDetail(*s.pick, detailColor);
        ImGui::SameLine();
        ImGui::TextColored(detailColor, "%s", detail.c_str());
    }
    if (ImGui::BeginTable("results", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Score");
        ImGui::TableSetupColumn("Clear");
        ImGui::TableSetupColumn("Grade");
        ImGui::TableSetupColumn("Cool / Fine / Safe / Sad / Miss");
        ImGui::TableSetupColumn("Combo");
        ImGui::TableHeadersRow();
        for (const mp::FinalResult& r : s.results) {
            ImVec4 color = r.id == s.myId ? Accent : ImVec4(1, 1, 1, 1);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(r.place == 1 ? Gold : Dim, "%d", r.place);
            ImGui::TableSetColumnIndex(1);
            // Players who already left the room have no picture any more: their initial instead.
            const float size = ImGui::GetTextLineHeight() * 1.4f;
            float rowY = ImGui::GetCursorPosY();
            DrawAvatar(FindPlayer(s, r.id), r.name, size);
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowY + (size - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(color, "%s", r.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(color, "%07d", r.score);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.2f%%", TruncatePercent(r.completion));
            ImGui::TableSetColumnIndex(4);
            if (r.failed)
                ImGui::TextColored(Bad, "%s", r.grade == 0 && r.maxCombo == 0 ? "Quit" : GradeName(r.grade));
            else
                ImGui::Text("%s", GradeName(r.grade));
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d / %d / %d / %d / %d", r.cool, r.fine, r.safe, r.sad, r.worst);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%d", r.maxCombo);
        }
        ImGui::EndTable();
    }
    WantEscape();
    if (ImGui::Button("Close (Esc)", ImVec2(-1, 0)) || EscapePressed())
        open = false;
    ImGui::End();
    if (!open)
        mp::DismissResults();
}

// Shown over NOW LOADING while this player's song is held at the loading barrier.
static void DrawWaitingBanner(const mp::State& s)
{
    int loaded = 0, total = 0;
    for (const mp::Player& p : s.players) {
        if (p.playing) {
            total++;
            loaded += p.loaded;
        }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.72f), ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##waiting", nullptr, flags)) {
        if (s.pick)
            ImGui::TextUnformatted(PickName(*s.pick).c_str());
        ImGui::TextColored(Accent, "Waiting for players...  %d / %d loaded", loaded, total);
        for (const mp::Player& p : s.players)
            if (p.playing && !p.loaded)
                ImGui::TextColored(Dim, p.away ? "  %s (reconnecting)" : "  %s", p.name.c_str());
    }
    ImGui::End();
}

// The connection dropped while the screens weren't open (in a song, or F8 hidden): say so for a few seconds.
static void DrawConnectionLost(const mp::State& s)
{
    if (s_screenFrame == ImGui::GetFrameCount())
        return;
    if (!s.reconnecting && (!s.connLostAt || GetTickCount() - s.connLostAt > 8000))
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.16f), ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##connlost", nullptr, flags)) {
        ImGui::TextColored(Bad, "Connection to the multiplayer server lost.");
        if (s.reconnecting)
            ImGui::TextColored(Dim, "Reconnecting...");
        else
            ImGui::TextColored(Dim, g_session.inSong ? "The song goes on as a solo play. F8 after it to rejoin."
                                                     : "Press F8 to rejoin.");
    }
    ImGui::End();
}

void RequestGlyphs()
{
    const mp::State& s = mp::Get();
    glyphs::RequestText(GetConfig().name);
    glyphs::RequestText(GetConfig().room);
    glyphs::RequestText(s.roomName);
    if (s.pick)
        glyphs::RequestText(PickName(*s.pick));
    for (const mp::Player& p : s.players)
        glyphs::RequestText(p.name);
    for (const mp::LiveScore& p : s.scores)
        glyphs::RequestText(p.name);
    for (const mp::FinalResult& r : s.results)
        glyphs::RequestText(r.name);
    for (const mp::ChatLine& c : s.chat) {
        glyphs::RequestText(c.name);
        glyphs::RequestText(c.text);
    }
}

void DrawRoundOverlays()
{
    DrawPauseHint();
    const mp::State& s = mp::Get();
    if (s.inRound && sync::IsHolding())
        DrawWaitingBanner(s);
    if (!s.inRound)
        ResetLeaderboard();
    else if (GetConfig().showLeaderboard && (g_session.inSong || s.finishedSelf) && !s.showResults && !s.scores.empty())
        DrawLeaderboard(s); // also after we failed or quit: the others are still playing
    if (s.showResults && !g_session.inSong)
        DrawResults(s);
    DrawConnectionLost(s);
}

} // namespace md::ui
