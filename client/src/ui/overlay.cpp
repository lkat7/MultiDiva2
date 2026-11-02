#include "overlay.h"
#include "bug_report.h"
#include "debug_panel.h"
#include "glyphs.h"
#include "gpu_capture.h"
#include "game_sprites.h"
#include "input_block.h"
#include "leaderboard.h"
#include "multiplayer_ui.h"
#include "pause_hint.h"
#include "../core/crash.h"
#include "../core/log.h"
#include "../core/mod.h"
#include "../game/launch.h"
#include "../game/session.h"

#include <cmath>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace md::ui {

static bool s_ready = false;
static HWND s_window = nullptr;
static WNDPROC s_originalWndProc = nullptr;
static ID3D11Device* s_device = nullptr;
static ID3D11DeviceContext* s_context = nullptr;
static Fonts s_fonts;

static bool s_debugPanelOpen = false;
static bool s_lobbyOpen = false;
// F9: hide every MultiDiva window (e.g. while someone watches). Networking keeps running.
static bool s_hidden = false;
static bool s_f9WasDown = false;
static constexpr DWORD KeyboardBlockGraceMs = 400;
static bool s_f8WasDown = false;

const Fonts& GetFonts() { return s_fonts; }
ID3D11Device* GetDevice() { return s_ready ? s_device : nullptr; }

static bool IsMouseMessage(UINT msg)
{
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
}

static bool IsKeyboardMessage(UINT msg)
{
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // The game may quit without unloading mods normally: its window going away counts as a normal exit.
    if (msg == WM_DESTROY)
        crash::MarkCleanExit();
    if (s_ready) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && IsMouseMessage(msg))
            return TRUE;
        if (io.WantTextInput && IsKeyboardMessage(msg))
            return TRUE;
    }
    return CallWindowProcW(s_originalWndProc, hWnd, msg, wParam, lParam);
}

static ImFont* LoadExo(const std::string& path, bool exists, float size)
{
    return exists ? ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size) : s_fonts.regular;
}

// UI scale relative to 720p, so the overlay reads the same at any resolution.
static float UiScale(UINT height)
{
    float scale = height / 720.0f;
    return scale < 1.0f ? 1.0f : scale;
}

static float s_uiScale = 1.0f;
static float s_screenHeight = 720.0f;

// Segoe UI plus, merged in, the requested characters it lacks from the Windows fonts that have them.
static ImFont* AddTextFont(const std::string& segoe, float size)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = io.Fonts->AddFontFromFileTTF(segoe.c_str(), size, nullptr, glyphs::PrimaryRanges());
    if (!font)
        return nullptr;
    for (int i = 0; i < glyphs::FallbackCount(); i++) {
        glyphs::Fallback f = glyphs::GetFallback(i);
        if (!f.data)
            continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF((void*)f.data, f.size, size, &cfg, f.ranges);
    }
    return font;
}

float CountdownFontPx(float screenHeight) { return std::round(screenHeight * 0.24f); }

static void LoadFonts(float scale, float screenHeight)
{
    ImGuiIO& io = ImGui::GetIO();
    // Segoe UI ships with every Windows install; the ImGui pixel font is the fallback.
    char windir[MAX_PATH];
    GetWindowsDirectoryA(windir, MAX_PATH);
    std::string segoe = std::string(windir) + "\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(segoe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        s_fonts.regular = AddTextFont(segoe, 15.0f * scale);
        s_fonts.cardSmall = io.Fonts->AddFontFromFileTTF(segoe.c_str(), CardSmallFontPx(screenHeight));
    }
    // Leaderboard card names: large regular weight, like osu!'s.
    if (GetFileAttributesA(segoe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        s_fonts.cardName = AddTextFont(segoe, CardNameFontPx(screenHeight));
        s_fonts.hintTitle = io.Fonts->AddFontFromFileTTF(segoe.c_str(), HintTitleFontPx(screenHeight));
        s_fonts.hintLine = io.Fonts->AddFontFromFileTTF(segoe.c_str(), HintLineFontPx(screenHeight));
    }
    {
        // The countdown digits: Segoe UI Bold at the size they are drawn (no stretching = no blur), digits only.
        static const ImWchar digits[] = { '0', '9', 0 };
        std::string bold = std::string(windir) + "\\Fonts\\segoeuib.ttf";
        const std::string& file = GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES ? bold : segoe;
        if (GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES)
            s_fonts.countdown = io.Fonts->AddFontFromFileTTF(file.c_str(), CountdownFontPx(screenHeight), nullptr, digits);
    }
    if (!s_fonts.regular) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale;
        s_fonts.regular = io.Fonts->AddFontDefault(&cfg);
    }
    if (!s_fonts.cardSmall)
        s_fonts.cardSmall = s_fonts.regular;
    if (!s_fonts.cardName)
        s_fonts.cardName = s_fonts.regular;
    if (!s_fonts.countdown)
        s_fonts.countdown = s_fonts.regular;
    // The full-screen screens' fallback fonts (used until the game's own font sheet has been found): never null.
    if (!s_fonts.sceneTitle)
        s_fonts.sceneTitle = s_fonts.hintTitle ? s_fonts.hintTitle : s_fonts.regular;
    if (!s_fonts.sceneButton)
        s_fonts.sceneButton = s_fonts.hintLine ? s_fonts.hintLine : s_fonts.regular;
    if (!s_fonts.sceneText)
        s_fonts.sceneText = s_fonts.regular;
    if (!s_fonts.sceneSmall)
        s_fonts.sceneSmall = s_fonts.cardSmall ? s_fonts.cardSmall : s_fonts.regular;
    if (!s_fonts.hintTitle)
        s_fonts.hintTitle = s_fonts.regular;
    if (!s_fonts.hintLine)
        s_fonts.hintLine = s_fonts.regular;

    std::wstring exoW = ModDir() + L"\\assets\\font\\Exo2-Regular.ttf";
    bool exists = GetFileAttributesW(exoW.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::string exo = ModPathUtf8(L"assets\\font\\Exo2-Regular.ttf");
    if (!exists)
        Log("font %s not found, using ImGui default", exo.c_str());

    s_fonts.exoSmall = LoadExo(exo, exists, 16);
    s_fonts.exoMedium = LoadExo(exo, exists, 20);
    s_fonts.exoLarge = LoadExo(exo, exists, 30);
    s_fonts.exoXL = LoadExo(exo, exists, 40);
}

static void ApplyStyle(float scale)
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 8;
    style.FrameRounding = 4;
    style.GrabRounding = 4;
    style.ChildRounding = 4;
    style.WindowBorderSize = 0;
    style.WindowPadding = ImVec2(12, 10);
    style.ItemSpacing = ImVec2(8, 6);

    const ImVec4 teal(0.22f, 0.77f, 0.73f, 1.0f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.25f);
    c[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.11f, 0.13f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.30f, 0.31f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.77f, 0.73f, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.77f, 0.73f, 0.45f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.77f, 0.73f, 0.60f);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.77f, 0.73f, 0.35f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.77f, 0.73f, 0.55f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.77f, 0.73f, 0.80f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.24f, 0.27f, 1.0f);
    c[ImGuiCol_CheckMark] = teal;
    c[ImGuiCol_SliderGrab] = teal;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    style.ScaleAllSizes(scale);
}

void OverlayInit(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (s_ready)
        return;

    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow) {
        Log("overlay: swap chain has no output window, overlay disabled");
        return;
    }

    s_window = desc.OutputWindow;
    s_device = device;
    s_context = context;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't drop imgui.ini into the game folder

    if (!ImGui_ImplWin32_Init(s_window) || !ImGui_ImplDX11_Init(s_device, s_context)) {
        Log("overlay: ImGui backend init failed, overlay disabled");
        return;
    }

    float scale = UiScale(desc.BufferDesc.Height);
    s_uiScale = scale;
    s_screenHeight = (float)desc.BufferDesc.Height;
    RequestGlyphs();
    glyphs::TakeDirty();
    glyphs::BuildRanges();
    LoadFonts(scale, s_screenHeight);
    ApplyStyle(scale);

    s_originalWndProc = (WNDPROC)SetWindowLongPtrW(s_window, GWLP_WNDPROC, (LONG_PTR)WndProc);
    InstallInputBlock();
    capture::Install(s_context);
    s_ready = true;
    Log("overlay ready (%ux%u). F8: multiplayer, F9: hide all, Ctrl+F8: debug panel.", desc.BufferDesc.Width, desc.BufferDesc.Height);
}

static void PollHotkeys()
{
    // Ctrl+F8 for the debug panel: F7 is New Classics' chart style key.
    bool focused = GetForegroundWindow() == s_window;
    bool ctrl = RealKeyDown(VK_CONTROL);
    bool f8Down = focused && RealKeyDown(VK_F8);
    if (f8Down && !s_f8WasDown) {
        if (ctrl)
            s_debugPanelOpen = !s_debugPanelOpen;
        else
            s_lobbyOpen = !s_lobbyOpen;
    }
    s_f8WasDown = f8Down;

    bool f9Down = focused && RealKeyDown(VK_F9);
    if (f9Down && !s_f9WasDown) {
        s_hidden = !s_hidden;
        Log("overlay %s", s_hidden ? "hidden (F9)" : "shown (F9)");
    }
    s_f9WasDown = f9Down;
}

void OverlayFrame(IDXGISwapChain* swapChain)
{
    if (!s_ready)
        return;

    // Research capture (Ctrl+F8 panel): the game's frame lies between our frames, so stop here and start after ours.
    capture::OnOverlayFrameStart();
    struct CaptureEnd {
        ~CaptureEnd() { capture::OnOverlayFrameEnd(); }
    } captureEnd;

    PollHotkeys();

    // The lobby gets out of the way during a song and comes back on song select (only if it was open).
    static bool s_wasInSong = false, s_reopenLobby = false;
    if (g_session.inSong && !s_wasInSong && s_lobbyOpen) {
        s_lobbyOpen = false;
        s_reopenLobby = true;
    }
    if (!g_session.inSong && s_reopenLobby && launch::InSongSelect()) {
        s_lobbyOpen = true;
        s_reopenLobby = false;
    }
    if (s_lobbyOpen)
        s_reopenLobby = false; // opened by hand meanwhile
    s_wasInSong = g_session.inSong;

    // Uses last frame's ImGui state, which is what the game reads during this frame.
    // Pressing Enter to send closes the text field on the key-down, while the key is still held,
    // so keep blocking briefly after typing ends or the game sees that Enter.
    static DWORD s_lastTyping = 0;
    if (ImGui::GetIO().WantTextInput)
        s_lastTyping = GetTickCount();
    // A full-screen multiplayer screen covers the game: none of its keys may reach the game (arrows and Enter
    // would drive song select behind it).
    bool screen = s_lobbyOpen && !s_hidden && LobbyIsFullScreen();
    SetKeyboardBlocked(screen || GetTickCount() - s_lastTyping < KeyboardBlockGraceMs);

    // New characters on screen or typed: rebuild the font atlas (the DX11 backend re-uploads it in NewFrame).
    RequestGlyphs();
    for (ImWchar c : ImGui::GetIO().InputQueueCharacters)
        glyphs::RequestChar(c);
    if (glyphs::TakeDirty()) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui::GetIO().Fonts->Clear();
        s_fonts = Fonts{};
        glyphs::BuildRanges();
        LoadFonts(s_uiScale, s_screenHeight);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    sprites::Tick(); // copy the game's sprite sets once it has loaded them
    if (!s_hidden) {
        if (s_lobbyOpen)
            DrawLobby(&s_lobbyOpen);
        DrawRoundOverlays();
        if (sprites::TestEnabled())
            sprites::DrawTest();
        if (s_debugPanelOpen)
            DrawDebugPanel(&s_debugPanelOpen);
        DrawBugReport();
    }
    EndFrameEscape();

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData->CmdListsCount == 0)
        return;

    // The back buffer view is created per frame so we never hold a reference across
    // the game's ResizeBuffers calls.
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)))
        return;
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = s_device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    if (FAILED(hr))
        return;

    // ImGui's DX11 backend restores most pipeline state but not the output merger targets.
    ID3D11RenderTargetView* prevRtv = nullptr;
    ID3D11DepthStencilView* prevDsv = nullptr;
    s_context->OMGetRenderTargets(1, &prevRtv, &prevDsv);

    s_context->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(drawData);

    s_context->OMSetRenderTargets(1, &prevRtv, prevDsv);
    if (prevRtv) prevRtv->Release();
    if (prevDsv) prevDsv->Release();
    rtv->Release();
}

void OverlayShutdown()
{
    if (!s_ready)
        return;
    s_ready = false;

    if (s_originalWndProc)
        SetWindowLongPtrW(s_window, GWLP_WNDPROC, (LONG_PTR)s_originalWndProc);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

} // namespace md::ui
