#include "session.h"
#include "launch.h"
#include "pause_guard.h"
#include "round_finder.h"
#include "titles.h"
#include "../core/config.h"
#include "../core/log.h"

namespace md {

Session g_session;

static constexpr size_t MaxRecentEvents = 32;

static void LogResult(const GameState& s)
{
    if (!s.resultOk) {
        Log("result: (score data not located yet)");
        return;
    }
    Log("result: score=%d maxCombo=%d cool=%u fine=%u safe=%u sad=%u worst=%u grade=%d completion=%.2f",
        s.score, s.maxCombo, s.cool, s.fine, s.safe, s.sad, s.worst, s.grade, s.resultCompletion);
}

static void Remember(const GameEvent& e)
{
    Session& s = g_session;
    s.recentEvents.push_front(e);
    if (s.recentEvents.size() > MaxRecentEvents)
        s.recentEvents.pop_back();
    s.frameEvents.push_back(e);
}

static void Handle(const GameEvent& e)
{
    if (e.type == GameEventType::PvGameInit || e.type == GameEventType::SongEnded)
        pause::Reset();

    Session& s = g_session;
    switch (e.type) {
    case GameEventType::SongEnded:
        Log("SongEnded");
        s.inSong = false;
        s.inResult = false;
        break;
    case GameEventType::ModeShift: {
        int32_t m[12] = {};
        SafeRead(addr::ModeState, m);
        Log("ModeShift %s -> %d (caller %llX)  state: %d %d %d %d | %d %d %d %d | %d %d %d %d",
            e.edition ? "sub" : "mode", e.pvId, (unsigned long long)e.caller,
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
        break;
    }
    case GameEventType::PvGameState:
        Log("PvGameState outer=%d inner=%d", e.pvId, e.difficulty);
        break;
    case GameEventType::PvGameInit:
        if (e.overridden)
            Log("PvGameInit stage=%d pv=%d diff=%d edition=%d (OVERRIDDEN, game had picked pv=%d)",
                e.stageIndex, e.pvId, e.difficulty, e.edition, e.originalPvId);
        else
            Log("PvGameInit stage=%d pv=%d diff=%d edition=%d", e.stageIndex, e.pvId, e.difficulty, e.edition);
        Log("  modules: %d %d %d %d %d %d", e.modules[0], e.modules[1], e.modules[2], e.modules[3], e.modules[4], e.modules[5]);
        s.inSong = true;
        s.inResult = false;
        s.pvId = e.pvId;
        break;
    case GameEventType::ResultShown:
        Log("ResultShown");
        s.inSong = false;
        s.inResult = true;
        ReadGameState(s.state);
        LogResult(s.state);
        if (GetConfig().research)
            finder::OnResultShown();
        break;
    }
    Remember(e);

    // Any mode change that leaves gameplay (back to song select, the menu...) ends the song or its results.
    if (e.type == GameEventType::ModeShift && (s.inSong || s.inResult)
        && !(e.edition == 1 && e.pvId == mode::SubGameMain))
        Handle({ GameEventType::SongEnded, -1, e.tick });
}

void SessionTick()
{
    g_session.frameEvents.clear();
    GameEvent e;
    while (g_gameEvents.TryPop(e))
        Handle(e);

    Session& s = g_session;
    if (!s.inResult) // keep the result read when the result screen opened
        ReadGameState(s.state);
    ReadStageState(s.stage);

    titles::Tick();
}

} // namespace md
