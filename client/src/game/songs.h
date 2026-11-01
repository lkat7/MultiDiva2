#pragma once
#include <cstdint>
#include <string>

// Which songs this player has, and which one song select has highlighted (see docs/re-notes.md "Songs").
namespace md::songs {

void Install(); // the song select highlight watch (hardware breakpoint, no code patch)
void Tick();    // once per frame

// True if the song is installed (base game, DLC or mod song). difficulty < 0: any chart; edition 2: any.
bool Has(int32_t pv, int32_t difficulty = -1, int32_t edition = 2);

// The song highlighted in song select, -1 when not on song select (or not known yet).
int32_t Hovered();

// The song's name from the game's song list (in the game's language); empty if it isn't installed.
// Game thread only.
std::string Title(int32_t pv);

// The chart's level in stars (7.5, 10.0...), -1 if unknown. Read from the pv_db chart struct at a layout taken from
// ReDIVA (the Arcade game's decompilation), so it is checked for a sane value and shown only if it is one.
// Game thread only; cached.
float Level(int32_t pv, int32_t difficulty, int32_t edition);

// The song's tempo in BPM, -1 if unknown (same caveat as Level). Game thread only; cached.
int32_t Bpm(int32_t pv);

// Research: while on, every frame in song select, logs each int32 in the song list object (the highlight hook's rcx)
// and in song select's selection record that changes between two values 0..4 (difficulty / edition look like that),
// plus every highlight change. Change the difficulty in song select a few times, then read the log: the offset that
// moves with it is where the game keeps the selected difficulty.
void SetListWatch(bool on);

// Research: logs every std::string inside the song's pv_db entry (to find the title fields).
void DumpEntryStrings(int32_t pv);

// Research: logs the start of the song's pv_db entry and of its chart as int32s, to confirm the Level and Bpm offsets
// against the star rating and tempo the game shows.
void DumpChart(int32_t pv, int32_t difficulty, int32_t edition);

} // namespace md::songs
