#include "streams.h"
#include "memory.h"
#include "watch.h"
#include "../core/log.h"

#include <cstring>
#include <windows.h>

namespace md::streams {

constexpr uintptr_t Players = 0x14CBFF0A0;
constexpr size_t Stride = 0x58;
constexpr int Count = 3;
constexpr size_t StateOffset = 0x50;
constexpr size_t NameSizeOffset = 0x10;

static bool CodeLooksRight()
{
    // 0x1405AED20: mov [rsp+8], rbx; push rdi; sub rsp, 20h; lea rbx, [rip+...] (the loop over the three players)
    static const uint8_t expected[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x1D };
    uint8_t got[sizeof(expected)] = {};
    return SafeCopy(got, (const void*)0x1405AED20, sizeof(got)) && memcmp(got, expected, sizeof(got)) == 0;
}

void Dump()
{
    if (!CodeLooksRight()) {
        Log("streams: game code differs from v1.03, not touching the players");
        return;
    }
    for (int i = 0; i < Count; i++) {
        uintptr_t at = Players + (uintptr_t)i * Stride;
        uint8_t raw[Stride] = {};
        if (!SafeCopy(raw, (const void*)at, sizeof(raw))) {
            Log("streams: player %d at %llX unreadable", i, (unsigned long long)at);
            continue;
        }
        Log("streams: player %d at %llX, state %d", i, (unsigned long long)at, *(int32_t*)(raw + StateOffset));
        for (size_t row = 0; row < sizeof(raw); row += 0x20) {
            size_t n = sizeof(raw) - row < 0x20 ? sizeof(raw) - row : 0x20;
            int32_t w[8] = {};
            memcpy(w, raw + row, n);
            Log("streams:   +%02zX: %08X %08X %08X %08X %08X %08X %08X %08X", row, w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                w[7]);
        }
        // A std::string inside (MSVC: 16 bytes of text or a pointer, then size and capacity).
        for (size_t off = 0; off + 0x20 <= sizeof(raw); off += 8) {
            const uint8_t* s = raw + off;
            uint64_t size = *(uint64_t*)(s + 16), cap = *(uint64_t*)(s + 24);
            if (size == 0 || size > 260 || cap < size || cap > 4096)
                continue;
            char text[261] = {};
            if (cap < 16)
                memcpy(text, s, (size_t)size);
            else if (!SafeCopy(text, (const void*)*(uint64_t*)s, (size_t)size))
                continue;
            bool printable = true;
            for (size_t k = 0; k < size; k++)
                printable &= (unsigned char)text[k] >= 0x20 && (unsigned char)text[k] < 0x7F;
            if (printable)
                Log("streams:   string at +%02zX: \"%s\"", off, text);
        }
        // The stream the player holds (+0x20): where the decoding state lives.
        uint64_t stream = *(uint64_t*)(raw + 0x20);
        uint8_t body[0x80] = {};
        if (stream > 0x10000 && SafeCopy(body, (const void*)stream, sizeof(body))) {
            for (size_t row = 0; row < sizeof(body); row += 0x20) {
                int32_t w[8];
                memcpy(w, body + row, sizeof(w));
                Log("streams:   stream %llX +%02zX: %08X %08X %08X %08X %08X %08X %08X %08X", (unsigned long long)stream, row,
                    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
    }
}

bool WatchName(int index)
{
    if (index < 0 || index >= Count || !CodeLooksRight()) {
        Log("streams: no player %d (or the game code differs)", index);
        return false;
    }
    // The file name is a std::string at +0x00 (text or pointer, size at +0x10): assigning a new name writes the size,
    // even when it is unchanged. The state at +0x50 is rewritten by the update loop every frame, so it is useless.
    watch::SetHitCallback(nullptr);
    return watch::Start(Players + (uintptr_t)index * Stride + NameSizeOffset, 80);
}

} // namespace md::streams
