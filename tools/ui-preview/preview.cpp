// Renders the in-game leaderboard offscreen (D3D11 WARP) over a game screenshot and saves PNGs,
// so the overlay's look can be checked without launching the game.
//
//   ui-preview.exe <background.png> <avatars dir> <out dir> [room|test] [sprite pack dir]
//
// The sprite pack (the game's decoded UI sheets + font tables) is made locally by a research script and never
// committed or shipped; without it, game sprites and the game font are simply missing from the render.
// Avatars dir: 1.png, 2.png, ... used as fake Steam avatars for steam ids 1, 2, ...
#include <d3d11.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "core/config.h"
#include "ui/avatars.h"
#include "ui/game_sprites.h"
#include "ui/glyphs.h"
#include "game/pause_guard.h"
#include "ui/leaderboard.h"
#include "ui/pause_hint.h"
#include "ui/room_scene.h"
#include "game/titles.h"
#include "ui/overlay.h"
#include "ui/texture.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <stb_image.h>

// ---- stand-ins for the parts of the mod that need the game ----

static ID3D11Device* g_device = nullptr;
static md::ui::Fonts g_fonts;
static md::Config g_config;
static std::string g_avatarDir;

namespace md {
void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}
Config& GetConfig() { return g_config; }
namespace ui {
const Fonts& GetFonts() { return g_fonts; }
ID3D11Device* GetDevice() { return g_device; }
ID3D11ShaderResourceView* Avatar(uint64_t steamId)
{
    static std::map<uint64_t, Texture> cache;
    if (!steamId)
        return nullptr;
    auto it = cache.find(steamId);
    if (it == cache.end()) {
        Texture t;
        LoadTextureFromFile(g_device, (g_avatarDir + "\\" + std::to_string(steamId) + ".png").c_str(), t);
        it = cache.emplace(steamId, t).first;
    }
    return it->second.srv;
}
// Players flagged hasAvatar get the bot's built-in picture (what the server would serve).
ID3D11ShaderResourceView* PlayerAvatar(const mp::Player& p)
{
    static Texture bot;
    if (ID3D11ShaderResourceView* tex = Avatar(p.steamId))
        return tex;
    if (p.hasAvatar && !bot.srv)
        LoadTextureFromFile(g_device, "server\\cmd\\bot\\miku.png", bot);
    return p.hasAvatar ? bot.srv : nullptr;
}
} // namespace ui
} // namespace md

// A pause was blocked 1.5 s ago: the hint shows with its second line already "armed".
namespace md::pause {
State GetState()
{
    State st;
    st.hint = Hint::Blocked;
    st.hintTick = GetTickCount() - 1500;
    return st;
}
} // namespace md::pause

namespace md::titles {
std::string Find(int32_t pv) { return pv == 54 ? "Roshin Yukai" : std::string(); }
std::string Describe(int32_t pv) { std::string t = Find(pv); return t.empty() ? "PV " + std::to_string(pv) : t; }
} // namespace md::titles

// A room between rounds: we host, Miku Bot is ready, one player lacks the highlighted song, one isn't ready.
static md::mp::State FakeRoom()
{
    md::mp::State s;
    s.myId = 1;
    s.hostId = 1;
    s.inRoom = true;
    s.roomName = "miku";
    s.roomState = "lobby";
    s.players = { { 1, "littl", true, false, false, 1 }, { 2, "Miku Bot", true, false, false, 0, true },
        { 3, "salamander77", false, false, false, 2, false, true }, { 4, "TimboPVP", false, false, false, 3 },
        { 5, "初音ミク", true, false, false, 0 }, { 6, "Rin & Len", false, true, false, 0 }, { 7, "Luka", true, false, false, 0 } };
    s.pick = md::mp::Pick{ 54, 2, 0, "Roshin Yukai" };
    md::mp::FinalResult a, b;
    a.id = 1, a.score = 477550, a.completion = 81.40;
    b.id = 2, b.score = 512300, b.completion = 88.12;
    s.results = { a, b };
    s.hover = 54;
    s.chat = { { "", "Miku Bot joined", md::mp::ChatLine::System }, { "littl", "ready when you are!" },
        { "salamander77", "i don't have that one :(" }, { "", "salamander77 joined", md::mp::ChatLine::System },
        { "Miku Bot", "let's go!" } };
    return s;
}

static md::mp::State FakeState(bool swapped)
{
    md::mp::State s;
    s.myId = 1;
    s.inRound = true;
    s.players = { { 1, "littl", true, true, true, 1 }, { 2, "MikuBot", true, true, true, 0, true },
        { 3, "双重音律", true, true, true, 2 }, { 4, "初音ミク", true, true, true, 3 } };
    // Second capture: littl overtakes salamander77, MikuBot breaks a long combo.
    if (!swapped)
        s.scores = { { 3, "双重音律", 3415988, 341, 62.48, false }, { 1, "littl", 3399660, 408, 61.93, false },
            { 4, "初音ミク", 3392708, 365, 61.70, false }, { 2, "MikuBot", 2684916, 325, 48.05, false } };
    else
        s.scores = { { 1, "littl", 3421130, 410, 62.71, false }, { 3, "双重音律", 3418020, 342, 62.55, false },
            { 4, "初音ミク", 3395580, 366, 61.77, false }, { 2, "MikuBot", 2684916, 0, 48.05, false } };
    s.scores.back().lag = true; // MikuBot's updates stopped: the card dims and shows the LAG chip
    s.players[0].wins = 2;
    s.rounds = 3;
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: ui-preview <background.png> <avatars dir> <out dir>\n");
        return 1;
    }
    g_avatarDir = argv[2];
    const std::string mode = argc > 4 ? argv[4] : "";
    const bool roomMode = mode == "room", testMode = mode == "test";
    const bool connectMode = mode == "connect" || mode == "connect2", hosting = mode == "connect2";
    std::string outDir = argv[3];

    int bgW = 0, bgH = 0;
    unsigned char* bg = stbi_load(argv[1], &bgW, &bgH, nullptr, 4);
    if (!bg) {
        fprintf(stderr, "can't load %s\n", argv[1]);
        return 1;
    }

    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &ctx)))
        return 1;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = bgW;
    td.Height = bgH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    g_device->CreateTexture2D(&td, nullptr, &rt);
    ID3D11RenderTargetView* rtv = nullptr;
    g_device->CreateRenderTargetView(rt, nullptr, &rtv);
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    g_device->CreateTexture2D(&td, nullptr, &staging);

    md::ui::Texture bgTex;
    md::ui::CreateTextureRGBA(g_device, bg, bgW, bgH, bgTex);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)bgW, (float)bgH);
    float h = (float)bgH;
    g_fonts.regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f * h / 720.0f);
    g_fonts.cardSmall = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", md::ui::CardSmallFontPx(h));
    // Card names with the CJK fallback fonts merged in, as the overlay does.
    for (const md::mp::LiveScore& p : FakeState(false).scores)
        md::ui::glyphs::RequestText(p.name);
    md::ui::glyphs::BuildRanges();
    g_fonts.cardName = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", md::ui::CardNameFontPx(h), nullptr,
        md::ui::glyphs::PrimaryRanges());
    for (int i = 0; i < md::ui::glyphs::FallbackCount(); i++) {
        md::ui::glyphs::Fallback f = md::ui::glyphs::GetFallback(i);
        if (!f.data)
            continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF((void*)f.data, f.size, md::ui::CardNameFontPx(h), &cfg, f.ranges);
    }
    const char* rounded = "C:\\Windows\\Fonts\\ARLRDBD.TTF";
    g_fonts.sceneTitle = io.Fonts->AddFontFromFileTTF(rounded, md::ui::SceneTitlePx(h));
    g_fonts.sceneButton = io.Fonts->AddFontFromFileTTF(rounded, md::ui::SceneButtonPx(h));
    g_fonts.sceneText = io.Fonts->AddFontFromFileTTF(rounded, md::ui::SceneTextPx(h));
    g_fonts.sceneSmall = io.Fonts->AddFontFromFileTTF(rounded, md::ui::SceneSmallPx(h));
    g_fonts.hintTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", md::ui::HintTitleFontPx(h));
    g_fonts.hintLine = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", md::ui::HintLineFontPx(h));
    ImGui_ImplDX11_Init(g_device, ctx);
    if (argc > 5 && !md::ui::sprites::LoadPack(argv[5]))
        fprintf(stderr, "can't load the sprite pack in %s\n", argv[5]);

    // Frame 0..59 settled, then swap; capture settled (frame 59) and ~0.15 s into the swap (frame 69).
    for (int frame = 0; frame < 70; frame++) {
        io.DeltaTime = 1.0f / 60.0f;
        // Room: the mouse rests on the second player (hover look), then on the start button.
        float k = (float)bgH / 1080.0f, ox = (bgW - 1920 * k) / 2;
        // Second capture: Settings clicked (its menu open).
        io.MousePos = frame < 60 ? ImVec2(ox + 400 * k, 330 * k) : ImVec2(ox + 1580 * k, 780 * k);
        io.MouseDown[0] = roomMode && frame == 61;
        if (connectMode)
            io.MousePos = hosting ? ImVec2(ox + 500 * k, 437 * k) : ImVec2(ox + 500 * k, 700 * k); // a room / Join
        io.MouseWheel = roomMode && frame == 20 ? -1.0f : 0.0f; // one notch down: the list scrolls a row
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)bgTex.srv, ImVec2(0, 0), io.DisplaySize);
        if (connectMode) {
            // connect: offline, typing a room name, mouse on Join. connect2: connected, hosting on a tunnel.
            md::mp::State st;
            st.conn = hosting ? md::mp::Conn::Online : md::mp::Conn::Offline;
            md::ui::ConnectView v;
            static std::string name = "littl", server;
            server = hosting ? "wss://multidiva.cachee.win" : "wss://wiggly-miku.trycloudflare.com";
            v.name = &name;
            v.server = &server;
            v.steamId = 1;
            v.serverShown = "multidiva.cachee.win";
            if (!hosting) { // after a drop
                st.connError = "Connection to the server was lost.";
                v.rejoinRoom = "kat's room";
            }
            if (hosting) {
                v.hostPhase = md::ui::ConnectView::HostPhase::Ready;
                v.hostUrl = "wss://multidiva.cachee.win";
                st.rooms = { { "littl's room", "littl", 3, false }, { "miku", "Miku Bot", 1, false },
                    { "late night EXEX", "salamander77", 4, true } };
            }
            md::ui::DrawConnectScene(st, v);
        } else if (testMode) {
            md::ui::sprites::DrawTest();
        } else if (roomMode) {
            md::ui::RoomView view;
            view.status = "salamander77 doesn't have Roshin Yukai and would sit it out.";
            view.statusTone = md::ui::RoomView::Tone::Bad;
            view.songTitle = "Roshin Yukai";
            view.songDifficulty = 2;
            view.inviteUrl = "wss://multidiva.cachee.win";
            md::ui::DrawRoomScene(FakeRoom(), view);
        } else {
            md::mp::State s = FakeState(frame >= 60);
            md::ui::DrawLeaderboard(s);
            md::ui::DrawPauseHint();
        }
        ImGui::Render();

        if (frame != 59 && frame != 69)
            continue;
        float clear[4] = { 0, 0, 0, 1 };
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT viewport = { 0, 0, (float)bgW, (float)bgH, 0, 1 };
        ctx->RSSetViewports(1, &viewport);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ctx->CopyResource(staging, rt);
        D3D11_MAPPED_SUBRESOURCE map;
        ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
        std::string out = outDir + (connectMode ? "\\" + mode + (frame == 59 ? ".png" : "_b.png")
                                   : testMode ? (frame == 59 ? "\\test.png" : "\\test_2.png")
                                   : roomMode ? (frame == 59 ? "\\room.png" : "\\room_2.png")
                                              : (frame == 59 ? "\\leaderboard.png" : "\\leaderboard_swap.png"));
        stbi_write_png(out.c_str(), bgW, bgH, 4, map.pData, (int)map.RowPitch);
        ctx->Unmap(staging, 0);
        printf("wrote %s\n", out.c_str());
    }
    return 0;
}
