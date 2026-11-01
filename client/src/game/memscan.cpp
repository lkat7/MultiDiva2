#include "memscan.h"
#include "memory.h"
#include "../core/log.h"

#include <cstdio>
#include <cstring>
#include <windows.h>
#include <d3d11.h>

namespace md::memscan {

// Searches one region; SEH-guarded (a region can be freed while we read it). No C++ objects here (/EHsc + __try).
static size_t ScanRegion(const uint8_t* p, size_t len, const uint8_t* pat, size_t size, uintptr_t* out, size_t room)
{
    size_t found = 0;
    __try {
        for (size_t i = 0; i + size <= len && found < room; i++)
            if (p[i] == pat[0] && memcmp(p + i, pat, size) == 0)
                out[found++] = (uintptr_t)(p + i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

// Our own module (search patterns, the result buffers below) and our stack (copies of the searched value)
// would be found too: skip both.
static bool IsOurs(uintptr_t base, uintptr_t len)
{
    static uintptr_t modBase = 0, modEnd = 0;
    if (!modBase) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&IsOurs, &self);
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(self, &mbi, sizeof(mbi));
        modBase = (uintptr_t)mbi.AllocationBase;
        modEnd = modBase;
        while (VirtualQuery((void*)modEnd, &mbi, sizeof(mbi)) && (uintptr_t)mbi.AllocationBase == modBase)
            modEnd += mbi.RegionSize;
    }
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    auto overlaps = [&](uintptr_t a, uintptr_t b) { return base < b && base + len > a; };
    return overlaps(modBase, modEnd) || overlaps(lo, hi);
}

// Result buffer inside our module, so the scan never finds its own results.
static uintptr_t s_hits[64];

static size_t FindInto(const void* pattern, size_t size, size_t max)
{
    max = max > 64 ? 64 : max;
    size_t count = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress, end = (uintptr_t)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && count < max && VirtualQuery((void*)addr, &mbi, sizeof(mbi))) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, len = mbi.RegionSize;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
        if (readable && !IsOurs(base, len))
            count += ScanRegion((const uint8_t*)base, len, (const uint8_t*)pattern, size, s_hits + count, max - count);
        addr = base + len;
    }
    return count;
}

std::vector<uintptr_t> Find(const void* pattern, size_t size, size_t max)
{
    size_t n = FindInto(pattern, size, max);
    return std::vector<uintptr_t>(s_hits, s_hits + n);
}

static void Dump(const char* what, uintptr_t at, size_t bytes)
{
    uint8_t buf[256] = {};
    bytes = bytes > sizeof(buf) ? sizeof(buf) : bytes;
    if (!SafeCopy(buf, (const void*)at, bytes)) {
        Log("memscan:   %s %llX: unreadable", what, (unsigned long long)at);
        return;
    }
    for (size_t row = 0; row < bytes; row += 32) {
        char line[200];
        int len = 0;
        for (size_t i = row; i < row + 32 && i < bytes; i += 8)
            len += snprintf(line + len, sizeof(line) - len, "%016llX ", *(unsigned long long*)(buf + i));
        Log("memscan:   %s +%02zX: %s", what, row, line);
    }
}

// True if `p` points at a Direct3D 11 object (its vtable lies inside d3d11.dll).
static bool IsD3DObject(uintptr_t p)
{
    static uintptr_t lo = 0, hi = 0;
    if (!lo) {
        HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
        MEMORY_BASIC_INFORMATION mbi;
        if (!d3d || !VirtualQuery(d3d, &mbi, sizeof(mbi)))
            return false;
        lo = hi = (uintptr_t)mbi.AllocationBase;
        while (VirtualQuery((void*)hi, &mbi, sizeof(mbi)) && (uintptr_t)mbi.AllocationBase == lo)
            hi += mbi.RegionSize;
    }
    uintptr_t vt = 0;
    return p > 0x10000 && !(p >> 47) && SafeRead(p, vt) && vt >= lo && vt < hi;
}

// Looks for Direct3D objects up to two pointer hops from `obj` (one hop = a qword inside an object).
static void FindD3DFrom(uintptr_t obj, int span)
{
    int found = 0;
    for (int i = 0; i < span && found < 24; i++) {
        uintptr_t p = 0;
        if (!SafeRead(obj + i * 8, p))
            continue;
        if (IsD3DObject(p)) {
            ID3D11DeviceChild* child = (ID3D11DeviceChild*)p;
            (void)child;
            Log("memscan:     D3D object at +%03X -> %llX", i * 8, (unsigned long long)p);
            found++;
            continue;
        }
        if (p < 0x10000 || (p >> 47))
            continue;
        for (int j = 0; j < 32 && found < 24; j++) {
            uintptr_t q = 0;
            if (SafeRead(p + j * 8, q) && IsD3DObject(q)) {
                Log("memscan:     D3D object at +%03X -> %llX +%03X -> %llX", i * 8, (unsigned long long)p, j * 8,
                    (unsigned long long)q);
                found++;
            }
        }
    }
    if (!found)
        Log("memscan:     no D3D objects within 2 hops");
}

// The game's loaded sprite sets: std::map<uint32 set id, SPRSET> at 0x14CBFF3A0 (filled by 0x1405B8C20).
// MSVC map: { node* head, size }; node: left, parent, right, color, isnil (+0x19), key (+0x20), SPRSET (+0x28).
// SPRSET: +0x10 loaded file, +0x18 relocated header (counts, sprite infos, names), +0x20 textures?
static void Walk(uintptr_t node, uintptr_t head, int depth, int& shown)
{
    if (!node || node == head || depth > 64)
        return;
    uintptr_t left = 0, right = 0;
    SafeRead(node, left);
    SafeRead(node + 0x10, right);
    Walk(left, head, depth + 1, shown);
    uint32_t id = 0;
    uintptr_t file = 0, header = 0, tex = 0;
    SafeRead(node + 0x20, id);
    SafeRead(node + 0x28 + 0x10, file);
    SafeRead(node + 0x28 + 0x18, header);
    SafeRead(node + 0x28 + 0x20, tex);
    if (!header) { // the map lists every set in the game; only loaded ones have data
        Walk(right, head, depth + 1, shown);
        return;
    }
    uint32_t texOffset = 0, texCount = 0, sprCount = 0;
    uintptr_t names = 0, name0 = 0;
    SafeRead(header + 4, texOffset);
    SafeRead(header + 8, texCount);
    SafeRead(header + 12, sprCount);
    SafeRead(header + 0x20, names);
    SafeRead(names, name0);
    char first[48] = {};
    SafeCopy(first, (const void*)name0, sizeof(first) - 1);
    uint32_t magic = 0;
    SafeRead(file + texOffset, magic);
    Log("memscan: set %u: %u sprites, %u textures, first \"%s\", file %llX, tex data %s, tex %llX", id, sprCount,
        texCount, first, (unsigned long long)file, magic == 0x03505854 ? "TXP present" : "gone", (unsigned long long)tex);
    if (shown++ < 3 && tex) {
        Dump("tex", tex, 64);
        // Three hops: texture array -> texture object -> D3D object.
        for (int i = 0; i < 4; i++) {
            uintptr_t t = 0;
            if (SafeRead(tex + i * 8, t) && t > 0x10000 && !(t >> 47))
                FindD3DFrom(t, 0x20);
        }
    }
    Walk(right, head, depth + 1, shown);
}

void ListSpriteSets()
{
    uintptr_t head = 0, size = 0, root = 0;
    SafeRead(0x14CBFF3A0, head);
    SafeRead(0x14CBFF3A8, size);
    SafeRead(head + 8, root);
    Log("memscan: sprite set map head %llX, %llu set(s)", (unsigned long long)head, (unsigned long long)size);
    int shown = 0;
    Walk(root, head, 0, shown);
}

void FindMenuSpriteSet()
{
    // A short sprite name of spr_nswgam_menu.bin, as it sits in the loaded set's string pool ("\0NAME\0").
    static const char name[] = "\0SELECTOR_RHYTHMGAME_EFF";
    DWORD t0 = GetTickCount();
    static uintptr_t names[8];
    size_t nn = FindInto(name, sizeof(name), 8);
    memcpy(names, s_hits, nn * sizeof(uintptr_t));
    Log("memscan: sprite set string at %zu place(s) (%lu ms)", nn, GetTickCount() - t0);

    for (size_t k = 0; k < nn; k++) {
        static uintptr_t str;
        str = names[k] + 1;
        Log("memscan: string %llX", (unsigned long long)str);
        // The name pointer array entry for it, then the start of that array.
        size_t nr = FindInto(&str, sizeof(str), 4);
        for (size_t r = 0; r < nr; r++) {
            static uintptr_t entry, arr;
            entry = s_hits[r];
            arr = entry;
            for (int i = 0; i < 200; i++) {
                uintptr_t prev = 0;
                if (!SafeRead(arr - 8, prev) || prev >= str || str - prev > 0x10000)
                    break;
                arr -= 8;
            }
            Log("memscan:  pointer at %llX, array starts %llX (index %llu)", (unsigned long long)entry,
                (unsigned long long)arr, (unsigned long long)((entry - arr) / 8));
            Dump("names[]", arr, 64);
            // Who points at the array: the loaded set's header.
            size_t nh = FindInto(&arr, sizeof(arr), 4);
            static uintptr_t heads[4];
            memcpy(heads, s_hits, nh * sizeof(uintptr_t));
            for (size_t h = 0; h < nh; h++) {
                // The set header (signature at -0x20 from the names pointer, as in the file) and who holds it.
                static uintptr_t set;
                set = heads[h] - 0x20;
                Log("memscan:  sprite set header at %llX", (unsigned long long)set);
                Dump("set", set, 0x40);
                Log("memscan:   near the set:");
                FindD3DFrom(set - 0x100, 0x60);
                size_t no = FindInto(&set, sizeof(set), 6);
                static uintptr_t owners[6];
                memcpy(owners, s_hits, no * sizeof(uintptr_t));
                for (size_t o = 0; o < no; o++) {
                    Log("memscan:   held by %llX", (unsigned long long)owners[o]);
                    Dump("holder", owners[o] - 0x40, 0xC0);
                    FindD3DFrom(owners[o] - 0x80, 0x40);
                }
            }
        }
    }
}

void FindText(const std::string& text)
{
    if (text.empty())
        return;
    std::wstring wide;
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    wide.resize(n > 0 ? (size_t)n : 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wide.data(), n);
    const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    DWORD t0 = GetTickCount();
    for (int pass = 0; pass < 2; pass++) {
        const void* pattern = pass == 0 ? (const void*)text.data() : (const void*)wide.data();
        size_t size = pass == 0 ? text.size() : wide.size() * sizeof(wchar_t);
        static uintptr_t hits[16];
        size_t nh = FindInto(pattern, size, 16);
        memcpy(hits, s_hits, nh * sizeof(uintptr_t));
        Log("memscan: \"%s\" as %s: %zu hit(s) (%lu ms)", text.c_str(), pass == 0 ? "UTF-8" : "UTF-16", nh, GetTickCount() - t0);
        for (size_t k = 0; k < nh; k++) {
            Log("memscan:   hit %llX (exe %+lld)", (unsigned long long)hits[k], (long long)(hits[k] - exe));
            Dump("around", hits[k] - 0x20, 0x80);
        }
    }
}

void FindFontMap()
{
    // Glyphs 'A'..'D' of the game's 36 px Latin fonts (fontmap.bin maps 20 and 21 agree on these), as parsed into
    // ReDIVA's font_character { init, halfwidth, column, row, offset, width }: 6 bytes per character code.
    static const uint8_t glyphs[] = { 1, 1, 0x21, 0, 0, 0x11, 1, 1, 0x22, 0, 0, 0x10, 1, 1, 0x23, 0, 0, 0x11,
        1, 1, 0x24, 0, 0, 0x11 };
    constexpr size_t Stride = 6;
    DWORD t0 = GetTickCount();
    static uintptr_t tables[16];
    size_t nt = FindInto(glyphs, sizeof(glyphs), 16);
    memcpy(tables, s_hits, nt * sizeof(uintptr_t));
    Log("memscan: font glyph tables: %zu (%lu ms)", nt, GetTickCount() - t0);
    for (size_t k = 0; k < nt; k++) {
        static uintptr_t base;
        base = tables[k] - 0x41 * Stride; // the table is indexed by character code
        uint8_t i[6] = {}, space[6] = {}, kanji[6] = {};
        SafeCopy(i, (const void*)(base + 'i' * Stride), 6);
        SafeCopy(space, (const void*)(base + ' ' * Stride), 6);
        SafeCopy(kanji, (const void*)(base + 0x521D * Stride), 6); // a kanji (U+521D), to see the table size
        Log("memscan:  table %llX: ' ' %02X %02X %02X %02X %02X %02X | 'i' %02X %02X %02X %02X %02X %02X | U+521D %02X %02X %02X %02X %02X %02X",
            (unsigned long long)base, space[0], space[1], space[2], space[3], space[4], space[5],
            i[0], i[1], i[2], i[3], i[4], i[5], kanji[0], kanji[1], kanji[2], kanji[3], kanji[4], kanji[5]);
        if (k >= 4)
            continue;
        // Who holds the table (a std::vector's begin pointer inside the game's font object).
        size_t no = FindInto(&base, sizeof(base), 6);
        static uintptr_t owners[6];
        memcpy(owners, s_hits, no * sizeof(uintptr_t));
        for (size_t o = 0; o < no; o++) {
            Log("memscan:   held by %llX", (unsigned long long)owners[o]);
            Dump("font", owners[o] - 0x30, 0x60);
        }
    }
    // Is the file itself kept loaded?
    static const char sig[] = "FMH3";
    size_t nf = FindInto(sig, 4, 8);
    for (size_t k = 0; k < nf; k++) {
        Log("memscan: FMH3 at %llX", (unsigned long long)s_hits[k]);
        Dump("fmh3", s_hits[k], 0x40);
    }
    Log("memscan: font map search done (%lu ms)", GetTickCount() - t0);
}

} // namespace md::memscan
