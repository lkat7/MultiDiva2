#include "room_scene.h"
#include "avatars.h"
#include "game_sprites.h"
#include "overlay.h"
#include "../core/log.h"
#include "../core/version.h"
#include "../game/titles.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <vector>
#include <imgui.h>
#include <initializer_list>
#include <map>
#include <string>

namespace md::ui {

float SceneTitlePx(float h) { return std::round(h * 0.052f); }
float SceneButtonPx(float h) { return std::round(h * 0.036f); }
float SceneTextPx(float h) { return std::round(h * 0.026f); }
float SceneSmallPx(float h) { return std::round(h * 0.023f); }

namespace {

using sprites::Font;
namespace set = sprites::set;

const ImU32 White = IM_COL32(255, 255, 255, 255);
const ImU32 Ink = IM_COL32(40, 40, 44, 255);       // song select's title color
const ImU32 InkSoft = IM_COL32(110, 112, 118, 255);
const ImU32 Shadow = IM_COL32(20, 20, 30, 170);
const ImU32 PinkPlate = IM_COL32(240, 80, 176, 255); // FOOTER_RIGHT's plate (sampled)
const ImU32 PinkShadow = IM_COL32(120, 30, 90, 255);
const ImU32 TealDark = IM_COL32(10, 110, 118, 255);

ImU32 Alpha(ImU32 c, float a)
{
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f)) << IM_COL32_A_SHIFT);
}

// Everything is laid out in 1080p pixels (the size the game's sprites are authored for) and scaled.
struct Ctx {
    ImDrawList* dl;
    ImVec2 o;
    float s;
    ImVec2 P(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
};

// A game sprite at 1080p position (x, y), `scale` times its authored size. Returns false if not loaded.
bool Spr(const Ctx& c, uint32_t setId, const char* name, float x, float y, float scale = 1, ImU32 tint = White)
{
    const sprites::Sprite* sp = sprites::Find(setId, name);
    if (!sp)
        return false;
    sprites::Draw(c.dl, *sp, c.P(x, y), c.P(x + sp->size.x * scale, y + sp->size.y * scale), tint);
    return true;
}

// Text in the game's font (falls back to ImGui's until the game font has been seen loaded). Sizes in 1080p px.
struct Style {
    Font font = Font::Bold;
    float px = 36;
    ImU32 color = Ink;
    ImU32 outline = 0; // 0 = none
    float outlineW = 3;
    float spacing = -1; // < 0: song select's title spacing
    float weight = 0;   // extra horizontal strokes, for headings heavier than the bold sheet
    ImU32 shadow = 0;   // drop shadow down-right, like the game's headings
};

// The overlay font used while the game's own font isn't available (never null: the screens used to crash on a null
// scene font whenever the game's font sheet hadn't been found).
ImFont* FallbackFont()
{
    const Fonts& f = GetFonts();
    return f.sceneButton ? f.sceneButton : f.regular ? f.regular : ImGui::GetFont();
}

float TextW(const Style& st, const char* text)
{
    float sp = st.spacing < 0 ? st.px * sprites::TitleSpacingOfSize : st.spacing;
    if (sprites::TextReady(st.font))
        return sprites::TextWidth(st.font, text, st.px, sp) + st.weight;
    return FallbackFont()->CalcTextSizeA(st.px, FLT_MAX, 0, text).x;
}

void Text(const Ctx& c, float x, float y, const char* text, const Style& st, float alpha = 1)
{
    float px = st.px * c.s;
    float sp = (st.spacing < 0 ? st.px * sprites::TitleSpacingOfSize : st.spacing) * c.s;
    bool game = sprites::TextReady(st.font);
    static bool s_loggedFallback = false;
    if (!game && !s_loggedFallback) {
        s_loggedFallback = true;
        Log("screens: the game's font isn't available yet, drawing text with the overlay font");
    }
    auto draw = [&](float dx, float dy, ImU32 col) {
        ImVec2 p = c.P(x, y);
        p.x += dx;
        p.y += dy;
        if (game)
            sprites::DrawText(c.dl, st.font, p, text, px, Alpha(col, alpha), sp);
        else
            c.dl->AddText(FallbackFont(), px, p, Alpha(col, alpha), text);
    };
    float heavy = st.weight * c.s;
    if (st.shadow)
        for (float w = 0; w <= heavy; w += 1)
            draw(w + 4 * c.s, 4 * c.s, st.shadow);
    if (st.outline) {
        float r = st.outlineW * c.s;
        for (int i = 0; i < 12; i++) {
            float a = i * 3.14159265f / 6;
            for (float w = 0; w <= heavy; w += 1)
                draw(std::cos(a) * r + w, std::sin(a) * r, st.outline);
        }
    }
    for (float w = 0; w <= heavy; w += 1)
        draw(w, 0, st.color);
}

// ---- hover animation ----

std::map<int, float> s_hover; // row key -> 0..1

float Ease(int key, bool on)
{
    float& t = s_hover[key];
    float dt = ImGui::GetIO().DeltaTime;
    t = std::clamp(t + (on ? dt : -dt) * 7.0f, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// An invisible ImGui button over a 1080p rectangle: the screen's ImGui window takes the mouse (so the game doesn't
// see it), and hover/clicks resolve like any ImGui item, also when another window covers part of the screen.
struct Hit {
    bool hovered = false, clicked = false, rightClicked = false;
};
Hit HitTest(const Ctx& c, int key, float x, float y, float w, float h)
{
    ImGui::SetCursorScreenPos(c.P(x, y));
    ImGui::PushID(key);
    Hit r;
    r.clicked = ImGui::InvisibleButton("##hit", ImVec2(std::max(1.0f, w * c.s), std::max(1.0f, h * c.s)));
    r.hovered = ImGui::IsItemHovered();
    r.rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    ImGui::PopID();
    return r;
}

// ---- player rows: song select's song bars ----

// Bar color by status, from the game's difficulty bars: host gold, ready green, playing purple, missing the song
// red; not ready stays grey. Drawn from the star-less grey bar, recolored (the stars made rows look like songs).
ImU32 BarColor(const mp::State& s, const mp::Player& p)
{
    if (p.away)
        return IM_COL32(90, 94, 104, 255);
    if (p.missing)
        return IM_COL32(218, 0, 30, 255);
    if (p.playing)
        return IM_COL32(168, 14, 234, 255);
    if (p.id == s.hostId)
        return IM_COL32(216, 152, 0, 255);
    return p.ready ? IM_COL32(25, 180, 6, 255) : 0;
}

void Bar(const Ctx& c, ImU32 color, bool sel, float x, float y, float scale, float alpha)
{
    const sprites::Sprite* sp = sprites::Find(set::SongSelect, sel ? "LIST_BASE_BLANK_SEL" : "LIST_BASE_BLANK");
    if (!sp)
        return;
    ImVec2 a = c.P(x, y), b = c.P(x + sp->size.x * scale, y + sp->size.y * scale);
    if (!color)
        sprites::Draw(c.dl, *sp, a, b, Alpha(White, alpha));
    else // the grey bar: band 94, top 177 (sampled)
        sprites::DrawRecolored(c.dl, *sp, a, b, color, White, 94 / 255.0f, 177 / 255.0f, Alpha(White, alpha));
}

const char* StatusText(const mp::State& s, const mp::Player& p)
{
    if (p.away)
        return "Reconnecting...";
    if (p.missing)
        return "No song";
    if (p.playing)
        return s.roomState == "playing" ? "Playing" : "Song in progress";
    if (p.id == s.hostId)
        return "Host";
    return p.ready ? "Ready" : "Not ready";
}

// Inner layout of a bar (normal and selected sprites), in that sprite's pixels.
struct BarLayout {
    float w, h;
    float picX, picY, picSize; // the white picture box on the left
    float nameX, nameY, namePx;
    float lineX, lineY, linePx; // status line in the colored band
    float starX, starY;         // centre of the star on the right
};
const BarLayout Normal = { 778, 114, 24, 14, 70, 140, 12, 38, 150, 62, 30, 722, 60 };
const BarLayout Selected = { 939, 126, 30, 15, 82, 176, 12, 44, 186, 70, 34, 880, 50 };

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

void Picture(const Ctx& c, const mp::Player& p, float x, float y, float size, float alpha)
{
    ImVec2 a = c.P(x, y), b = c.P(x + size, y + size);
    if (ID3D11ShaderResourceView* tex = PlayerAvatar(p)) {
        c.dl->AddImage((ImTextureID)tex, a, b, ImVec2(0, 0), ImVec2(1, 1), Alpha(White, alpha));
        return;
    }
    c.dl->AddRectFilled(a, b, Alpha(IM_COL32(120, 190, 210, 255), alpha));
    // The first letter (a whole UTF-8 character: names can be Japanese), upper case if ASCII.
    std::string initial = p.name.empty() ? "?" : p.name.substr(0, 1);
    unsigned char lead = p.name.empty() ? 0 : (unsigned char)p.name[0];
    size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (len > 1)
        initial = p.name.substr(0, len);
    else if (!p.name.empty())
        initial[0] = (char)toupper(lead);
    Style st;
    st.px = size * 0.62f;
    st.color = White;
    st.spacing = 0;
    Text(c, x + (size - TextW(st, initial.c_str())) / 2, y + size * 0.18f, initial.c_str(), st, alpha);
}

Hit PlayerRow(const Ctx& c, const mp::State& s, const mp::Player& p, float x, float y, bool canHover)
{
    Hit hit = HitTest(c, 1000 + p.id, x, y, Normal.w, Normal.h);
    bool hot = canHover && hit.hovered;
    float t = Ease(p.id, hot);
    // The selected bar pokes out to the left and is taller, like song select's highlighted song.
    float sx = x - 40 * t, sy = y - 6 * t;
    float grow = Lerp(0.86f, 1.0f, t);
    ImU32 color = BarColor(s, p);
    if (t < 1)
        Bar(c, color, false, x, y, 1, 1 - t * 0.999f);
    if (t > 0) {
        Bar(c, color, true, sx, sy, grow, t);
        const sprites::Sprite* cur = sprites::Find(set::SongSelect, "LIST_BASE_CURSOL");
        if (cur) {
            float pulse = 0.75f + 0.25f * std::sin((float)ImGui::GetTime() * 5);
            float cx = sx - 22 * grow, cy = sy - 15 * grow;
            sprites::Draw(c.dl, *cur, c.P(cx, cy), c.P(cx + cur->size.x * grow, cy + cur->size.y * grow), Alpha(White, t * pulse));
        }
    }
    // Contents follow the bar between its two sizes.
    auto lx = [&](float n, float sel) { return Lerp(x + n, sx + sel * grow, t); };
    auto ly = [&](float n, float sel) { return Lerp(y + n, sy + sel * grow, t); };
    // The bar's slanted white picture box is painted away (its lower part, below the white top band, gets the
    // band's color back), then the square picture sits there like a sticker. Corners measured on both sprites.
    static const ImVec2 bandN[4] = { { 22, 54.5f }, { 127, 54.5f }, { 137, 88 }, { 32, 88 } };
    static const ImVec2 bandS[4] = { { 26, 60.5f }, { 157, 60.5f }, { 171, 100 }, { 38, 100 } };
    ImVec2 band[4];
    for (int i = 0; i < 4; i++)
        band[i] = c.P(lx(bandN[i].x, bandS[i].x), ly(bandN[i].y, bandS[i].y));
    c.dl->AddConvexPolyFilled(band, 4, color ? color : IM_COL32(94, 93, 94, 255));
    float pic = Lerp(72, 80 * grow, t);
    float px = lx(36, 40), py = ly(13, 14);
    c.dl->AddRectFilled(c.P(px - 3, py - 3), c.P(px + pic + 3, py + pic + 3), White);
    Picture(c, p, px, py, pic, 1);

    Style name;
    name.px = Lerp(Normal.namePx, Selected.namePx * grow, t);
    std::string label = p.name + (p.id == s.myId ? "  (you)" : "");
    Text(c, lx(Normal.nameX, Selected.nameX), ly(Normal.nameY, Selected.nameY), label.c_str(), name);

    Style line;
    line.px = Lerp(Normal.linePx, Selected.linePx * grow, t);
    line.color = White;
    line.outline = IM_COL32(0, 0, 0, 110);
    line.outlineW = 2;
    std::string status = StatusText(s, p);
    if (p.wins > 0) // the session scoreboard: rounds won in this room
        status += p.wins == 1 ? "  -  1 win" : "  -  " + std::to_string(p.wins) + " wins";
    Text(c, lx(Normal.lineX, Selected.lineX), ly(Normal.lineY, Selected.lineY), status.c_str(), line);
    if (!canHover)
        hit = Hit{};
    return hit;
}

// ---- song card: the jacket frame, title plate and difficulty ----

void SongCard(const Ctx& c, const mp::State& s, const RoomView& view, float x, float y)
{
    const float jk = 400; // jacket size on screen (the sprites are 502)
    float k = jk / 502;
    Spr(c, set::SongSelect, "SONG_JK_SDW", x + 14, y + 14, k, Alpha(White, 0.8f));
    // The song's own jacket once the game has loaded it for us, else the game's NOW PRINTING placeholder.
    if (const sprites::Sprite* jacket = view.songPv >= 0 ? sprites::Jacket(view.songPv) : nullptr)
        sprites::Draw(c.dl, *jacket, c.P(x, y), c.P(x + jk, y + jk));
    else if (!Spr(c, set::SongSelect, "SONG_JK_DUMMY", x, y, k))
        c.dl->AddRectFilled(c.P(x, y), c.P(x + jk, y + jk), IM_COL32(120, 120, 120, 255));

    // Title on the game's white menu plate, the difficulty chip on its right end.
    float py = y + jk + 36;
    Spr(c, set::Common, "MENU_BASE", x - 140, py, 0.82f);
    bool song = !view.songTitle.empty();
    Style st;
    st.px = song ? 40.0f : 32.0f;
    st.color = song ? Ink : InkSoft;
    const char* title = song ? view.songTitle.c_str() : view.songHint.c_str();
    const float chipW = 184 * 0.9f, chipH = 52 * 0.9f, margin = (80 - chipH) / 2;
    bool chip = song && view.songDifficulty >= 0;
    float space = 705 - 50 - (chip ? chipW + margin * 2 : 30); // title room on the plate
    while (st.px > 24 && TextW(st, title) > space)
        st.px -= 1, st.spacing = st.px * sprites::TitleSpacingOfSize;
    c.dl->PushClipRect(c.P(x - 140, py), c.P(x - 90 + space, py + 80), true);
    Text(c, x - 90, py + (80 - st.px) / 2 - 3, title, st);
    c.dl->PopClipRect();
    if (chip) {
        static const char* chips[] = { "LV_ESY", "LV_NML", "LV_HRD", "LV_EXT", "LV_EXEX" };
        // Plate: 876x98 at 0.82 -> white area ends ~705 px in, middle at 40 px; chip 184x52 at 0.9.
        Spr(c, set::SongSelect, chips[std::clamp(view.songDifficulty, 0, 4)], x - 140 + 705 - margin - chipW,
            py + 40 - chipH / 2, 0.9f);
    }
    // A highlight has no difficulty yet: list the song's charts, each in its colour.
    bool hasCharts = false;
    for (float lv : view.charts)
        hasCharts |= lv >= 0;
    if (hasCharts || (view.songDifficulty < 0 && view.songBpm > 0)) {
        static const char* short_[] = { "Easy", "Normal", "Hard", "Extreme", "Extra" };
        struct Piece { std::string text; ImU32 color; bool selected; };
        std::vector<Piece> pieces;
        for (int i = 0; i < 5; i++) {
            if (view.charts[i] < 0)
                continue;
            std::string t = short_[i];
            if (view.charts[i] > 0) {
                char lv[16];
                snprintf(lv, sizeof(lv), " %.1f", view.charts[i]);
                t += lv;
            }
            // Only the selected chart keeps its colour; the others are grey (all coloured while the selection is unknown).
            bool grey = view.selectedChart >= 0 && i != view.selectedChart;
            pieces.push_back({ t, grey ? IM_COL32(150, 152, 160, 255) : DifficultyColor(i), i == view.selectedChart });
        }
        if (view.songBpm > 0)
            pieces.push_back({ "BPM " + std::to_string(view.songBpm), White, false });
        Style d;
        d.font = Font::Bold;
        d.px = 32;
        d.outline = IM_COL32(0, 0, 0, 170);
        d.outlineW = 2;
        const float room = 705 - 60, gapW = 24;
        auto total = [&]() {
            float w = -gapW;
            for (const Piece& p : pieces)
                w += TextW(d, p.text.c_str()) + gapW;
            return w;
        };
        while (d.px > 20 && total() > room) // many charts with levels: shrink to fit the plate
            d.px -= 1;
        float cx = x - 90;
        for (const Piece& p : pieces) {
            float w = TextW(d, p.text.c_str());
            if (p.selected) { // the chart the host has selected: a tinted tab behind it
                c.dl->AddRectFilled(c.P(cx - 10, py + 98 - 4), c.P(cx + w + 10, py + 98 + d.px + 8),
                    (p.color & 0x00FFFFFF) | 0x66000000, 8 * c.s);
                c.dl->AddRect(c.P(cx - 10, py + 98 - 4), c.P(cx + w + 10, py + 98 + d.px + 8), p.color, 8 * c.s, 0, 2);
            }
            d.color = p.color;
            Text(c, cx, py + 98, p.text.c_str(), d);
            cx += w + gapW;
        }
        py += 44;
    }
    // Difficulty (in its colour), level and tempo under the plate (a picked song; a highlight has the chart row above).
    if (view.songDifficulty >= 0 && !hasCharts) {
        static const char* names[] = { "Easy", "Normal", "Hard", "Extreme", "Extra Extreme" };
        std::string info;
        if (view.songDifficulty >= 0 && view.songDifficulty < 5) {
            info = names[view.songDifficulty];
            if (view.songLevel > 0) {
                char lv[24];
                snprintf(lv, sizeof(lv), "  %.1f", view.songLevel);
                info += lv;
            }
        }
        if (view.songBpm > 0)
            info += std::string(info.empty() ? "" : "     ") + "BPM " + std::to_string(view.songBpm);
        Style d;
        d.font = Font::Bold;
        d.px = 34;
        d.color = view.songDifficulty >= 0 ? DifficultyColor(view.songDifficulty) : White;
        d.outline = IM_COL32(0, 0, 0, 170);
        d.outlineW = 2;
        Text(c, x - 90, py + 98, info.c_str(), d);
        py += 44; // the status line moves down
    }
    if (!view.status.empty()) {
        Style h;
        h.font = Font::Thin;
        h.px = 28;
        h.spacing = 1;
        h.color = view.statusTone == RoomView::Tone::Bad ? IM_COL32(255, 150, 150, 255)
            : view.statusTone == RoomView::Tone::Good ? IM_COL32(170, 255, 235, 255) : White;
        h.outline = IM_COL32(0, 0, 0, 150);
        h.outlineW = 2;
        Text(c, x - 90, py + 104, view.status.c_str(), h);
    }
}

// The start button: cyan when it is the thing to press, with the game's cursor around it when hovered.
bool StartButton(const Ctx& c, float x, float y, const char* label, const char* labelSprite, bool lit, int key)
{
    Hit hit = HitTest(c, key, x, y, 530, 84);
    bool hot = hit.hovered;
    float t = Ease(key, hot);
    // Hovered: 4% bigger around its centre, with the game's cursor frame pulsing around it.
    float g = 1 + 0.04f * t, gx = x - 265 * (g - 1), gy = y - 42 * (g - 1);
    Spr(c, set::SongSelect, lit ? "STARTSETTING_START_BTN_SEL" : "STARTSETTING_START_BTN", gx - (lit ? 14 : 0) * g,
        gy - (lit ? 2 : 0) * g, g);
    if (t > 0) {
        float pulse = 0.75f + 0.25f * std::sin((float)ImGui::GetTime() * 5);
        for (int i = 0; i < 2; i++) // twice: one pass is too faint on the cyan button
            Spr(c, set::SongSelect, "STARTSETTING_STARTBTN_CURSOL", gx - 18 * g, gy - 2 * g, g, Alpha(White, t * pulse));
    }
    if (labelSprite && Spr(c, set::SongSelect, labelSprite, x + 65, y + 10))
        return hit.clicked;
    Style st;
    st.px = 46;
    st.color = lit ? White : InkSoft;
    st.outline = lit ? TealDark : 0;
    st.outlineW = 4;
    st.weight = 2;
    Text(c, x + (530 - TextW(st, label)) / 2, y + 16, label, st);
    return hit.clicked;
}

// ---- shared screen parts ----

Ctx BeginScreen(const char* id)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    // Below every other ImGui window (results, debug panel, popups); takes the mouse wheel itself.
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float S = vp->Size.y / 1080.0f;
    // Keep the 16:9 layout centred on wider screens.
    Ctx c{ dl, ImVec2(vp->Pos.x + (vp->Size.x - 1920 * S) / 2, vp->Pos.y), S };
    ImVec2 o = vp->Pos, e(o.x + vp->Size.x, o.y + vp->Size.y);
    // Background: the game's Miku loading art, mirrored so she stands on the right; a soft wash on the left keeps
    // the content readable. Plain cyan until the common set has been seen loaded.
    dl->AddRectFilled(o, e, IM_COL32(60, 190, 210, 255));
    if (const sprites::Sprite* bg = sprites::Find(set::Common, "LOADING_BG_01"))
        sprites::Draw(dl, *bg, c.P(1920, 0), c.P(0, 1080));
    dl->AddRectFilledMultiColor(o, ImVec2(o.x + vp->Size.x * 0.62f, e.y), IM_COL32(230, 250, 252, 170),
        IM_COL32(230, 250, 252, 0), IM_COL32(230, 250, 252, 0), IM_COL32(230, 250, 252, 170));
    Spr(c, set::SongSelect, "HEADER", 0, 0);
    return c;
}

// The main menu's footer with "Multiplayer" painted over its label, and key hints right-aligned under it.
void Footer(const Ctx& c, std::initializer_list<std::pair<const char*, const char*>> hints)
{
    Spr(c, set::MainMenu, "FOOTER_LEFT", 0, 1080 - 180);
    if (Spr(c, set::MainMenu, "FOOTER_RIGHT", 1142, 1080 - 160)) {
        c.dl->AddRectFilled(c.P(1142 + 250, 1080 - 160), c.P(1142 + 778, 1080 - 160 + 90), PinkPlate);
        Style st;
        st.px = 62;
        st.color = White;
        st.shadow = PinkShadow;
        st.weight = 3;
        st.spacing = 4;
        Text(c, 1890 - TextW(st, "Multiplayer"), 1080 - 160 + 12, "Multiplayer", st);
    }
    // Pink chip + key name, then what it does; measured first so the row ends at the same margin as the label.
    Style k;
    k.font = Font::Thin;
    k.px = 26;
    k.color = IM_COL32(60, 20, 50, 255);
    k.spacing = 1;
    Style w;
    w.px = 30;
    w.color = IM_COL32(240, 120, 200, 255);
    w.spacing = 1;
    const float gap = 40;
    float total = -gap;
    for (auto& h : hints)
        total += TextW(k, h.first) + 24 + 10 + TextW(w, h.second) + gap;
    float hx = 1890 - total, hy = 1080 - 52;
    for (auto& h : hints) {
        float kw = TextW(k, h.first) + 24;
        c.dl->AddRectFilled(c.P(hx, hy), c.P(hx + kw, hy + 34), IM_COL32(240, 120, 200, 255), 4 * c.s);
        Text(c, hx + 12, hy + 4, h.first, k);
        Text(c, hx + kw + 10, hy + 1, h.second, w);
        hx += kw + 10 + TextW(w, h.second) + gap;
    }
}

// A field on the game's white menu plate: small label, then the value (cyan plate + cursor while typing).
// `w` is the plate's width; its height follows the sprite (876x98). `value` null = shown, not editable.
// A hidden ImGui text box does the typing (keys, clipboard, IME, Tab to the next field); we draw the text.
struct FieldResult {
    bool enter = false;  // Enter pressed in it
    bool edited = false; // changed and left (save now)
};
int s_caret = 0; // cursor position (bytes) of the active text box, from its callback
int CaretCallback(ImGuiInputTextCallbackData* d)
{
    s_caret = d->CursorPos;
    return 0;
}
FieldResult Field(const Ctx& c, float x, float y, float w, const char* label, std::string* value, const char* shownValue,
    const char* hint, size_t maxLen, int key)
{
    float k = w / 876, h = 98 * k;
    FieldResult res;
    bool focused = false, hot = false;
    if (value) {
        char buf[256];
        strncpy_s(buf, value->c_str(), _TRUNCATE);
        ImGui::SetCursorScreenPos(c.P(x, y));
        ImGui::PushID(key);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
        ImGui::PushStyleColor(ImGuiCol_Text, 0);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, 0);
        ImGui::PushStyleColor(ImGuiCol_NavHighlight, 0);
        float fontH = ImGui::GetFontSize();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, std::max(0.0f, (h * c.s - fontH) / 2)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        ImGui::SetNextItemWidth(w * c.s);
        size_t room = std::min(sizeof(buf), maxLen + 1);
        res.enter = ImGui::InputText("##field", buf, room, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
            CaretCallback);
        focused = ImGui::IsItemActive();
        hot = ImGui::IsItemHovered();
        res.edited = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(6);
        ImGui::PopID();
        *value = buf;
    }
    float t = Ease(key, hot || focused);
    bool locked = !value;
    Spr(c, set::Common, focused ? "MENU_BASE_SEL" : "MENU_BASE", x, y, k, locked ? IM_COL32(215, 215, 220, 255) : White);
    if (t > 0)
        Spr(c, set::Common, "MENU_BASE_CURSOL", x - 6 * k, y - 2 * k, k, Alpha(White, t * (focused ? 1.0f : 0.6f)));
    Style l;
    l.font = Font::Thin;
    l.px = 26;
    l.spacing = 1;
    l.color = focused ? White : InkSoft;
    Text(c, x + 48 * k + 10, y + (h - 26) / 2 - 2, label, l);
    std::string text = value ? *value : std::string(shownValue ? shownValue : "");
    Style v;
    v.px = 32;
    v.spacing = 2;
    v.color = focused ? White : text.empty() ? IM_COL32(160, 162, 168, 255) : Ink;
    if (focused)
        v.outline = TealDark, v.outlineW = 2;
    const char* shown = text.empty() && !focused ? hint : text.c_str();
    float space = w - 48 * k - 150 - 70 * k; // up to the plate's slanted right end
    while (v.px > 22 && TextW(v, shown) > space)
        v.px -= 1, v.spacing = v.px / 16;
    c.dl->PushClipRect(c.P(x, y), c.P(x + 48 * k + 150 + space, y + h), true);
    Text(c, x + 48 * k + 150, y + (h - v.px) / 2 - 3, shown, v);
    if (focused && std::fmod(ImGui::GetTime(), 1.0) < 0.55) { // caret
        std::string before = text.substr(0, std::min<size_t>(text.size(), (size_t)std::max(0, s_caret)));
        float cx = x + 48 * k + 150 + (before.empty() ? 0 : TextW(v, before.c_str()) + v.spacing) + 2;
        c.dl->AddRectFilled(c.P(cx, y + h * 0.28f), c.P(cx + 3, y + h * 0.72f), White);
    }
    c.dl->PopClipRect();
    return res;
}

// The game's help window as a square card: light panel, colored strips on top and bottom (cyan or orange), a title.
struct Card {
    float x, y, w, h;
};
Card DrawCard(const Ctx& c, float x, float y, float w, float h, bool orange, const char* title)
{
    auto stretch = [&](const char* name, float sx, float sy, float sw, float sh, ImU32 tint = White) {
        if (const sprites::Sprite* sp = sprites::Find(set::Common, name))
            sprites::Draw(c.dl, *sp, c.P(sx, sy), c.P(sx + sw, sy + sh), tint);
    };
    c.dl->AddRectFilled(c.P(x + 10, y + 12), c.P(x + w + 10, y + h + 12), IM_COL32(0, 30, 50, 60)); // soft shadow
    stretch("WIN_BASE", x, y, w, h, Alpha(White, 0.96f));
    stretch(orange ? "WIN_BASE_TOP_Y" : "WIN_BASE_TOP_G", x, y, w, 30);
    stretch(orange ? "WIN_BASE_BOTTOM_Y" : "WIN_BASE_BOTTOM_G", x, y + h - 38, w, 38);
    Style st;
    st.px = 44;
    st.color = Ink;
    Text(c, x + 44, y + 58, title, st);
    return Card{ x, y, w, h };
}

// A small white plate button ("Copy", "Stop hosting"), cyan while hovered: the menu plate stretched to the label.
bool SmallButton(const Ctx& c, float x, float y, const char* label, int key)
{
    Style st;
    st.px = 28;
    st.spacing = 1;
    const float h = 60, w = TextW(st, label) + 90;
    Hit hit = HitTest(c, key, x, y, w, h);
    bool hot = hit.hovered, clicked = hit.clicked;
    float t = Ease(key, hot);
    if (const sprites::Sprite* sp = sprites::Find(set::Common, t > 0.5f ? "MENU_BASE_SEL" : "MENU_BASE"))
        sprites::Draw(c.dl, *sp, c.P(x, y), c.P(x + w, y + h));
    st.color = t > 0.5f ? White : Ink;
    if (t > 0.5f)
        st.outline = TealDark, st.outlineW = 2;
    Text(c, x + 45, y + (h - 28) / 2 - 3, label, st);
    return clicked;
}

void StatusLine(const Ctx& c, float x, float y, const std::string& text, ImU32 color)
{
    if (text.empty())
        return;
    Style st;
    st.font = Font::Thin;
    st.px = 30;
    st.spacing = 1;
    st.color = color;
    c.dl->AddRectFilled(c.P(x - 24, y - 10), c.P(x + TextW(st, text.c_str()) + 24, y + 30 + 12), IM_COL32(20, 30, 50, 170), 10 * c.s);
    Text(c, x, y, text.c_str(), st);
}

// A room in the server's list: a white plate row (cyan while hovered) with name, host, players and state.
bool RoomRow(const Ctx& c, const mp::RoomInfo& r, float x, float y, float w, int key)
{
    const float h = 74;
    Hit hit = HitTest(c, key, x, y, w, h);
    bool hot = hit.hovered;
    float t = Ease(key, hot);
    if (const sprites::Sprite* sp = sprites::Find(set::Common, t > 0.5f ? "MENU_BASE_SEL" : "MENU_BASE"))
        sprites::Draw(c.dl, *sp, c.P(x, y), c.P(x + w, y + h));
    if (t > 0)
        if (const sprites::Sprite* cur = sprites::Find(set::Common, "MENU_BASE_CURSOL"))
            sprites::Draw(c.dl, *cur, c.P(x - 5, y - 2), c.P(x + w + 5, y + h + 2), Alpha(White, t));
    bool lit = t > 0.5f;
    Style name;
    name.px = 34;
    name.color = lit ? White : Ink;
    if (lit)
        name.outline = TealDark, name.outlineW = 2;
    Text(c, x + 50, y + (h - 34) / 2 - 3, r.name.c_str(), name);
    Style info;
    info.font = Font::Thin;
    info.px = 26;
    info.spacing = 1;
    info.color = lit ? White : InkSoft;
    // Fixed columns: host, players, state (right-aligned to the plate's end).
    float ty = y + (h - 26) / 2 - 2;
    Text(c, x + w - 430, ty, r.host.c_str(), info);
    char players[16];
    snprintf(players, sizeof(players), "%d / 8", r.players);
    Text(c, x + w - 210 - TextW(info, players), ty, players, info);
    const char* state = r.playing ? "Playing" : "Waiting";
    Text(c, x + w - 60 - TextW(info, state), ty, state, info);
    return hit.clicked;
}

// Right-click menu of a player row (host only): our plate buttons in a small ImGui popup above the screen.
void PlayerMenu(const Ctx& c, const mp::State& s, int& menuPlayer, RoomAction& action)
{
    ImGui::PushStyleColor(ImGuiCol_PopupBg, 0);
    ImGui::PushStyleColor(ImGuiCol_Border, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginPopup("##playermenu")) {
        const mp::Player* p = nullptr;
        for (const mp::Player& q : s.players)
            if (q.id == menuPlayer)
                p = &q;
        ImVec2 at = ImGui::GetWindowPos();
        Ctx m{ ImGui::GetWindowDrawList(), c.o, c.s };
        float x = (at.x - c.o.x) / c.s, y = (at.y - c.o.y) / c.s;
        if (!p) {
            ImGui::CloseCurrentPopup();
        } else {
            m.dl->AddRectFilled(m.P(x, y), m.P(x + 300, y + 186), IM_COL32(20, 30, 50, 225), 12 * c.s);
            Style st;
            st.font = Font::Thin;
            st.px = 26;
            st.spacing = 1;
            st.color = IM_COL32(200, 215, 230, 255);
            Text(m, x + 22, y + 14, p->name.c_str(), st);
            if (SmallButton(m, x + 18, y + 50, "Give host", 201)) {
                action = { RoomAction::GiveHost, p->id };
                ImGui::CloseCurrentPopup();
            }
            if (SmallButton(m, x + 18, y + 116, "Kick", 202)) {
                action = { RoomAction::Kick, p->id };
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetCursorScreenPos(m.P(x + 300, y + 186)); // the popup's size
            ImGui::Dummy(ImVec2(1, 1));
        }
        ImGui::EndPopup();
    } else {
        menuPlayer = -1;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// The room's settings menu: toggles on plate buttons, in a small popup like the player menu.
void SettingsMenu(const Ctx& c, const RoomView& view, RoomAction& action)
{
    ImGui::PushStyleColor(ImGuiCol_PopupBg, 0);
    ImGui::PushStyleColor(ImGuiCol_Border, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginPopup("##roomsettings")) {
        ImVec2 at = ImGui::GetWindowPos();
        Ctx m{ ImGui::GetWindowDrawList(), c.o, c.s };
        float x = (at.x - c.o.x) / c.s, y = (at.y - c.o.y) / c.s;
        const float w = 470, h = 22 + 4 * 72 + 10;
        m.dl->AddRectFilled(m.P(x, y), m.P(x + w, y + h), IM_COL32(20, 30, 50, 225), 12 * c.s);
        struct Row { const char* label; RoomAction::Kind kind; } rows[] = {
            { view.ownOutfits ? "Outfits: my own" : "Outfits: the host's", RoomAction::ToggleOutfits },
            { view.showLeaderboard ? "Live leaderboard: on" : "Live leaderboard: off", RoomAction::ToggleLeaderboard },
            { view.showGaps ? "Score gaps: on" : "Score gaps: off", RoomAction::ToggleGaps },
            { view.browseSounds ? "Browse sounds: on" : "Browse sounds: off", RoomAction::ToggleBrowseSounds },
        };
        float ry = y + 22;
        int key = 210;
        for (const Row& r : rows) {
            if (SmallButton(m, x + 22, ry, r.label, key++))
                action = { r.kind }; // the menu stays open: toggle several in a row
            ry += 72;
        }
        ImGui::SetCursorScreenPos(m.P(x + w, y + h));
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

} // namespace

unsigned int DifficultyColor(int d)
{
    switch (d) {
    case 0: return IM_COL32(90, 200, 255, 255);  // easy: blue
    case 1: return IM_COL32(110, 225, 90, 255);  // normal: green
    case 2: return IM_COL32(255, 190, 60, 255);  // hard: orange
    case 3: return IM_COL32(255, 85, 100, 255);  // extreme: red
    case 4: return IM_COL32(205, 120, 255, 255); // extra extreme: purple
    }
    return IM_COL32(255, 255, 255, 255);
}

RoomAction DrawRoomScene(const mp::State& s, const RoomView& view)
{
    Ctx c = BeginScreen("##md_room");
    ImDrawList* dl = c.dl;
    RoomAction action;
    const bool host = s.hostId == s.myId;

    // ---- header: room name, player count, leave ----
    {
        Style st;
        st.px = 44;
        st.color = White;
        std::string room = "Room  " + s.roomName;
        Text(c, 60, 30, room.c_str(), st);
        char count[32];
        snprintf(count, sizeof(count), "Players  %d / 8", (int)s.players.size());
        st.font = Font::Thin;
        st.px = 34;
        float right = 1640;
        if (!view.inviteUrl.empty()) {
            const char* label = view.inviteCopied ? "Copied!" : "Copy invite";
            Style b;
            b.px = 28;
            b.spacing = 1;
            right -= TextW(b, label) + 90 + 20;
            if (SmallButton(c, right + 20, 24, label, 306))
                action = { RoomAction::CopyInvite };
        }
        float cw = TextW(st, count);
        Text(c, right - 40 - cw, 36, count, st);
        if (SmallButton(c, 1680, 24, "Leave", 301))
            action = { RoomAction::Leave };
    }

    // ---- left: players, staggered like song select's list; scrolls with the wheel past ~5 rows ----
    static int s_menuPlayer = -1;
    {
        const float top = 144, bottom = 796, step = 122, first = 150;
        static float scroll = 0, target = 0;
        float content = s.players.size() * step - (step - 114), view = bottom - first;
        float maxScroll = std::max(0.0f, content - view);
        ImVec2 m = ImGui::GetIO().MousePos;
        bool overList = ImGui::IsWindowHovered() && m.y >= c.P(0, top).y && m.y < c.P(0, bottom).y && m.x < c.P(1120, 0).x;
        if (overList && ImGui::GetIO().MouseWheel != 0)
            target -= ImGui::GetIO().MouseWheel * step;
        target = std::clamp(target, 0.0f, maxScroll);
        scroll += (target - scroll) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14);
        dl->PushClipRect(c.P(0, top), c.P(1120, bottom), true);
        float y = first - scroll;
        for (const mp::Player& p : s.players) {
            if (y > top - 130 && y < bottom) {
                Hit hit = PlayerRow(c, s, p, 40 + (y - first) / step * 26.0f, y, overList);
                if (hit.rightClicked && host && p.id != s.myId) {
                    s_menuPlayer = p.id;
                    ImGui::OpenPopup("##playermenu");
                }
            }
            y += step;
        }
        dl->PopClipRect();
        // The game's slanted scrollbar: dark track, white thumb (a slice of the mask sprite).
        const sprites::Sprite* track = sprites::Find(set::SongSelect, "SCRL_BASE");
        const sprites::Sprite* thumb = sprites::Find(set::SongSelect, "SCRL_BAR_MASK");
        if (maxScroll > 0 && track && thumb) {
            float k = view / track->size.y, tx = 1000, ty = first;
            sprites::Draw(dl, *track, c.P(tx, ty), c.P(tx + track->size.x * k, ty + view), Alpha(White, 0.8f));
            float f0 = scroll / content, f1 = (scroll + view) / content;
            sprites::Sprite part = *thumb;
            part.uv0.y = thumb->uv0.y + (thumb->uv1.y - thumb->uv0.y) * f0;
            part.uv1.y = thumb->uv0.y + (thumb->uv1.y - thumb->uv0.y) * f1;
            float tw = thumb->size.x * k, ox = (track->size.x - thumb->size.x) * k / 2;
            sprites::Draw(dl, part, c.P(tx + ox, ty + view * f0), c.P(tx + ox + tw, ty + view * f1));
        }
    }

    // ---- right: the song, room options, then the big button ----
    SongCard(c, s, view, 1290, 150);
    {
        float bx = 1160, by = 750;
        if (host) {
            if (SmallButton(c, bx, by, view.botRunning ? "Remove Miku Bot" : "Add Miku Bot", 302))
                action = { RoomAction::ToggleBot };
            bx += 330;
        }
        if (SmallButton(c, bx, by, "Settings", 303))
            ImGui::OpenPopup("##roomsettings");

        const mp::Player* me = nullptr;
        for (const mp::Player& p : s.players)
            if (p.id == s.myId)
                me = &p;
        bool ready = me && me->ready;
        bool busy = s.roomState == "playing";
        // The host starts a round by playing any song in song select: this button just gets the screen out of
        // the way. Everyone else says they're ready.
        if (host) {
            if (StartButton(c, 1250, 830, "Pick a song", nullptr, !busy, 304))
                action = { RoomAction::PickSong };
        } else if (StartButton(c, 1250, 830, ready ? "Not ready" : "Ready!", nullptr, !ready && !busy, 304)) {
            action = { RoomAction::ToggleReady };
        }
    }

    // ---- chat (bottom left, above the footer): the last lines, then the line being typed ----
    {
        dl->AddRectFilled(c.P(40, 812), c.P(1080, 988), IM_COL32(20, 30, 50, 150), 14 * c.s);
        Style st;
        st.font = Font::Thin;
        st.px = 28;
        st.spacing = 1;
        st.color = White;
        float ly = 988 - 16 - 28 - 34;
        int shown = 0;
        dl->PushClipRect(c.P(40, 812), c.P(1080, 988), true);
        for (auto it = s.chat.rbegin(); it != s.chat.rend() && shown < 4; ++it, ++shown) {
            std::string line = it->kind == mp::ChatLine::System ? "* " + it->text : it->name + ":  " + it->text;
            st.color = it->kind == mp::ChatLine::System ? IM_COL32(170, 210, 230, 255)
                : it->kind == mp::ChatLine::Error ? IM_COL32(255, 150, 150, 255) : White;
            Text(c, 64, ly, line.c_str(), st);
            ly -= 34;
        }
        dl->PopClipRect();
        // Enter starts typing; Enter again sends (an empty line just stops typing).
        static std::string s_chat;
        static bool s_keepTyping = false; // a line was just sent: the box stays open for the next one
        if (s_keepTyping || (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
            ImGui::SetKeyboardFocusHere();
        s_keepTyping = false;
        ImGui::SetCursorScreenPos(c.P(56, 988 - 50));
        ImGui::PushID(305);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
        ImGui::PushStyleColor(ImGuiCol_Text, 0);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, 0);
        ImGui::SetNextItemWidth(1000 * c.s);
        char buf[256];
        strncpy_s(buf, s_chat.c_str(), _TRUNCATE);
        bool sent = ImGui::InputText("##chat", buf, 200, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
            CaretCallback);
        bool active = ImGui::IsItemActive();
        ImGui::PopStyleColor(5);
        ImGui::PopID();
        s_chat = buf;
        Style in = st;
        in.color = active ? White : IM_COL32(150, 165, 185, 255);
        std::string line = active ? "> " + s_chat : s_chat.empty() ? "Press Enter to chat" : "> " + s_chat;
        Text(c, 64, 988 - 16 - 28, line.c_str(), in);
        if (active && std::fmod(ImGui::GetTime(), 1.0) < 0.55) {
            std::string before = "> " + s_chat.substr(0, std::min<size_t>(s_chat.size(), (size_t)std::max(0, s_caret)));
            float cx = 64 + TextW(in, before.c_str()) + 3;
            dl->AddRectFilled(c.P(cx, 988 - 16 - 28), c.P(cx + 2, 988 - 16), White);
        }
        if (sent) {
            if (!s_chat.empty()) {
                action = { RoomAction::Chat, -1, s_chat };
                s_keepTyping = true;
            }
            s_chat.clear();
        }
    }

    PlayerMenu(c, s, s_menuPlayer, action);
    SettingsMenu(c, view, action);
    Footer(c, { { "F8", "Hide" }, { "Enter", "Chat" }, { "Right click", "Player menu" } });
    ImGui::End();
    return action;
}

ConnectAction DrawConnectScene(const mp::State& s, ConnectView& v)
{
    Ctx c = BeginScreen("##md_connect");
    ConnectAction action = ConnectAction::None;
    const bool online = s.conn == mp::Conn::Online, connecting = s.conn == mp::Conn::Connecting;

    // ---- header: you (picture + name), connection state on the right ----
    {
        mp::Player me;
        me.name = *v.name;
        me.steamId = v.steamId;
        c.dl->AddRectFilled(c.P(57, 17), c.P(57 + 76 + 6, 17 + 76 + 6), White);
        Picture(c, me, 60, 20, 76, 1);
        Style st;
        st.px = 44;
        st.color = White;
        Text(c, 160, 32, v.name->empty() ? "Your name" : v.name->c_str(), st);
        st.font = Font::Thin;
        st.px = 30;
        st.spacing = 1;
        std::string state = s.reconnecting ? "Connection lost, reconnecting..."
            : online ? "Connected to  " + v.serverShown : connecting ? "Connecting..." : "Not connected";
        st.color = s.reconnecting ? IM_COL32(255, 190, 90, 255)
            : online ? IM_COL32(150, 240, 230, 255) : IM_COL32(220, 225, 235, 255);
        Text(c, 1880 - TextW(st, state.c_str()), 40, state.c_str(), st);

        // Both releases, so a mismatch is easy to spot when something doesn't work.
        std::string ver = std::string("Client ") + MULTIDIVA_VERSION;
        if (online)
            ver += "   Server " + (s.serverVersion.empty() ? std::string("unknown (older)") : s.serverVersion);
        st.px = 24;
        st.color = IM_COL32(190, 198, 212, 255);
        Text(c, 1880 - TextW(st, ver.c_str()), 86, ver.c_str(), st);
    }

    const float top = 170, cardH = 640;
    if (!online) {
        // ---- join: name + server, one button ----
        Card card = DrawCard(c, 80, top, 860, cardH, false, "Join a game");
        FieldResult name = Field(c, card.x + 40, card.y + 150, 780, "Name", connecting ? nullptr : v.name, v.name->c_str(),
            "type a name", 25, 101);
        FieldResult server = Field(c, card.x + 40, card.y + 260, 780, "Server", connecting ? nullptr : v.server,
            v.server->c_str(), "address from the host", 128, 102);
        if (name.edited || server.edited)
            action = ConnectAction::Save;
        if (!connecting && !v.rejoinRoom.empty()) {
            std::string label = "Rejoin " + v.rejoinRoom;
            if (SmallButton(c, card.x + 50, card.y + 372, label.c_str(), 112))
                action = ConnectAction::Rejoin;
        }
        if (StartButton(c, card.x + (card.w - 530) / 2, card.y + cardH - 170, connecting ? "Connecting..." : "Join", nullptr,
                !connecting, 104) || name.enter || server.enter)
            if (!connecting)
                action = ConnectAction::Connect;
    } else {
        // ---- the server's rooms: click one to join, or make a new one ----
        Card card = DrawCard(c, 80, top, 860, cardH, false, "Rooms");
        Style count;
        count.font = Font::Thin;
        count.px = 28;
        count.spacing = 1;
        count.color = InkSoft;
        char n[32];
        snprintf(n, sizeof(n), "%d open", (int)s.rooms.size());
        Text(c, card.x + card.w - 44 - TextW(count, n), card.y + 70, n, count);
        float ry = card.y + 140;
        if (s.rooms.empty()) {
            Style st;
            st.font = Font::Thin;
            st.px = 30;
            st.spacing = 1;
            st.color = InkSoft;
            Text(c, card.x + 50, ry + 20, "No rooms yet: create one and invite your friends.", st);
        }
        int key = 120;
        for (const mp::RoomInfo& r : s.rooms) {
            if (ry > card.y + cardH - 250)
                break; // TODO: scroll like the player list once servers get busy
            if (RoomRow(c, r, card.x + 40, ry, 780, key++)) {
                v.joinRoom = r.name;
                action = ConnectAction::JoinRoom;
            }
            ry += 88;
        }
        float by = card.y + cardH - 170;
        if (StartButton(c, card.x + 40, by, "Create room", nullptr, true, 110))
            action = ConnectAction::CreateRoom;
        if (SmallButton(c, card.x + 40 + 530 + 40, by + 12, "Disconnect", 111))
            action = ConnectAction::Disconnect;
    }

    // ---- host: one button, then the address to share while it runs ----
    {
        Card card = DrawCard(c, 980, top, 860, cardH, true, "Host a game");
        Style st;
        st.font = Font::Thin;
        st.px = 30;
        st.spacing = 1;
        st.color = InkSoft;
        if (v.hostPhase == ConnectView::HostPhase::Ready) {
            Text(c, card.x + 50, card.y + 150, "Friends join with this address:", st);
            Field(c, card.x + 40, card.y + 200, 780, "Address", nullptr, v.hostUrl.c_str(), "", 0, 105);
            if (SmallButton(c, card.x + 40, card.y + 310, v.copied ? "Copied!" : "Copy", 106))
                action = ConnectAction::CopyAddress;
            if (SmallButton(c, card.x + 40 + 220, card.y + 310, "Stop hosting", 107))
                action = ConnectAction::StopHosting;
        } else {
            if (v.hostPhase == ConnectView::HostPhase::Busy) {
                Text(c, card.x + 50, card.y + 150, v.hostMessage.c_str(), st);
            } else {
                Text(c, card.x + 50, card.y + 150, "Runs a server on this PC with a free Cloudflare tunnel,", st);
                Text(c, card.x + 50, card.y + 192, "so friends can join from anywhere.", st);
            }
            if (v.hostPhase == ConnectView::HostPhase::Failed)
                StatusLine(c, card.x + 50, card.y + 260, v.hostMessage, IM_COL32(255, 120, 120, 255));
            const char* label = v.hostPhase == ConnectView::HostPhase::Busy ? "Starting..."
                : v.permanent ? "Host (my address)" : "Host a server";
            bool busy = v.hostPhase == ConnectView::HostPhase::Busy;
            if (StartButton(c, card.x + (card.w - 530) / 2, card.y + cardH - 170, label, nullptr, !busy, 108) && !busy)
                action = ConnectAction::Host;
            if (!busy && SmallButton(c, card.x + 50, card.y + cardH - 260, "Permanent address...", 109))
                action = ConnectAction::AddressSetup;
        }
    }

    // ---- what went wrong / why we're here ----
    if (!s.connError.empty())
        StatusLine(c, 110, top + cardH + 40, s.connError, IM_COL32(255, 120, 120, 255));
    else if (!s.roomNotice.empty())
        StatusLine(c, 110, top + cardH + 40, s.roomNotice, IM_COL32(255, 200, 120, 255));
    else if (!v.notice.empty())
        StatusLine(c, 110, top + cardH + 40, v.notice, IM_COL32(255, 200, 120, 255));

    if (online)
        Footer(c, { { "F8", "Hide" }, { "Click", "Join a room" } });
    else
        Footer(c, { { "F8", "Hide" }, { "Tab", "Next field" }, { "Enter", "Join" } });
    ImGui::End();
    return action;
}

} // namespace md::ui
