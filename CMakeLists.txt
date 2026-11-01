cmake_minimum_required(VERSION 3.25)

# Static CRT (/MT, /MTd) so players don't need a VC++ redistributable.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(MultiDiva LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MultiDiva must be built with MSVC for x64 (the game is a 64-bit MSVC binary).")
endif()

# Path to the game folder (the one containing DivaMegaMix.exe). When set, every build
# installs MultiDiva.dll + dist/mods/MultiDiva into <DIVA_DIR>/mods/MultiDiva.
set(DIVA_DIR "" CACHE PATH "Project DIVA Mega Mix+ install folder (optional, enables auto-deploy)")

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/client/deps/ImGui)
set(DETOURS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/client/deps/detours)

add_library(MultiDiva SHARED
    client/src/main.cpp
    client/src/core/base64.cpp
    client/src/core/config.cpp
    client/src/core/crash.cpp
    client/src/core/log.cpp
    client/src/core/mod.cpp
    client/src/game/diffhunt.cpp
    client/src/game/filetrace.cpp
    client/src/game/gamedump.cpp
    client/src/game/hooks.cpp
    client/src/game/launch.cpp
    client/src/game/memory.cpp
    client/src/game/memscan.cpp
    client/src/game/round_finder.cpp
    client/src/game/pause_guard.cpp
    client/src/game/session.cpp
    client/src/game/sigs.cpp
    client/src/game/sigscan.cpp
    client/src/game/songs.cpp
    client/src/game/streams.cpp
    client/src/game/steam.cpp
    client/src/game/sync_start.cpp
    client/src/game/titles.cpp
    client/src/game/watch.cpp
    client/src/mp/multiplayer.cpp
    client/src/net/host.cpp
    client/src/net/report.cpp
    client/src/net/ws_client.cpp
    client/src/ui/avatars.cpp
    client/src/ui/bug_report.cpp
    client/src/ui/debug_panel.cpp
    client/src/ui/game_sprites.cpp
    client/src/ui/glyphs.cpp
    client/src/ui/gpu_capture.cpp
    client/src/ui/input_block.cpp
    client/src/ui/leaderboard.cpp
    client/src/ui/multiplayer_ui.cpp
    client/src/ui/overlay.cpp
    client/src/ui/own_avatar.cpp
    client/src/ui/pause_hint.cpp
    client/src/ui/room_scene.cpp
    client/src/ui/texture.cpp

    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
)

target_compile_features(MultiDiva PRIVATE cxx_std_20)
target_include_directories(MultiDiva PRIVATE
    client/src
    client/deps
    ${IMGUI_DIR}
    ${DETOURS_DIR}/include
)
target_compile_definitions(MultiDiva PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)
target_compile_options(MultiDiva PRIVATE /permissive- /utf-8 /W3 /MP)

target_link_libraries(MultiDiva PRIVATE
    ${DETOURS_DIR}/lib/detours.lib
    d3d11
    dxgi
    dwmapi
    psapi
    winhttp
    dinput8
    dxguid
    wintrust
    crypt32
)

if(DIVA_DIR)
    set(MOD_DIR "${DIVA_DIR}/mods/MultiDiva")
    add_custom_command(TARGET MultiDiva POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DMOD_DIR=${MOD_DIR}"
            "-DDIST_DIR=${CMAKE_CURRENT_SOURCE_DIR}/dist/mods/MultiDiva"
            "-DDLL=$<TARGET_FILE:MultiDiva>"
            "-DSERVER_EXE=${CMAKE_CURRENT_SOURCE_DIR}/server/bin/multidiva-server.exe"
            "-DBOT_EXE=${CMAKE_CURRENT_SOURCE_DIR}/server/bin/multidiva-bot.exe"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deploy.cmake"
        COMMENT "Deploying MultiDiva to ${MOD_DIR}"
    )
endif()

# Offscreen render of the in-game leaderboard over a screenshot, for checking the overlay's look:
#   ui-preview <background.png> <avatars dir> <out dir>
add_executable(ui-preview
    tools/ui-preview/preview.cpp
    client/src/ui/room_scene.cpp
    client/src/ui/game_sprites.cpp
    client/src/game/memory.cpp
    client/src/ui/glyphs.cpp
    client/src/ui/leaderboard.cpp
    client/src/ui/pause_hint.cpp
    client/src/ui/texture.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
)
target_compile_features(ui-preview PRIVATE cxx_std_20)
target_include_directories(ui-preview PRIVATE client/src client/deps ${IMGUI_DIR})
target_compile_definitions(ui-preview PRIVATE MD_SPRITE_PACK WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ui-preview PRIVATE /permissive- /utf-8 /W3 /MP)
target_link_libraries(ui-preview PRIVATE d3d11 d3dcompiler)
