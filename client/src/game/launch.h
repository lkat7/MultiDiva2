#pragma once
#include <cstddef>
#include <cstdint>

namespace md::launch {

// The launch parameters of one stage entry (ReDIVA: struc_775), byte range [BlockBegin, BlockEnd) of the
// entry: pv id, difficulty, edition, clear/life settings, performer modules, items and item masks.
// The host's block is what clients replay, so everyone gets the same song, chart and outfits.
// Ends right before the result data (grade at entry + 0x140).
constexpr size_t BlockBegin = 0x2C;
constexpr size_t BlockEnd = 0x140;

struct Block {
    uint8_t bytes[BlockEnd - BlockBegin];

    int32_t Int(size_t entryOffset) const;
    void SetInt(size_t entryOffset, int32_t value);
    int32_t PvId() const;
    int32_t Difficulty() const;
    int32_t Edition() const;
    int32_t Module(int slot) const;
};

struct Request {
    Block block;
    bool fullBlock; // false: only pv id / difficulty / edition are applied (outfits stay as selected)
};

void Arm(const Request& request);
void Disarm();
bool IsArmed(Request* out = nullptr);

// The block of the most recent game launch (after any override). This is what a host sends.
bool LastLaunch(Block& out);

// True while the player is on the song select screen (mode 2 "rhythm game", sub mode 5).
bool InSongSelect();

// Starts `block` right now, as if the player had confirmed a song: arms it, then requests the
// gameplay sub mode (7) the same way song select does. Only works from song select.
// Call from OnFrame (main thread, between game frames).
// ownOutfits: the performers wear this player's own outfits for that song (OwnModules) instead of the
// host's.
bool StartNow(const Block& block, bool ownOutfits = false);

// This player's outfits for `pv` as song select would launch it: their saved choice per performer slot,
// resolved (recommended, random...) into module ids. False if the game code isn't the expected version.
// Song select only (it borrows song select's selection record). OnFrame only.
bool OwnModules(int32_t pv, int32_t (&modules)[6]);

// The song select "enter" sound, which StartNow plays in place of the skipped Start press. OnFrame only.
void PlayStartSound();
// Any of the game's sound effects by name (the names are in its data: se_ft_music_selector_select_01 is the song
// cursor, se_ft_music_selector_difficulty_change_01 the difficulty change). Game thread only; unknown names are silent.
void PlayGameSound(const char* name);

// Game thread only, called by the PvGameInitPv hook right before the game reads the stage entry.
// Applies an armed request (then disarms) and records the final block as the last launch.
// Returns true if a request was applied.
bool OnPvGameInit(int32_t stageIndex);

} // namespace md::launch
