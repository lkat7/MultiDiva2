#pragma once

// Research: finds the game code that opens a song's audio file (the preview player's path). A hardware execute breakpoint
// (DR2) on KernelBase!CreateFileW logs every open of a path containing "sound/song" or ".ogg" with the exact call chain
// of the game functions that asked for it. Nothing is patched.
namespace md::filetrace {

void Start(); // arm on every thread
void Stop();
bool Active();

} // namespace md::filetrace
