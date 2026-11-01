#pragma once

// Research: the game's three sound stream players. A global array (0x14CBFF0A0, 0x58 bytes each) that 0x1405AED20 updates
// every frame; the update (0x1405AF2A0 -> state machine 0x1405AF420, state at +0x50) opens a stream's file through
// 0x1405AF560 (the path of a song's .ogg reaches CreateFileW from there). See docs/re-notes.md.
namespace md::streams {

// Logs the three players: their ints and every std::string inside (a stream's file name). Do it while a preview plays.
void Dump();

// Write watch (with the exact call chain) on one player's file name (the size of its std::string): whatever requests a new
// stream is logged. (The state field is useless: the update loop rewrites it every frame.)
bool WatchName(int index);

} // namespace md::streams
