#include "launch.h"
#include "memory.h"
#include "sigs.h"
#include "../core/log.h"

#include <cstring>
#include <mutex>

namespace md::launch {

int32_t Block::Int(size_t entryOffset) const
{
    int32_t v;
    memcpy(&v, bytes + (entryOffset - BlockBegin), sizeof(v));
    return v;
}

void Block::SetInt(size_t entryOffset, int32_t value)
{
    memcpy(bytes + (entryOffset - BlockBegin), &value, sizeof(value));
}

int32_t Block::PvId() const { return Int(stage::PvId); }
int32_t Block::Difficulty() const { return Int(stage::Difficulty); }
int32_t Block::Edition() const { return Int(stage::Edition); }
int32_t Block::Module(int slot) const { return Int(stage::Modules + slot * sizeof(int32_t)); }

// Arming happens a few times per round, so a mutex is fine even though the hook takes it on the game thread.
static std::mutex s_mutex;
static Request s_armed;
static bool s_isArmed = false;
static Block s_last;
static bool s_hasLast = false;

void Arm(const Request& request)
{
    std::lock_guard lock(s_mutex);
    s_armed = request;
    s_isArmed = true;
}

void Disarm()
{
    std::lock_guard lock(s_mutex);
    s_isArmed = false;
}

bool IsArmed(Request* out)
{
    std::lock_guard lock(s_mutex);
    if (s_isArmed && out)
        *out = s_armed;
    return s_isArmed;
}

bool LastLaunch(Block& out)
{
    std::lock_guard lock(s_mutex);
    if (s_hasLast)
        out = s_last;
    return s_hasLast;
}

// Called by song select right after requesting SubGameMain: void (unused, int = 0, bool).
// Not signature-scanned (tiny thunk), so the bytes are checked before every call.
static constexpr uintptr_t SelectStartFlag = 0x140653C30;
static const uint8_t SelectStartFlagBytes[] = { 0x44, 0x88, 0x05 }; // mov byte ptr [rip+...], r8b
using SelectStartFlagFn = void(__fastcall*)(void*, int32_t, bool);
using ShiftNextModeSubFn = void(__fastcall*)(int32_t);

// play_se(name, volume) on the system channel; song select plays this sound when a song is entered.
static constexpr uintptr_t PlaySe = 0x1405AA540;
static const uint8_t PlaySeBytes[] = { 0x48, 0x8B, 0xD1, 0x0F, 0x28, 0xD1, 0xB9, 0x01, 0x00, 0x00, 0x00 };
using PlaySeFn = void(__fastcall*)(const char* name, float volume);
static constexpr const char* StartSound = "se_ft_music_selector_enter_01";

void PlayGameSound(const char* name)
{
    uint8_t check[sizeof(PlaySeBytes)];
    if (SafeRead(PlaySe, check) && memcmp(check, PlaySeBytes, sizeof(check)) == 0)
        ((PlaySeFn)PlaySe)(name, 1.0f);
}

void PlayStartSound() { PlayGameSound(StartSound); }

bool InSongSelect()
{
    int32_t m[12];
    if (!SafeRead(addr::ModeState, m))
        return false;
    // NextSub == SubGameMain means a start is already in flight.
    return m[mode::CurrentMode] == mode::RhythmGame && m[mode::CurrentSub] == mode::SubSongSelect
        && m[mode::NextSub] != mode::SubGameMain;
}

// ---- the player's own outfits for any song (see docs/re-notes.md "Outfits") ----
// Song select keeps the highlighted song's outfit choices in a selection record, reloaded every frame by
// LoadChoices(record, pv, nullptr) from the save data; its start builder (0x1406F2AE0) then resolves each
// performer's choice (recommended, random...) into a module id with Resolve(ResolveContext(), choice, extra).
namespace outfit {
constexpr uintptr_t SelectionRecord = 0x141750890;
constexpr size_t Choices = 0x1C0;     // int32[6]
constexpr size_t Extras = 0x338 + 0x10; // int32 every 0x14 bytes, the resolver's third argument
constexpr size_t ExtraStride = 0x14;
constexpr uintptr_t LoadChoices = 0x1401EAD50;
constexpr uintptr_t Resolve = 0x1404427B0;
constexpr uintptr_t ResolveContext = 0x140441B60;
const uint8_t LoadChoicesBytes[] = { 0x83, 0xFA, 0xFE, 0x0F, 0x84 };  // cmp edx, -2; je
const uint8_t ResolveBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
const uint8_t ResolveContextBytes[] = { 0x48, 0x8D, 0x05 };            // lea rax, [rip+...]
using LoadChoicesFn = void(__fastcall*)(void* record, int32_t pv, const void* source);
using ResolveFn = int32_t(__fastcall*)(void* context, int32_t choice, int32_t extra);
using ResolveContextFn = void*(__fastcall*)();
} // namespace outfit

template <size_t N> static bool CodeIs(uintptr_t address, const uint8_t (&bytes)[N])
{
    uint8_t check[N];
    return SafeRead(address, check) && memcmp(check, bytes, N) == 0;
}

bool OwnModules(int32_t pv, int32_t (&modules)[6])
{
    using namespace outfit;
    if (!CodeIs(LoadChoices, LoadChoicesBytes) || !CodeIs(Resolve, ResolveBytes)
        || !CodeIs(ResolveContext, ResolveContextBytes))
        return false;
    // Song select reloads the record for its highlighted song next frame, so borrowing it is harmless.
    ((LoadChoicesFn)LoadChoices)((void*)SelectionRecord, pv, nullptr);
    void* context = ((ResolveContextFn)ResolveContext)();
    int32_t choices[6] = {};
    SafeRead(SelectionRecord + Choices, choices);
    for (int i = 0; i < 6; i++) {
        int32_t extra = 0;
        SafeRead(SelectionRecord + Extras + i * ExtraStride, extra);
        modules[i] = ((ResolveFn)Resolve)(context, choices[i], extra);
    }
    Log("own outfits: pv %d choices %d %d %d %d %d %d -> modules %d %d %d %d %d %d", pv, choices[0], choices[1],
        choices[2], choices[3], choices[4], choices[5], modules[0], modules[1], modules[2], modules[3], modules[4],
        modules[5]);
    return true;
}

bool StartNow(const Block& block, bool ownOutfits)
{
    const sigs::Result& shift = sigs::Get(sigs::ShiftNextModeSub);
    uint8_t check[sizeof(SelectStartFlagBytes)];
    if (!shift.address || !InSongSelect() || !SafeRead(SelectStartFlag, check)
        || memcmp(check, SelectStartFlagBytes, sizeof(check)) != 0)
        return false;

    // The host's whole block (song, chart, settings), optionally with this player's own outfits.
    Block b = block;
    int32_t modules[6];
    if (ownOutfits && OwnModules(block.PvId(), modules))
        for (int i = 0; i < 6; i++)
            b.SetInt(stage::Modules + i * sizeof(int32_t), modules[i]);
    Arm({ b, true });

    // Same sequence as ModeGamePS4_PvselImpl's ctrl (0x1401ECD75..0x1401ECD8E).
    // Calls go through the hooked entry points, so the ModeShift log still sees them.
    ((ShiftNextModeSubFn)shift.address)(mode::SubGameMain);
    uint8_t flag19 = 0;
    SafeRead(addr::StageData + 0x19, flag19);
    ((SelectStartFlagFn)SelectStartFlag)(nullptr, 0, flag19 == 0);
    PlayStartSound(); // the Start press we skip would have played it
    return true;
}

bool OnPvGameInit(int32_t stageIndex)
{
    // The stage data is plain writable game memory, no VirtualProtect needed.
    uint8_t* entry = (uint8_t*)StageEntryAddress(stageIndex);
    uint8_t* block = entry + BlockBegin;

    std::lock_guard lock(s_mutex);
    bool applied = false;
    if (s_isArmed) {
        if (s_armed.fullBlock) {
            memcpy(block, s_armed.block.bytes, sizeof(Block::bytes));
        } else {
            for (size_t offset : { stage::PvId, stage::Difficulty, stage::Edition })
                memcpy(entry + offset, s_armed.block.bytes + (offset - BlockBegin), sizeof(int32_t));
        }
        s_isArmed = false;
        applied = true;
    }

    memcpy(s_last.bytes, block, sizeof(Block::bytes));
    s_hasLast = true;
    return applied;
}

} // namespace md::launch
