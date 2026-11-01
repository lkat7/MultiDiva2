#include "gamedump.h"
#include "memory.h"
#include "../core/log.h"
#include "../core/mod.h"

#include <cstdint>
#include <cstdio>
#include <vector>
#include <windows.h>

namespace md::gamedump {

void Write()
{
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS64 nt = {};
    if (!base || !SafeCopy(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || !SafeCopy(&nt, base + dos.e_lfanew, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) {
        Log("dump: could not read the game's PE header");
        return;
    }
    // The game's code is 0x140001000..0x140B24A00 (11.6 MB); the next few MB hold the vtables and jump tables the
    // decompiler needs. The rest of the image (the exe is 300 MB on disk) is not needed.
    constexpr size_t MaxBytes = 16u << 20;
    size_t size = nt.OptionalHeader.SizeOfImage;
    if (size > MaxBytes)
        size = MaxBytes;
    Log("dump: game image at %p, size 0x%X (writing the first 0x%zX bytes)", (const void*)base, nt.OptionalHeader.SizeOfImage, size);

    // Page by page: a page that can't be read stays zero instead of stopping the dump.
    std::vector<uint8_t> image(size, 0);
    size_t unreadable = 0;
    for (size_t off = 0; off < size; off += 0x1000)
        if (!SafeCopy(image.data() + off, base + off, 0x1000))
            unreadable++;

    std::wstring dir = DataDir() + L"\\dump";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\game_" + std::wstring(L"140000000") + L".bin";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log("dump: could not create %ls", path.c_str());
        return;
    }
    DWORD written = 0;
    bool ok = true;
    for (size_t off = 0; off < size && ok; off += 1u << 20) {
        DWORD n = (DWORD)(size - off < (1u << 20) ? size - off : (1u << 20));
        ok = WriteFile(f, image.data() + off, n, &written, nullptr) && written == n;
    }
    CloseHandle(f);

    // The section table, for choosing what to look at in Ghidra.
    std::wstring info = dir + L"\\game_sections.txt";
    FILE* t = _wfopen(info.c_str(), L"w");
    if (t) {
        fprintf(t, "image base %p, SizeOfImage 0x%X, dumped 0x%zX bytes, %zu unreadable page(s)\n", (const void*)base,
            nt.OptionalHeader.SizeOfImage, size, unreadable);
        IMAGE_SECTION_HEADER sec[96] = {};
        int count = nt.FileHeader.NumberOfSections < 96 ? nt.FileHeader.NumberOfSections : 96;
        SafeCopy(sec, base + dos.e_lfanew + sizeof(nt), sizeof(IMAGE_SECTION_HEADER) * count);
        for (int i = 0; i < count; i++)
            fprintf(t, "section %-8.8s  va 0x%08X  size 0x%08X  flags 0x%08X\n", (const char*)sec[i].Name, sec[i].VirtualAddress,
                sec[i].Misc.VirtualSize, sec[i].Characteristics);
        fclose(t);
    }
    Log("dump: %s %ls (%zu unreadable page(s) of %zu)", ok ? "wrote" : "FAILED writing", path.c_str(), unreadable, size / 0x1000);
}

} // namespace md::gamedump
