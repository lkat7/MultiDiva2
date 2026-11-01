#pragma once
#include <cstdint>

// Research: finds where song select keeps the selected difficulty. At confirm the game copies it into the stage
// entry (the instruction at 0x14040B6C1, see docs/re-notes.md) from a struct built by its caller. For every such write
// seen by the write watch (game/watch.h) this looks, in every register and every pointer on the stack, for int32 fields
// equal to the value written, and keeps only the fields that matched on every hit so far. Start rounds on different
// difficulties, in no particular order, and the survivors are the fields that really are the selected difficulty.
namespace md::diffhunt {

void Arm(); // installs the hit callback on the write watch (call before watch::Start)

} // namespace md::diffhunt
