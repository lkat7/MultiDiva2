#include "debug_panel.h"
#include "multiplayer_ui.h"
#include "gpu_capture.h"
#include "game_sprites.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../core/version.h"
#include "../game/diffhunt.h"
#include "../game/launch.h"
#include "../game/memory.h"
#include "../game/memscan.h"
#include "../mp/multiplayer.h"
#include "../game/pause_guard.h"
#include "../game/session.h"
#include "../game/sigs.h"
#include "../game/songs.h"
#include "../game/sync_start.h"
#include "../game/watch.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <imgui.h>

namespace md::ui {

static const ImVec4 Bad(1.0f, 0.4f, 0.4f, 1.0f);
static const ImVec4 Dim(0.6f, 0.6f, 0.6f, 1.0f);

static void Row(const char* label, uintptr_t address, bool ok, const char* fmt, ...)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    if (address)
        ImGui::TextColored(Dim, "%llX", (unsigned long long)address);
    ImGui::TableSetColumnIndex(2);
    if (!ok) {
        ImGui::TextColored(Bad, "read failed");
        return;
    }
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

static bool BeginValueTable(const char* id)
{
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        return false;
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn("Value");
    ImGui::TableHeadersRow();
    return true;
}

static void DrawSignatures()
{
    ImGui::Text("%d/%d signatures resolved", sigs::ResolvedCount(), (int)sigs::Count);
    for (int i = 0; i < sigs::Count; i++) {
        const sigs::Result& r = sigs::Get((sigs::Id)i);
        if (!r.address)
            ImGui::TextColored(Bad, "  %-18s not found (hook disabled)", r.name);
        else if ((uintptr_t)r.address != r.known103)
            ImGui::TextColored(Bad, "  %-18s %p (expected %llX)", r.name, r.address, (unsigned long long)r.known103);
        else
            ImGui::Text("  %-18s %p", r.name, r.address);
    }
}

static void DrawLive(const Session& s)
{
    const GameState& g = s.state;
    ImGui::Text("State: %s   pv=%d", s.inSong ? "in song" : s.inResult ? "result screen" : "menu", s.pvId);
    if (!g.liveOk && !g.resultOk) {
        ImGui::TextColored(Dim, "Score data not located yet (research: round finder at the result screen).");
        return;
    }
    ImGui::Text("Live: score %d  combo %d  %.2f %%", g.liveScore, g.liveCombo, g.completion);
    if (g.resultOk)
        ImGui::Text("Result: score %d  max combo %d  %.2f %%  grade %s  C/F/S/S/W %u/%u/%u/%u/%u", g.score,
            g.maxCombo, g.resultCompletion, ToString((FinalGrade)g.grade), g.cool, g.fine, g.safe, g.sad, g.worst);
}

static void DrawEvents(const Session& s)
{
    DWORD now = GetTickCount();
    if (s.recentEvents.empty())
        ImGui::TextColored(Dim, "No hook events yet. Start a song.");
    for (const GameEvent& e : s.recentEvents) {
        if (e.type == GameEventType::PvGameInit)
            ImGui::Text("%6.1fs ago  %s stage=%d pv=%d %s ed=%d%s", (now - e.tick) / 1000.0f, ToString(e.type),
                e.stageIndex, e.pvId, ToString((Difficulty)e.difficulty), e.edition, e.overridden ? "  OVERRIDDEN" : "");
        else
            ImGui::Text("%6.1fs ago  %s", (now - e.tick) / 1000.0f, ToString(e.type));
    }
}

static void DrawStage(const Session& s)
{
    int32_t m[12] = {};
    if (SafeRead(addr::ModeState, m))
        ImGui::TextColored(Dim, "Mode state: %d %d %d %d | %d %d %d %d | %d %d %d %d", m[0], m[1], m[2], m[3], m[4], m[5],
            m[6], m[7], m[8], m[9], m[10], m[11]);

    const StageState& st = s.stage;
    if (!st.ok) {
        ImGui::TextColored(Bad, "stage data read failed (%llX)", (unsigned long long)addr::StageData);
        return;
    }
    ImGui::Text("Current stage index: %d", st.index);
    if (!ImGui::BeginTable("stage", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        return;
    ImGui::TableSetupColumn("Entry");
    ImGui::TableSetupColumn("PV id");
    ImGui::TableSetupColumn("Difficulty");
    ImGui::TableSetupColumn("Edition");
    ImGui::TableHeadersRow();
    for (int i = 0; i < stage::EntryCount; i++) {
        const StageEntry& e = st.entries[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text(i == st.index ? "%d  <" : "%d", i);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%d", e.pvId);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d  %s", e.difficulty, ToString((Difficulty)e.difficulty));
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%d", e.edition);
    }
    ImGui::EndTable();
}

static void DrawModules(const launch::Block& b)
{
    ImGui::TextColored(Dim, "  modules: %d %d %d %d %d %d", b.Module(0), b.Module(1), b.Module(2), b.Module(3),
        b.Module(4), b.Module(5));
}

static void DrawLaunchOverride()
{
    static const ImVec4 Armed(0.4f, 1.0f, 0.4f, 1.0f);
    static const char* names[] = { "Easy", "Normal", "Hard", "Extreme", "ExExtreme" };

    ImGui::TextWrapped("Arm a launch, then pick ANY song in song select and start it. "
                       "The game launches the armed one instead.");

    launch::Request armed;
    if (launch::IsArmed(&armed)) {
        ImGui::TextColored(Armed, "ARMED (%s): pv %d, %s, edition %d", armed.fullBlock ? "full block" : "song only",
            armed.block.PvId(), ToString((Difficulty)armed.block.Difficulty()), armed.block.Edition());
        if (armed.fullBlock)
            DrawModules(armed.block);
        if (ImGui::Button("Disarm"))
            launch::Disarm();
        return;
    }

    // What a host would send: the whole block of their last launch, outfits included.
    launch::Block last;
    if (launch::LastLaunch(last)) {
        ImGui::Text("Last launch: pv %d, %s, edition %d", last.PvId(), ToString((Difficulty)last.Difficulty()), last.Edition());
        DrawModules(last);
        if (ImGui::Button("Arm last launch (as a client would)")) {
            launch::Arm({ last, true });
            Log("launch override armed: full block of last launch, pv=%d diff=%d", last.PvId(), last.Difficulty());
        }

        // Auto-start: what a client does when the host starts the round.
        bool inSelect = launch::InSongSelect();
        if (!inSelect)
            ImGui::BeginDisabled();
        if (ImGui::Button("Start last launch NOW")) {
            bool ok = launch::StartNow(last);
            Log("auto-start pv=%d diff=%d: %s", last.PvId(), last.Difficulty(), ok ? "requested" : "refused");
        }
        ImGui::SameLine();
        if (ImGui::Button("...with MY outfits (test)")) {
            bool ok = launch::StartNow(last, true);
            Log("auto-start (own outfits) pv=%d diff=%d: %s", last.PvId(), last.Difficulty(), ok ? "requested" : "refused");
        }
        if (!inSelect) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(Dim, "(only in song select)");
        }
    } else {
        ImGui::TextColored(Dim, "Start a song once to capture a launch block.");
    }
    if (ImGui::Button("Play start sound"))
        launch::PlayStartSound();

    int32_t hovered = songs::Hovered();
    if (hovered >= 0) {
        ImGui::Text("Highlighted song: pv %d (%s)", hovered, songs::Has(hovered) ? "installed" : "NOT installed");
        if (GetConfig().research) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Dump song entry (research)"))
                songs::DumpEntryStrings(hovered);
        }
    }
    ImGui::TextColored(Dim, "Song check: pv 9999 is %s (should be not installed)", songs::Has(9999) ? "installed" : "not installed");

    // F8 as the full-screen screens instead of the window (work in progress).
    bool screens = !ui::ClassicLobby();
    if (ImGui::Checkbox("Full-screen F8 screens (work in progress)", &screens))
        ui::SetClassicLobby(!screens);

    // Reverse-engineering tools, only with "research=1" in multidiva.ini (some freeze the game for a while).
    // Which game code writes the performer outfits into the launch slot, and other research.
    if (!GetConfig().research) {
    } else if (watch::Active()) {
        ImGui::TextColored(Armed, "Watching outfit writes: %d hit(s) logged", watch::Hits());
        if (ImGui::Button("Stop watching"))
            watch::Stop();
    } else {
        if (ImGui::Button("Watch outfit writes (research)")) {
            int32_t index = 0;
            SafeRead(addr::StageData + stage::IndexOffset, index);
            watch::Start(StageEntryAddress(index) + stage::Modules);
        }
        // Which game code writes the stage entry's difficulty (song select copies the selected difficulty there when
        // you confirm): start it in song select, change the difficulty with left/right, confirm, send the log. Each
        // hit logs the registers and the code before the write, which shows where the difficulty is read from.
        // Start it in song select, then start 4+ rounds on different difficulties in NO particular order (e.g. Extreme,
        // Easy, Hard, Normal), and send the log: the "diffhunt" lines name the fields that follow the difficulty.
        if (ImGui::Button("Watch difficulty write (research)")) {
            int32_t index = 0;
            SafeRead(addr::StageData + stage::IndexOffset, index);
            diffhunt::Arm();
            watch::Start(StageEntryAddress(index) + stage::Difficulty, 60);
        }
        // Which game code sets the highlighted song (song list object +0xA88, a 4-byte write): start it in song select,
        // press down a few times, use Random once, send the log. The stack and code around each write show the function
        // that moves the cursor and who calls it.
        if (ImGui::Button("Watch highlight write (research)")) {
            if (uintptr_t list = songs::ListObject()) {
                watch::SetHitCallback(nullptr);
                watch::Start(list + 0xA88, 60);
            } else {
                Log("watch: no song list object yet, open song select first");
            }
        }
        // Does calling the game's set-highlight function move the list? Open song select, type another song's id (the
        // F8 window / log show ids) and press the button: say whether the cursor scrolled to it, only the jacket/title
        // changed, or nothing; the log has the value before and after.
        static int testPv = 50;
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Test pv", &testPv, 0);
        ImGui::SameLine();
        if (ImGui::Button("Call set-highlight (research)"))
            songs::SetHighlightTest(testPv);
        // Song select's per-performer outfit selection for the highlighted song (0x141750890 + 0x1C0).
        if (ImGui::Button("Watch outfit selection (research)"))
            watch::Start(0x141750A50, 40);
        // Which textures and sprite rectangles the game draws this screen with (writes mods\MultiDiva\capture).
        if (ImGui::Button(ui::capture::Busy() ? "Capturing..." : "Capture this screen's sprites (research)"))
            ui::capture::Request();
        // Is the main menu's sprite set file (names, rectangles, textures) kept in memory once loaded?
        if (ImGui::Button("Find menu sprite set in memory (research)"))
            memscan::FindMenuSpriteSet();
        ImGui::SameLine();
        if (ImGui::Button("List loaded sprite sets (instant)"))
            memscan::ListSpriteSets();
        // Where the game keeps its font's per-letter widths (for drawing our text in the game's font).
        if (ImGui::Button("Find font map in memory (research)"))
            memscan::FindFontMap();
        // The game's own sprites drawn by us (ui/game_sprites.cpp).
        bool test = ui::sprites::TestEnabled();
        if (ImGui::Checkbox("Show game sprite test", &test)) {
            ui::sprites::SetTest(test);
            ui::sprites::LogStatus();
        }
        // The pause menu task's state (TaskPauseManagerSwitch + 0x70): who opens it on alt-tab.
        if (ImGui::Button("Watch pause menu state (research)"))
            watch::Start(0x14CC0CA90 + 0x70, 12);

        // ---- layouts taken from ReDIVA (the Arcade game), to confirm on Mega Mix+ ----
        // Level and BPM: the song's pv_db entry and chart struct as int32s. Compare with the star rating and tempo the
        // game shows for that chart: the number is at the offset songs.cpp assumes (EntryBpm 0x4C, ChartLevel 0x40;
        // the level is in half stars, 15 = 7.5).
        if (ImGui::Button("Dump pick/highlight entry + chart (research)")) {
            const mp::State& ms = mp::Get();
            int32_t pv = ms.pick ? ms.pick->pv : songs::Hovered();
            if (pv >= 0)
                songs::DumpChart(pv, ms.pick ? ms.pick->difficulty : 3, ms.pick ? ms.pick->edition : 0);
            else
                Log("songs: nothing picked or highlighted to dump");
        }
        // Button presses: which call checks song select's confirm button (press Enter / Cross on song select).
        static bool buttonLog = false;
        if (ImGui::Checkbox("Log button presses (research)", &buttonLog))
            pause::SetButtonLog(buttonLog);
        // The input object's bitsets: tap, hold and release buttons while playing and read the changed bytes in the log.
        static bool watchInput = false;
        static uint8_t lastInput[0x80] = {};
        ImGui::Checkbox("Watch input state while pressing buttons (research)", &watchInput);
        if (watchInput) {
            uint8_t now[sizeof(lastInput)] = {};
            if (const void* input = pause::InputObject(); input && SafeCopy(now, input, sizeof(now))) {
                for (size_t row = 0; row < sizeof(now); row += 16) {
                    if (memcmp(now + row, lastInput + row, 16) == 0)
                        continue;
                    char hex[16 * 3 + 1] = {};
                    for (int i = 0; i < 16; i++)
                        snprintf(hex + i * 3, 4, "%02X ", now[row + i]);
                    // The bits set, as the game's button numbers (ReDIVA InputButton: UP 3, DOWN 4, LEFT 5, RIGHT 6,
                    // TRIANGLE 7, SQUARE 8, CROSS 9, CIRCLE 10, L 11, R 12).
                    static const char* names[] = { "TEST", "SERVICE", "START", "UP", "DOWN", "LEFT", "RIGHT", "TRIANGLE",
                        "SQUARE", "CROSS", "CIRCLE", "L", "R" };
                    std::string bits;
                    for (int b = 0; b < 128; b++)
                        if (now[row + b / 8] & (1 << (b % 8))) {
                            bits += " " + std::to_string(b);
                            if (b < (int)(sizeof(names) / sizeof(*names)))
                                bits += std::string("(") + names[b] + ")";
                        }
                    Log("input: +0x%02zX  %s |%s", row, hex, bits.empty() ? " none" : bits.c_str());
                }
                memcpy(lastInput, now, sizeof(lastInput));
            }
        }
        // Where song select keeps the selected difficulty: turn this on in song select, then use whichever key CHANGES
        // THE DIFFICULTY (the arrows only move the song list) a few times on a song with all its charts, then send the log.
        // Also tick "Watch input state" above so the log names the buttons you pressed.
        static bool watchSelect = false;
        if (ImGui::Checkbox("Watch song select for the difficulty (research)", &watchSelect)) {
            songs::SetListWatch(watchSelect);
            pause::SetButtonLog(watchSelect); // button taps in the same log, to line up with what changes
        }
        // The player profile: type the in-game profile name, then find it in memory (UTF-8 and UTF-16 hits are logged).
        static char profileName[64] = "";
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("Profile name", profileName, sizeof(profileName));
        ImGui::SameLine();
        if (ImGui::Button("Find in memory") && profileName[0])
            memscan::FindText(profileName);
    }

    ImGui::Separator();
    static int pvId = 1;
    static int difficulty = (int)Difficulty::Hard;
    static bool extra = false;
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("PV id", &pvId);
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("Difficulty", &difficulty, names, IM_ARRAYSIZE(names));
    ImGui::Checkbox("Extra chart (edition 1)", &extra);
    if (ImGui::Button("Arm song only (keeps selected outfits)")) {
        launch::Request r{};
        r.block.SetInt(stage::PvId, pvId);
        r.block.SetInt(stage::Difficulty, difficulty);
        r.block.SetInt(stage::Edition, extra ? 1 : 0);
        r.fullBlock = false;
        launch::Arm(r);
        Log("launch override armed: song only, pv=%d diff=%d edition=%d", pvId, difficulty, extra ? 1 : 0);
    }
}

static void DrawSyncStart()
{
    sync::PvGameState st = sync::ReadState();
    if (st.exists)
        ImGui::Text("pv_game: outer %d, inner %d", st.outer, st.inner);
    else
        ImGui::TextColored(Dim, "pv_game: none (not in a song)");

    int step = sync::HoldStep();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Hold at loading step (-1 = off)", &step))
        sync::SetHoldStep(step < -1 ? -1 : step);

    if (sync::IsHolding()) {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.3f, 1.0f), "HOLDING at step %d", sync::HoldStep());
        if (ImGui::Button("Release"))
            sync::Release();
    }
}

void DrawDebugPanel(bool* open)
{
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MultiDiva2 debug v" MULTIDIVA_VERSION " (Ctrl+F8)###md_debug", open)) {
        ImGui::End();
        return;
    }

    const Session& s = g_session;
    if (ImGui::CollapsingHeader("Signatures", ImGuiTreeNodeFlags_DefaultOpen))
        DrawSignatures();
    if (ImGui::CollapsingHeader("Live state", ImGuiTreeNodeFlags_DefaultOpen))
        DrawLive(s);
    if (ImGui::CollapsingHeader("Stage data (song launch)", ImGuiTreeNodeFlags_DefaultOpen))
        DrawStage(s);
    if (ImGui::CollapsingHeader("Launch override", ImGuiTreeNodeFlags_DefaultOpen))
        DrawLaunchOverride();
    if (ImGui::CollapsingHeader("Sync start (experiment)", ImGuiTreeNodeFlags_DefaultOpen))
        DrawSyncStart();
    if (ImGui::CollapsingHeader("Pause guard", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool forced = pause::IsForced();
        if (ImGui::Checkbox("Block pausing outside multiplayer too (test)", &forced))
            pause::SetForced(forced);
        pause::State ps = pause::GetState();
        ImGui::TextColored(Dim, "paused: %s, hint: %d", ps.paused ? "yes" : "no", (int)ps.hint);
    }
    if (ImGui::CollapsingHeader("Hook events", ImGuiTreeNodeFlags_DefaultOpen))
        DrawEvents(s);

    ImGui::End();
}

} // namespace md::ui
