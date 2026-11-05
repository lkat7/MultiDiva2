#pragma once
#include <d3d11.h>
#include <dxgi.h>

struct ImFont;

namespace md::ui {

// Driven by DivaModLoader's D3DInit / OnFrame exports; no Present hook of our own.
void OverlayInit(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context);
void OverlayFrame(IDXGISwapChain* swapChain);
void OverlayShutdown();

struct Fonts {
    ImFont* regular = nullptr; // Segoe UI (ImGui default font if missing)
    ImFont* cardName = nullptr;  // leaderboard player names
    ImFont* cardSmall = nullptr; // leaderboard score / combo line
    ImFont* sceneTitle = nullptr;  // room scene (room_scene.h): rounded heavy font like the game's menus
    ImFont* sceneButton = nullptr;
    ImFont* sceneText = nullptr;
    ImFont* sceneSmall = nullptr;
    ImFont* hintTitle = nullptr; // pause hint card
    ImFont* hintLine = nullptr;
    ImFont* exoSmall = nullptr; // Exo2 sizes fall back to `regular` if the TTF is missing
    ImFont* exoMedium = nullptr;
    ImFont* exoLarge = nullptr;
    ImFont* exoXL = nullptr;
    ImFont* countdown = nullptr; // the 3-2-1: digits only, baked at CountdownFontPx so the big numbers stay sharp
};
const Fonts& GetFonts();
float CountdownFontPx(float screenHeight);
ID3D11Device* GetDevice(); // the game's device, null before D3DInit

} // namespace md::ui
