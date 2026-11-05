#pragma once
#include <cstdint>

// Debug tool: a hardware write watchpoint (debug register DR0) on one game address, on every thread.
// Each write is logged with the writing instruction and the game code return addresses on the stack,
// to find which game function writes a field. Off unless started from the Ctrl+F8 panel.
namespace md::watch {

// Called with the CONTEXT and the value just written, for each logged hit, inside the exception handler (be quick, no
// allocation, only safe memory reads). Null = none.
void SetHitCallback(void (*callback)(void* contextRecord, int32_t value));

// Logs the exact call chain of a CONTEXT (function start <- return address per frame) and dumps the first 128 bytes of
// every function met for the first time.
void LogCallChain(void* contextRecord, const char* tag);

bool Start(uintptr_t address, int maxHits = 24); // 4-byte write watch; false if it could not be set
void Stop();
bool Active();
int Hits();

} // namespace md::watch
