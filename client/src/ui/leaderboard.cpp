#include "leaderboard.h"
#include "avatars.h"
#include "overlay.h"
#include "../core/config.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <unordered_map>
#include <windows.h>

namespace md::ui {

// Proportions measured from osu!'s multiplayer leaderboard, relative to the card height.
constexpr float CardHeightOfScreen = 0.060f; // ~65 px at 1080p
constexpr float NameOfCard = 0.40f;
constexpr float SmallOfCard = 0.31f;
constexpr float OwnWidthFactor = 1.15f;      // our own card is this much wider than the others
constexpr float WidthOfCard = 3.7f;          // card incl. avatar (was 4.6: it covered notes)
constexpr float GapOfCard = 0.12f;
constexpr float RoundingOfCard = 0.16f;
constexpr float TopOfScreen = 0.097f;        // just below the life bar: notes reach the screen edges lower down

float CardHeight(float screenHeight) { return std::round(screenHeight * CardHeightOfScreen); }
float CardNameFontPx(float screenHeight) { return std::round(CardHeight(screenHeight) * NameOfCard); }
float CardSmallFontPx(float screenHeight) { return std::round(CardHeight(screenHeight) * SmallOfCard); }

namespace {

// Per-player animation state, keyed by player id.
struct Row {
    float y = -1;          // current slot position (in rows), animates toward the rank
    double shownScore = 0; // rolling number
    int lastRank = -1;
    int lastCombo = 0;
    float overtake = 0;    // 1 -> 0 glow after moving up
    float comboBreak = 0;  // 1 -> 0 red flash after a combo break
};

std::unordered_map<int, Row> g_rows;

constexpr float SlideSpeed = 10.0f; // higher = snappier position swaps
constexpr float ScoreSpeed = 12.0f; // how fast rolling scores catch up
constexpr float FlashFade = 1.8f;   // flash lifetime ~0.55 s
constexpr int ComboBreakMin = 20;   // combos below this breaking aren't dramatic

const ImVec4 Teal(0.45f, 0.86f, 0.86f, 1.0f);
const ImVec4 Gold(1.0f, 0.82f, 0.30f, 1.0f);
const ImVec4 Red(1.0f, 0.36f, 0.40f, 1.0f);
const ImVec4 Amber(1.0f, 0.69f, 0.18f, 1.0f);
const ImVec4 White(1.0f, 1.0f, 1.0f, 1.0f);

float Approach(float t, float dt) { return 1.0f - std::exp(-t * dt); }

ImU32 Col(ImVec4 c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

// 3033270 -> "3,033,270"
void FormatThousands(char* buf, size_t size, long long v)
{
    char digits[32];
    int n = snprintf(digits, sizeof(digits), "%lld", v < 0 ? -v : v);
    size_t o = 0;
    for (int i = 0; i < n && o + 2 < size; i++) {
        if (i > 0 && (n - i) % 3 == 0)
            buf[o++] = ',';
        buf[o++] = digits[i];
    }
    buf[o] = '\0';
}

ID3D11ShaderResourceView* PlayerPicture(const mp::State& s, int id)
{
    for (const mp::Player& p : s.players)
        if (p.id == id)
            return PlayerAvatar(p);
    return nullptr;
}

// Stable color per name, for players without an avatar.
ImVec4 NameColor(const std::string& name)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : name)
        h = (h ^ c) * 16777619u;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB((h % 360) / 360.0f, 0.40f, 0.62f, r, g, b);
    return ImVec4(r, g, b, 1.0f);
}

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text, const ImVec4* clip = nullptr)
{
    dl->AddText(font, size, ImVec2(std::round(pos.x), std::round(pos.y)), color, text, nullptr, 0.0f, clip);
}

ImVec2 Measure(ImFont* font, float size, const char* text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text); }

} // namespace

void ResetLeaderboard() { g_rows.clear(); }

void DrawLeaderboard(const mp::State& s)
{
    const Fonts& fonts = GetFonts();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dt = ImGui::GetIO().DeltaTime;

    const float cardH = CardHeight(vp->Size.y);
    const float nameSize = CardNameFontPx(vp->Size.y);
    const float smallSize = CardSmallFontPx(vp->Size.y);
    const float width = std::round(cardH * WidthOfCard);
    const float gap = std::round(cardH * GapOfCard);
    const float rounding = cardH * RoundingOfCard;
    const float pad = std::round(cardH * 0.16f);
    const int count = (int)s.scores.size();
    const bool showGaps = GetConfig().showGaps;

    bool haveMe = false; // our own card is on the board (not after we quit): the others can show a difference to it
    for (const mp::LiveScore& p : s.scores)
        haveMe |= p.id == s.myId;

    // Update animation state from the latest server ranking (scores arrive sorted).
    for (int rank = 0; rank < count; rank++) {
        const mp::LiveScore& p = s.scores[rank];
        Row& r = g_rows[p.id];
        if (r.y < 0) {
            r.y = (float)rank; // first sight: no slide-in
            r.lastRank = rank;
        }
        if (rank < r.lastRank)
            r.overtake = 1.0f;
        if (p.combo == 0 && r.lastCombo >= ComboBreakMin)
            r.comboBreak = 1.0f;
        r.lastRank = rank;
        r.lastCombo = p.combo;

        r.y += ((float)rank - r.y) * Approach(SlideSpeed, dt);
        r.shownScore += ((double)p.score - r.shownScore) * Approach(ScoreSpeed, dt);
        if (std::fabs(p.score - r.shownScore) < 1.0)
            r.shownScore = p.score;
        r.overtake = r.overtake > 0 ? r.overtake - dt * FlashFade : 0;
        r.comboBreak = r.comboBreak > 0 ? r.comboBreak - dt * FlashFade : 0;
    }

    // Cards hug the left screen edge, like osu!.
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y * TopOfScreen), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width * OwnWidthFactor + 2, count * (cardH + gap) + cardH * 0.6f), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin("##leaderboard", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetWindowPos();
    // A window clips half its padding off its edges, which would cut the rounded corners square.
    dl->PushClipRectFullScreen();

    // Draw bottom-up so a card sliding up passes over the one it overtakes.
    for (int rank = count - 1; rank >= 0; rank--) {
        const mp::LiveScore& p = s.scores[rank];
        Row& r = g_rows[p.id];
        bool me = p.id == s.myId;
        // A player whose updates stopped (lag) or who hasn't loaded yet: the numbers aren't current, so dim the card.
        const bool stale = (p.lag || p.loading) && !p.finished;
        const float dim = stale ? 0.62f : 1.0f;

        ImVec2 a(origin.x, std::round(origin.y + r.y * (cardH + gap)));
        ImVec2 b(a.x + (me ? width * OwnWidthFactor : width), a.y + cardH);
        ImVec2 avB(a.x + cardH, b.y);

        // Card body: translucent navy, rounded on the right; the avatar sits square on the left.
        ImVec4 navy = me ? ImVec4(0.13f, 0.23f, 0.35f, 0.90f * dim) : ImVec4(0.06f, 0.09f, 0.16f, 0.66f * dim);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(navy), rounding, ImDrawFlags_RoundCornersRight);
        if (me) // a teal outline: our own card stands out from the others
            dl->AddRect(a, b, Col(Teal, 0.85f * dim), rounding, ImDrawFlags_RoundCornersRight, 2.0f);
        if (r.overtake > 0)
            dl->AddRectFilled(a, b, Col(me ? Teal : Gold, 0.28f * r.overtake), rounding, ImDrawFlags_RoundCornersRight);
        if (r.comboBreak > 0)
            dl->AddRectFilled(a, b, Col(Red, 0.38f * r.comboBreak), rounding, ImDrawFlags_RoundCornersRight);

        if (ID3D11ShaderResourceView* tex = PlayerPicture(s, p.id)) {
            dl->AddImage((ImTextureID)tex, a, avB, ImVec2(0, 0), ImVec2(1, 1), Col(White, dim));
        } else {
            dl->AddRectFilled(a, avB, Col(NameColor(p.name), dim));
            char initial[2] = { p.name.empty() ? '?' : (char)toupper((unsigned char)p.name[0]), 0 };
            ImVec2 sz = Measure(fonts.cardName, nameSize * 1.3f, initial);
            Text(dl, fonts.cardName, nameSize * 1.3f, ImVec2(a.x + (cardH - sz.x) * 0.5f, a.y + (cardH - sz.y) * 0.5f),
                Col(White, 0.95f), initial);
        }

        char buf[48];
        const float textX = avB.x + pad;
        const float right = b.x - pad;
        const float alpha = (me ? 1.0f : 0.88f) * dim;

        // Faint rank number behind the second line's right end.
        snprintf(buf, sizeof(buf), "%d", rank + 1);
        ImVec2 rankSz = Measure(fonts.cardName, nameSize * 1.15f, buf);
        Text(dl, fonts.cardName, nameSize * 1.15f, ImVec2(right - rankSz.x, a.y + cardH * 0.30f), Col(White, 0.10f), buf);

        // First line: the name, and the accuracy at the right (the game's completion percentage).
        snprintf(buf, sizeof(buf), "%.2f%%", std::floor(p.completion * 100.0 + 1e-6) / 100.0);
        ImVec2 pctSz = Measure(fonts.cardSmall, smallSize, buf);
        const float nameY = a.y + cardH * 0.08f;
        Text(dl, fonts.cardSmall, smallSize, ImVec2(right - pctSz.x, nameY + (nameSize - smallSize) * 0.7f), Col(White, 0.72f * alpha), buf);
        ImVec4 clip(textX, a.y, right - pctSz.x - pad * 0.4f, b.y);
        Text(dl, fonts.cardName, nameSize, ImVec2(textX, nameY), Col(White, alpha), p.name.c_str(), &clip);

        // Second line: the score on the left; on the right the combo, or a chip when the numbers aren't current.
        const float lineY = b.y - cardH * 0.14f - smallSize;
        // With "Score gaps" on, the others show how far ahead (+, red) or behind (-, teal) they are of us instead of
        // their whole score; our own card keeps the full score.
        const bool noScore = p.loading && !p.finished;
        const bool asGap = showGaps && haveMe && !me && !p.finished && !noScore;
        ImVec4 scoreColor = White;
        float scoreAlpha = noScore ? 0.45f : 0.92f;
        if (asGap) {
            const long long diff = std::llround(r.shownScore) - std::llround(g_rows[s.myId].shownScore);
            buf[0] = diff > 0 ? '+' : diff < 0 ? '-' : ' ';
            FormatThousands(buf + 1, sizeof(buf) - 1, diff < 0 ? -diff : diff);
            if (diff == 0)
                snprintf(buf, sizeof(buf), "even");
            scoreColor = diff > 0 ? Red : Teal;
            scoreAlpha = 0.95f;
        } else {
            FormatThousands(buf, sizeof(buf), std::llround(r.shownScore));
        }
        Text(dl, fonts.cardSmall, smallSize, ImVec2(textX, lineY), Col(scoreColor, scoreAlpha * alpha), buf);

        auto chip = [&](const char* label, ImVec4 bg, ImU32 fg) {
            const float px = smallSize * 0.80f;
            ImVec2 sz = Measure(fonts.cardSmall, px, label);
            const float padX = px * 0.45f, padY = px * 0.12f;
            ImVec2 c0(right - sz.x - padX * 2, lineY + (smallSize - px) * 0.5f - padY);
            ImVec2 c1(right, c0.y + px + padY * 2);
            dl->AddRectFilled(c0, c1, Col(bg, 0.95f), px * 0.30f);
            Text(dl, fonts.cardSmall, px, ImVec2(c0.x + padX, c0.y + padY), fg, label);
        };
        if (p.lag && !p.finished) {
            chip("LAG", Amber, IM_COL32(42, 26, 0, 255));
        } else if (p.loading && !p.finished) {
            chip("loading", ImVec4(1, 1, 1, 0.22f), IM_COL32(255, 255, 255, 235));
        } else {
            snprintf(buf, sizeof(buf), p.failed ? "failed" : p.finished ? "done" : "%dx", p.combo);
            ImVec2 comboSz = Measure(fonts.cardSmall, smallSize, buf);
            Text(dl, fonts.cardSmall, smallSize, ImVec2(right - comboSz.x, lineY),
                Col(p.failed || r.comboBreak > 0 ? Red : Teal, alpha), buf);
        }
    }
    // No scoreboard message from the server for 2 s: the stall is on our side (or the connection), not the players'.
    if (!s.finishedSelf && s.scoresAt && GetTickCount() - s.scoresAt > 2000) {
        const char* label = "no updates from the server";
        const float px = smallSize * 0.85f;
        ImVec2 sz = Measure(fonts.cardSmall, px, label);
        ImVec2 c0(origin.x, origin.y + count * (cardH + gap));
        ImVec2 c1(c0.x + sz.x + px, c0.y + px * 1.5f);
        dl->AddRectFilled(c0, c1, Col(Amber, 0.92f), px * 0.3f);
        Text(dl, fonts.cardSmall, px, ImVec2(c0.x + px * 0.5f, c0.y + px * 0.25f), IM_COL32(42, 26, 0, 255), label);
    }
    dl->PopClipRect();
    ImGui::End();
}

} // namespace md::ui
