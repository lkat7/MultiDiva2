#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Research tool: searches the game's readable memory for a byte pattern (e.g. to find loaded sprite sets).
namespace md::memscan {

// Every address where `pattern` occurs, up to `max` hits. Slow (scans all committed memory): research only.
std::vector<uintptr_t> Find(const void* pattern, size_t size, size_t max = 32);

// Ctrl+F8 research button: looks for the main menu sprite set (spr_nswgam_menu.bin) and logs what it finds.
void FindMenuSpriteSet();

// Ctrl+F8 research button: lists the game's loaded sprite sets from its sprite set map (instant, no scan).
void ListSpriteSets();

// Ctrl+F8 research button: looks for the game's parsed font glyph tables (fontmap.bin) and who holds them.
void FindFontMap();

// Ctrl+F8 research button: logs where `text` is in the game's memory, as UTF-8 and as UTF-16, with the bytes around
// each hit and its offset from the game exe (to find the player profile's name and the struct around it).
void FindText(const std::string& text);

} // namespace md::memscan
