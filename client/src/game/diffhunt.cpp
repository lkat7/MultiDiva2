#include "diffhunt.h"
#include "memory.h"
#include "songs.h"
#include "watch.h"
#include "../core/log.h"

#include <cstring>
#include <windows.h>

namespace md::diffhunt {

namespace {

// `mov [r14+8], eax` ends at this address: the difficulty store of the stage entry copy.
constexpr uintptr_t WriteEnd = 0x14040B6C5;
const uint8_t WriteBytes[] = { 0x41, 0x89, 0x46, 0x08 };

constexpr int Regs = 15;
constexpr int StackSlots = 128;
constexpr int Slots = Regs + StackSlots;
constexpr size_t ObjectBytes = 0x400;
constexpr int Words = ObjectBytes / 4;

uint8_t s_alive[Slots][Words]; // 1 = this field matched on every hit so far
int s_hits = 0;

struct Slot {
    char name[16];
    uint64_t value;
};

void OnHit(void* contextRecord, int32_t value)
{
    const CONTEXT* c = (const CONTEXT*)contextRecord;
    if (c->Rip != WriteEnd || value < 0 || value > 4)
        return;
    uint8_t code[sizeof(WriteBytes)] = {};
    if (!SafeCopy(code, (const void*)(WriteEnd - sizeof(code)), sizeof(code)) || memcmp(code, WriteBytes, sizeof(code)) != 0)
        return;

    Slot slots[Slots] = {};
    const struct { const char* name; DWORD64 v; } regs[Regs] = { { "rax", c->Rax }, { "rcx", c->Rcx }, { "rdx", c->Rdx },
        { "rbx", c->Rbx }, { "rbp", c->Rbp }, { "rsi", c->Rsi }, { "rdi", c->Rdi }, { "r8", c->R8 }, { "r9", c->R9 },
        { "r10", c->R10 }, { "r11", c->R11 }, { "r12", c->R12 }, { "r13", c->R13 }, { "r14", c->R14 }, { "r15", c->R15 } };
    for (int i = 0; i < Regs; i++) {
        strcpy_s(slots[i].name, regs[i].name);
        slots[i].value = regs[i].v;
    }
    for (int i = 0; i < StackSlots; i++) {
        uint64_t q = 0;
        SafeCopy(&q, (const void*)(c->Rsp + (uintptr_t)i * 8), sizeof(q));
        wsprintfA(slots[Regs + i].name, "rsp+%X", i * 8);
        slots[Regs + i].value = q;
    }

    for (int s = 0; s < Slots; s++) {
        uint8_t match[Words] = {};
        uint64_t p = slots[s].value;
        int32_t words[Words];
        if (p > 0x10000 && !(p >> 47) && SafeCopy(words, (const void*)p, ObjectBytes))
            for (int w = 0; w < Words; w++)
                match[w] = words[w] == value;
        for (int w = 0; w < Words; w++)
            s_alive[s][w] = s_hits == 0 ? match[w] : (uint8_t)(s_alive[s][w] & match[w]);
    }
    s_hits++;

    Log("diffhunt: hit %d wrote difficulty %d; fields that matched every time:", s_hits, value);
    int shown = 0, total = 0;
    for (int s = 0; s < Slots; s++)
        for (int w = 0; w < Words; w++)
            if (s_alive[s][w]) {
                total++;
                if (shown < 40) {
                    shown++;
                    // Is this object referenced from the song list object we already track? Then it is reachable.
                    char link[48] = "";
                    if (uintptr_t list = songs::ListObject(); list && slots[s].value > 0x10000) {
                        for (size_t off = 0; off < 0x1800; off += 8) {
                            uint64_t q = 0;
                            if (SafeCopy(&q, (const void*)(list + off), sizeof(q)) && q == slots[s].value) {
                                wsprintfA(link, "  (also at list+0x%X)", (unsigned)off);
                                break;
                            }
                        }
                    }
                    Log("diffhunt:   %s = %llX  +0x%03X%s", slots[s].name, (unsigned long long)slots[s].value, w * 4, link);
                }
            }
    Log("diffhunt: %d candidate field(s)%s", total, total > shown ? " (first 40 shown)" : "");
}

} // namespace

void Arm()
{
    memset(s_alive, 0, sizeof(s_alive));
    s_hits = 0;
    watch::SetHitCallback(OnHit);
    Log("diffhunt: armed (rounds on different difficulties narrow the candidates)");
}

} // namespace md::diffhunt
