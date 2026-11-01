#pragma once

// Research: writes the game's code and data (the exe module's memory, after the protector has decrypted what it
// decrypts) to a file, for reading in Ghidra. The file goes to %LOCALAPPDATA%\MultiDiva2\dump\ (never into the repo or
// the mod folder) and is for your own use: don't share it. See docs/ghidra.md.
namespace md::gamedump {

void Write(); // logs where the file went; unreadable pages are zero

} // namespace md::gamedump
