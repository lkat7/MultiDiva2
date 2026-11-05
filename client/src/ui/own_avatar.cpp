#include "own_avatar.h"
#include "../core/base64.h"
#include "../core/log.h"
#include "../core/mod.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace md::ui {

namespace {

constexpr size_t MaxFileBytes = 8 << 20;   // don't read more than this from a picture file
constexpr size_t MaxSendBytes = 32 << 10;  // the server's limit (server/avatar.go)
constexpr int MaxSendSide = 256;

bool ReadFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty() && out.size() <= MaxFileBytes;
}

bool IsPngOrJpeg(const std::vector<uint8_t>& d)
{
    static const uint8_t png[] = { 0x89, 'P', 'N', 'G' };
    return (d.size() > 4 && std::equal(png, png + 4, d.begin())) || (d.size() > 3 && d[0] == 0xFF && d[1] == 0xD8);
}

// Centre-crops to a square and shrinks to side x side (area average). RGBA in, RGBA out.
std::vector<uint8_t> SquareScaled(const uint8_t* src, int w, int h, int side)
{
    const int crop = std::min(w, h);
    const int x0 = (w - crop) / 2, y0 = (h - crop) / 2;
    std::vector<uint8_t> out((size_t)side * side * 4);
    for (int y = 0; y < side; y++) {
        int sy0 = y0 + (int)((int64_t)y * crop / side), sy1 = std::max(sy0 + 1, y0 + (int)((int64_t)(y + 1) * crop / side));
        for (int x = 0; x < side; x++) {
            int sx0 = x0 + (int)((int64_t)x * crop / side), sx1 = std::max(sx0 + 1, x0 + (int)((int64_t)(x + 1) * crop / side));
            uint32_t sum[4] = {};
            int n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++, n++)
                    for (int c = 0; c < 4; c++)
                        sum[c] += src[((size_t)sy * w + sx) * 4 + c];
            for (int c = 0; c < 4; c++)
                out[((size_t)y * side + x) * 4 + c] = (uint8_t)(sum[c] / n);
        }
    }
    return out;
}

void Append(void* context, void* data, int size)
{
    auto* v = (std::vector<uint8_t>*)context;
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

// The first encoding that fits the server's size limit: PNG at 128 or 96 px, then JPEG at 128, 96 and 64 px.
bool Encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out)
{
    struct Try { int side; int jpegQuality; }; // quality 0 = PNG
    static const Try tries[] = { { 128, 0 }, { 96, 0 }, { 128, 85 }, { 96, 80 }, { 64, 75 } };
    for (const Try& t : tries) {
        int side = std::min(t.side, std::min(w, h));
        std::vector<uint8_t> px = SquareScaled(rgba, w, h, side);
        out.clear();
        if (t.jpegQuality == 0)
            stbi_write_png_to_func(Append, &out, side, side, 4, px.data(), side * 4);
        else
            stbi_write_jpg_to_func(Append, &out, side, side, 4, px.data(), t.jpegQuality);
        if (!out.empty() && out.size() <= MaxSendBytes)
            return true;
    }
    return false;
}

std::string Load()
{
    static const wchar_t* names[] = { L"avatar.png", L"avatar.jpg", L"avatar.jpeg" };
    const std::wstring* dirs[] = { &ModDir(), &DataDir() };
    for (const std::wstring* dir : dirs) {
        for (const wchar_t* name : names) {
            std::wstring path = *dir + L"\\" + name;
            std::vector<uint8_t> file;
            if (!ReadFile(path, file))
                continue;
            int w = 0, h = 0, comp = 0;
            if (!IsPngOrJpeg(file) || !stbi_info_from_memory(file.data(), (int)file.size(), &w, &h, &comp) || w <= 0 || h <= 0) {
                Log("avatar: %ls is not a usable PNG/JPEG, ignored", path.c_str());
                continue;
            }
            // Square, small and light enough already: send the file untouched.
            if (w == h && w <= MaxSendSide && file.size() <= MaxSendBytes) {
                Log("avatar: using %ls as it is (%dx%d, %zu bytes)", path.c_str(), w, h, file.size());
                return Base64Encode(file.data(), file.size());
            }
            uint8_t* rgba = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &comp, 4);
            if (!rgba) {
                Log("avatar: could not decode %ls", path.c_str());
                continue;
            }
            std::vector<uint8_t> encoded;
            bool ok = Encode(rgba, w, h, encoded);
            stbi_image_free(rgba);
            if (!ok) {
                Log("avatar: %ls is too detailed to fit 32 KB even when shrunk, ignored", path.c_str());
                continue;
            }
            Log("avatar: using %ls (resized from %dx%d to %zu bytes)", path.c_str(), w, h, encoded.size());
            return Base64Encode(encoded.data(), encoded.size());
        }
    }
    return {};
}

} // namespace

const std::string& OwnAvatarBase64()
{
    static const std::string avatar = Load();
    return avatar;
}

} // namespace md::ui
