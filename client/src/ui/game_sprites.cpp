#include "game_sprites.h"
#include "overlay.h"
#include "../core/log.h"
#include "../game/memory.h"
#ifdef MD_SPRITE_PACK
#include "texture.h"
#include <cstdio>
#endif

#include <d3dcompiler.h>
#include <imgui_internal.h>
#include <windows.h>
#include <map>
#include <string>
#include <vector>

namespace md::ui::sprites {

// ---- the game's sprite set map (see docs/re-notes.md) ----
namespace addr {
constexpr uintptr_t SetMap = 0x14CBFF3A0; // std::map<uint32 set id, SPRSET>: { node* head, size }
constexpr size_t NodeKey = 0x20, NodeSet = 0x28; // node: left, parent, right, color, isnil (+0x19)
constexpr size_t SetHeader = 0x18;   // SPRSET: relocated sprite set header
constexpr size_t SetTextures = 0x20; // SPRSET: texture object pointers
constexpr size_t TextureD3D = 0x28;  // texture object -> D3D holder
constexpr size_t D3DFirst = 0x10, D3DSecond = 0x18; // holder: texture and shader view (checked by QueryInterface)
} // namespace addr

struct SetCache {
    bool done = false;
    std::map<std::string, Sprite> sprites;
    std::vector<ID3D11ShaderResourceView*> views; // our references
};

static const uint32_t s_wanted[] = { set::MainMenu, set::Common, set::SongSelect, set::FontThin, set::FontBold };
static std::map<uint32_t, SetCache> s_sets;

static uintptr_t FindSetNode(uint32_t id)
{
    uintptr_t head = 0, node = 0;
    if (!SafeRead(addr::SetMap, head) || !head || !SafeRead(head + 8, node))
        return 0;
    for (int depth = 0; node && node != head && depth < 64; depth++) {
        uint32_t key = 0;
        uint8_t isNil = 1;
        if (!SafeRead(node + 0x19, isNil) || isNil || !SafeRead(node + addr::NodeKey, key))
            return 0;
        if (key == id)
            return node;
        if (!SafeRead(node + (id < key ? 0 : 0x10), node))
            return 0;
    }
    return 0;
}

// True if `p` points at a Direct3D 11 object (its vtable lies inside d3d11.dll): only then is it safe to call.
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

// The shader view of a game texture object, AddRef'd (null if the layout isn't what we expect).
static ID3D11ShaderResourceView* ViewOf(uintptr_t texture)
{
    uintptr_t holder = 0;
    if (!SafeRead(texture + addr::TextureD3D, holder) || !holder)
        return nullptr;
    for (size_t off : { addr::D3DSecond, addr::D3DFirst }) {
        uintptr_t p = 0;
        if (!SafeRead(holder + off, p) || !IsD3DObject(p))
            continue;
        IUnknown* unk = (IUnknown*)p;
        ID3D11ShaderResourceView* view = nullptr;
        if (SUCCEEDED(unk->QueryInterface(__uuidof(ID3D11ShaderResourceView), (void**)&view)))
            return view;
    }
    return nullptr;
}

static bool CopySet(uint32_t id, SetCache& out)
{
    uintptr_t node = FindSetNode(id);
    uintptr_t header = 0, textures = 0;
    if (!node || !SafeRead(node + addr::NodeSet + addr::SetHeader, header) || !header
        || !SafeRead(node + addr::NodeSet + addr::SetTextures, textures) || !textures)
        return false;
    uint32_t texCount = 0, sprCount = 0;
    uintptr_t infos = 0, names = 0;
    if (!SafeRead(header + 8, texCount) || !SafeRead(header + 12, sprCount) || !SafeRead(header + 0x10, infos)
        || !SafeRead(header + 0x20, names) || texCount == 0 || texCount > 256 || sprCount > 4096)
        return false;

    std::vector<ID3D11ShaderResourceView*> views(texCount);
    std::vector<bool> ycbcr(texCount);
    for (uint32_t t = 0; t < texCount; t++) {
        uintptr_t tex = 0;
        if (!SafeRead(textures + t * 8, tex) || !tex || !(views[t] = ViewOf(tex))) {
            for (ID3D11ShaderResourceView* v : views)
                if (v) v->Release();
            Log("sprites: set %u: texture %u not readable yet", id, t);
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC d;
        views[t]->GetDesc(&d);
        ycbcr[t] = d.Format == DXGI_FORMAT_BC5_UNORM && d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D
            && d.Texture2D.MipLevels >= 2;
    }
    for (uint32_t i = 0; i < sprCount; i++) {
        struct Info { uint32_t texture, pad; float u0, v0, u1, v1, x, y, w, h; } info;
        uintptr_t namePtr = 0;
        char name[64] = {};
        if (!SafeRead(infos + i * sizeof(Info), info) || !SafeRead(names + i * 8, namePtr)
            || !SafeCopy(name, (const void*)namePtr, sizeof(name) - 1) || info.texture >= texCount)
            continue;
        Sprite s;
        s.srv = views[info.texture];
        s.uv0 = ImVec2(info.u0, 1.0f - info.v0); // sheets are stored bottom-up
        s.uv1 = ImVec2(info.u1, 1.0f - info.v1);
        s.size = ImVec2(info.w, info.h);
        s.ycbcr = ycbcr[info.texture];
        out.sprites[name] = s;
    }
    out.views = std::move(views);
    out.done = true;
    Log("sprites: set %u copied: %zu sprites, %u textures", id, out.sprites.size(), texCount);
    return true;
}

// ---- song jackets: each song's select set (SPR_SEL_PV%03d: SONG_JK, SONG_LOGO, SONG_BG), loaded on request ----
//
// The game's own sprite API (docs/re-notes.md "Song jackets"): set name -> id, request (async file read), wait
// (call every frame until the set is ready), free. We copy the jacket once loaded and free the set right away:
// our references keep its textures alive. The last few jackets stay cached.

namespace jk {
#ifndef MD_SPRITE_PACK
constexpr uintptr_t SetIdByName = 0x1405BC770; // (unused, const {char* begin, end}*) -> const uint32_t* id
constexpr uintptr_t SetEntry = 0x1405BC600;    // (unused, uint32_t id) -> entry: +0x28 index in the set map
constexpr uintptr_t Request = 0x1405B4770;     // spr::request(uint32_t id, const {char* begin, end}* data dir)
constexpr uintptr_t Wait = 0x1405B4810;        // spr::wait(uint32_t id)
constexpr uintptr_t Free = 0x1405B48B0;        // spr::free(uint32_t id)
constexpr size_t SetReady = 0x38;              // SPRSET: ready flag (set by wait once the file is parsed)

struct Range { const char* begin; const char* end; };
using SetIdByNameFn = const uint32_t* (*)(void*, const Range*);
using SetEntryFn = const uint8_t* (*)(void*, uint32_t);
using IdFn = void (*)(uint32_t, const Range*);
using Id1Fn = void (*)(uint32_t);

// The first bytes of each function, checked once before anything is called (another game version: no jackets).
static bool CodeLooksRight()
{
    static int ok = -1;
    if (ok < 0) {
        struct Check { uintptr_t at; uint8_t bytes[8]; } checks[] = {
            { SetIdByName, { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89 } },
            { SetEntry, { 0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xEC, 0x38 } },
            { Request, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 } },
            { Wait, { 0x48, 0x83, 0xEC, 0x28, 0x8B, 0xD1, 0xE8, 0xE5 } },
            { Free, { 0x48, 0x83, 0xEC, 0x28, 0x8B, 0xD1, 0xE8, 0x45 } },
        };
        ok = 1;
        for (const Check& ch : checks) {
            uint8_t got[8] = {};
            if (!SafeCopy(got, (const void*)ch.at, 8) || memcmp(got, ch.bytes, 8) != 0) {
                Log("jackets: game code at %llX is not what we expect: song jackets off", (unsigned long long)ch.at);
                ok = 0;
                break;
            }
        }
    }
    return ok == 1;
}
#endif

struct Jacket {
    uint32_t setId = 0xFFFFFFFF, index = 0;
    bool requested = false, failed = false, loggedMissing = false;
    int waitFrames = 0;
    SetCache cache;
    DWORD usedAt = 0;
};
static std::map<int, Jacket> s_jackets;
constexpr size_t MaxCached = 12;
constexpr int MaxWaitFrames = 60 * 20; // give up on a set that never loads

static void Release(Jacket& j)
{
    for (ID3D11ShaderResourceView* v : j.cache.views)
        if (v) v->Release();
    j.cache = SetCache{};
}

static void Evict()
{
    while (s_jackets.size() > MaxCached) {
        auto oldest = s_jackets.begin();
        for (auto it = s_jackets.begin(); it != s_jackets.end(); ++it)
            if (it->second.usedAt < oldest->second.usedAt)
                oldest = it;
        Release(oldest->second);
        s_jackets.erase(oldest);
    }
}

#ifndef MD_SPRITE_PACK
// Set id and map index of a song's select set; false if the song has none.
static bool Lookup(int pv, uint32_t& id, uint32_t& index)
{
    char name[48];
    int n = snprintf(name, sizeof(name), "SPR_SEL_PV%03d", pv);
    Range r{ name, name + n };
    const uint32_t* found = ((SetIdByNameFn)SetIdByName)(nullptr, &r);
    if (!found || !SafeRead((uintptr_t)found, id) || id == 0xFFFFFFFF)
        return false;
    const uint8_t* entry = ((SetEntryFn)SetEntry)(nullptr, id);
    return entry && SafeRead((uintptr_t)entry + 0x28, index) && index != 0xFFFFFFFF;
}

static bool SetReadyNow(uint32_t index)
{
    uintptr_t node = FindSetNode(index);
    uint8_t ready = 0;
    uintptr_t header = 0;
    return node && SafeRead(node + addr::NodeSet + SetReady, ready) && ready
        && SafeRead(node + addr::NodeSet + addr::SetHeader, header) && header;
}
#endif

// Every frame (render thread, which is the game's): moves pending requests along.
static void TickJackets()
{
#ifndef MD_SPRITE_PACK
    for (auto& [pv, j] : s_jackets) {
        if (!j.requested || j.cache.done || j.failed)
            continue;
        ((Id1Fn)Wait)(j.setId);
        if (SetReadyNow(j.index) && CopySet(j.index, j.cache)) {
            ((Id1Fn)Free)(j.setId); // our references keep the textures
            j.requested = false;
        } else if (++j.waitFrames > MaxWaitFrames) {
            Log("jackets: song %d's set never loaded", pv);
            ((Id1Fn)Free)(j.setId);
            j.requested = false;
            j.failed = true;
        }
    }
#endif
}

} // namespace jk

const Sprite* Jacket(int pv)
{
    if (pv < 0)
        return nullptr;
    jk::Jacket& j = jk::s_jackets[pv];
    j.usedAt = GetTickCount();
    char name[32];
    snprintf(name, sizeof(name), "SONG_JK%03d", pv);
    if (j.cache.done) {
        auto it = j.cache.sprites.find(name);
        if (it == j.cache.sprites.end()) {
            // A mod song may name its jacket differently: take any "SONG_JK..." sprite that isn't the Extra chart's.
            for (auto cand = j.cache.sprites.begin(); cand != j.cache.sprites.end(); ++cand)
                if (cand->first.rfind("SONG_JK", 0) == 0 && cand->first.find("_EX") == std::string::npos) {
                    it = cand;
                    break;
                }
            if (!j.loggedMissing) {
                j.loggedMissing = true;
                std::string names;
                int n = 0;
                for (const auto& [sprite, unused] : j.cache.sprites)
                    if (n++ < 12)
                        names += " " + sprite;
                Log("jackets: pv %d: set loaded, no sprite %s%s; sprites:%s", pv, name,
                    it == j.cache.sprites.end() ? "" : " (using another SONG_JK one)", names.c_str());
            }
        }
        return it == j.cache.sprites.end() ? nullptr : &it->second;
    }
#ifndef MD_SPRITE_PACK
    if (!j.requested && !j.failed && jk::CodeLooksRight()) {
        if (!jk::Lookup(pv, j.setId, j.index)) {
            j.failed = true; // no select set for this song (or not installed)
            Log("jackets: pv %d has no select set (SPR_SEL_PV%03d not in the sprite database)", pv, pv);
        } else if (jk::SetReadyNow(j.index) && CopySet(j.index, j.cache)) {
            // already loaded by the game (song select has it highlighted)
        } else {
            jk::Range none{ nullptr, nullptr }; // the default folder (./rom/2d/, redirected by mods)
            ((jk::IdFn)jk::Request)(j.setId, &none);
            j.requested = true;
            j.waitFrames = 0;
        }
        jk::Evict();
    }
#endif
    return nullptr;
}

void Tick()
{
    jk::TickJackets();
    static DWORD lastTry = 0;
    if (GetTickCount() - lastTry < 500)
        return;
    lastTry = GetTickCount();
    for (uint32_t id : s_wanted) {
        SetCache& c = s_sets[id];
        if (!c.done)
            CopySet(id, c);
    }
}

bool Ready(uint32_t setId)
{
    auto it = s_sets.find(setId);
    return it != s_sets.end() && it->second.done;
}

const Sprite* Find(uint32_t setId, const char* name)
{
    auto it = s_sets.find(setId);
    if (it == s_sets.end())
        return nullptr;
    auto sp = it->second.sprites.find(name);
    return sp == it->second.sprites.end() ? nullptr : &sp->second;
}

// ---- drawing: YCbCr sheets need their own pixel shader ----

static ID3D11PixelShader* s_ycbcr = nullptr;
static ID3D11SamplerState* s_sampler = nullptr;

static bool CreateShader()
{
    static bool tried = false;
    if (tried)
        return s_ycbcr != nullptr;
    tried = true;
    ID3D11Device* device = GetDevice();
    if (!device)
        return false;
    // Same inputs as ImGui's pixel shader. Mip 0: luma (r) + alpha (g); mip 1: chroma (MikuMikuLibrary's
    // TextureDecoderCore::DecodeYCbCr, BT.709).
    static const char* src = R"(
        struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
        sampler sampler0;
        Texture2D texture0;
        float4 main(PS_INPUT input) : SV_Target
        {
            float2 la = texture0.SampleLevel(sampler0, input.uv, 0).rg;
            float2 c = texture0.SampleLevel(sampler0, input.uv, 1).rg * 1.003922 - 0.503929;
            float3 rgb = float3(la.r + 1.5748 * c.g, la.r - 0.1873 * c.r - 0.4681 * c.g, la.r + 1.8556 * c.r);
            return float4(saturate(rgb), la.g) * input.col;
        })";
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, &errors))) {
        Log("sprites: shader compile failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
        if (errors) errors->Release();
        return false;
    }
    device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_ycbcr);
    blob->Release();
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &s_sampler);
    return s_ycbcr != nullptr;
}

static void UseYCbCr(const ImDrawList*, const ImDrawCmd*)
{
    ID3D11Device* device = GetDevice();
    if (!device || !s_ycbcr)
        return;
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    ctx->PSSetShader(s_ycbcr, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &s_sampler);
    ctx->Release();
}

void Draw(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, ImU32 tint)
{
    if (!s.srv)
        return;
    if (s.ycbcr) {
        if (!CreateShader())
            return;
        dl->AddCallback(UseYCbCr, nullptr);
        dl->AddImage((ImTextureID)s.srv, a, b, s.uv0, s.uv1, tint);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } else {
        dl->AddImage((ImTextureID)s.srv, a, b, s.uv0, s.uv1, tint);
    }
}

// ---- recolor: a grey sprite mapped black -> `low` (at grey g0) -> `high` (at grey g1 and above) ----

struct RecolorParams {
    float low[4], high[4];
    float g0, g1, ycbcr, pad;
};
static ID3D11PixelShader* s_recolor = nullptr;
static ID3D11Buffer* s_recolorParams = nullptr;
// Each draw's parameters live until the frame is rendered: a ring big enough for a frame's worth of draws.
static RecolorParams s_recolorRing[512];
static unsigned s_recolorNext = 0;

static bool CreateRecolor()
{
    static bool tried = false;
    if (tried)
        return s_recolor != nullptr;
    tried = true;
    ID3D11Device* device = GetDevice();
    if (!device || !CreateShader()) // also makes the sampler
        return false;
    static const char* src = R"(
        struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
        cbuffer Params : register(b0) { float4 low; float4 high; float4 stops; };
        sampler sampler0;
        Texture2D texture0;
        float4 main(PS_INPUT input) : SV_Target
        {
            float4 c;
            if (stops.z > 0.5) {
                float2 la = texture0.SampleLevel(sampler0, input.uv, 0).rg;
                float2 ch = texture0.SampleLevel(sampler0, input.uv, 1).rg * 1.003922 - 0.503929;
                c = float4(0, la.r - 0.1873 * ch.r - 0.4681 * ch.g, 0, la.g); // only green (grey) is used
            } else {
                c = texture0.Sample(sampler0, input.uv);
            }
            float g = c.g;
            float3 rgb = g < stops.x ? low.rgb * (g / stops.x) : lerp(low.rgb, high.rgb, saturate((g - stops.x) / (stops.y - stops.x)));
            return float4(rgb, c.a) * input.col;
        })";
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, &errors))) {
        Log("sprites: recolor shader compile failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
        if (errors) errors->Release();
        return false;
    }
    device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_recolor);
    blob->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(RecolorParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &s_recolorParams);
    return s_recolor && s_recolorParams;
}

static void UseRecolor(const ImDrawList*, const ImDrawCmd* cmd)
{
    ID3D11Device* device = GetDevice();
    if (!device || !s_recolor)
        return;
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(s_recolorParams, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, cmd->UserCallbackData, sizeof(RecolorParams));
        ctx->Unmap(s_recolorParams, 0);
    }
    ctx->PSSetConstantBuffers(0, 1, &s_recolorParams);
    ctx->PSSetShader(s_recolor, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &s_sampler);
    ctx->Release();
}

void DrawRecolored(ImDrawList* dl, const Sprite& s, ImVec2 a, ImVec2 b, ImU32 low, ImU32 high, float g0, float g1,
    ImU32 tint)
{
    if (!s.srv || !CreateRecolor())
        return;
    RecolorParams& p = s_recolorRing[s_recolorNext++ % (sizeof(s_recolorRing) / sizeof(s_recolorRing[0]))];
    ImVec4 l = ImGui::ColorConvertU32ToFloat4(low), h = ImGui::ColorConvertU32ToFloat4(high);
    p = { { l.x, l.y, l.z, 1 }, { h.x, h.y, h.z, 1 }, g0, g1, s.ycbcr ? 1.0f : 0.0f, 0 };
    dl->AddCallback(UseRecolor, &p);
    dl->AddImage((ImTextureID)s.srv, a, b, s.uv0, s.uv1, tint);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// ---- text ----

namespace font {
// The game's fonts are static 0x40-byte records (ReDIVA's `font`): +0 i32 sprite id, +4 u8 glyph w/h, box w/h,
// +8 u8 column scale num/denom, +0xC i32 fontmap id, +0x10 f32 column scale, +0x18 u64 characters per row,
// +0x20 std::vector of 6-byte characters (init, halfwidth, column, row, glyph offset, glyph width) indexed by
// UTF-16 code, +0x38 bool spacing.
constexpr uintptr_t ThinRecord = 0x14CC60FF0; // fontmap 20, sprite SPR_FNT_36LATIN9_DIVA_36_38
constexpr uintptr_t BoldRecord = 0x14CC61030; // fontmap 21, sprite SPR_FNT_BOLD36LATIN9_DIVA_36_38
constexpr int Glyph = 36, Box = 38;
constexpr size_t MaxChars = 0x10000;
} // namespace font

struct Glyph { uint8_t init, halfwidth, column, row, offset, width; };
static_assert(sizeof(Glyph) == 6);

struct FontCache {
    bool tried = false;
    std::vector<Glyph> glyphs;
};
static FontCache s_fonts[2];

// The font's character table, copied once (null if the game's font record isn't what we expect).
static const std::vector<Glyph>* Glyphs(Font f)
{
    FontCache& c = s_fonts[(int)f];
    if (c.tried)
        return c.glyphs.empty() ? nullptr : &c.glyphs;
    c.tried = true;
    uintptr_t rec = f == Font::Bold ? font::BoldRecord : font::ThinRecord;
    int32_t mapId = 0;
    uint8_t sizes[4] = {};
    uintptr_t begin = 0, end = 0;
    if (!SafeRead(rec + 0x0C, mapId) || !SafeCopy(sizes, (const void*)(rec + 0x04), 4)
        || !SafeRead(rec + 0x20, begin) || !SafeRead(rec + 0x28, end)) {
        Log("text: font record %llX unreadable", (unsigned long long)rec);
        return nullptr;
    }
    size_t count = end > begin ? (end - begin) / sizeof(Glyph) : 0;
    if (!begin) {
        c.tried = false; // the game hasn't read its font map yet: ask again later
        return nullptr;
    }
    if (mapId != (f == Font::Bold ? 21 : 20) || sizes[0] != font::Glyph || sizes[2] != font::Box || !count
        || (end - begin) % sizeof(Glyph) || count > font::MaxChars) {
        Log("text: font record %llX is not the expected font (map %d, %u/%u)", (unsigned long long)rec, mapId,
            sizes[0], sizes[2]);
        return nullptr;
    }
    std::vector<Glyph> g(count);
    if (!SafeCopy(g.data(), (const void*)begin, count * sizeof(Glyph))) {
        Log("text: font table unreadable");
        return nullptr;
    }
    c.glyphs = std::move(g);
    Log("text: %s font: %zu characters", f == Font::Bold ? "bold" : "thin", count);
    return &c.glyphs;
}

static const Sprite* Sheet(Font f)
{
    return Find(f == Font::Bold ? set::FontBold : set::FontThin, "DIVA_36_38");
}

bool TextReady(Font f) { return Sheet(f) && Glyphs(f); }

static const Glyph* GlyphOf(const std::vector<Glyph>& g, unsigned c)
{
    return c < g.size() && g[c].init ? &g[c] : nullptr;
}

// Advance of a letter in sheet pixels (a missing letter takes a space's width).
static float Advance(const std::vector<Glyph>& g, const Glyph* gl)
{
    if (!gl)
        gl = GlyphOf(g, ' ');
    return gl ? gl->width : font::Glyph / 2.0f;
}

float TextWidth(Font f, const char* text, float px, float spacing)
{
    const std::vector<Glyph>* g = Glyphs(f);
    if (!g)
        return 0;
    float scale = px / font::Glyph, x = 0;
    for (const char* p = text; *p;) {
        unsigned c = 0;
        p += ImTextCharFromUtf8(&c, p, nullptr);
        x += Advance(*g, GlyphOf(*g, c)) * scale + spacing;
    }
    return x > 0 ? x - spacing : 0;
}

void DrawText(ImDrawList* dl, Font f, ImVec2 pos, const char* text, float px, ImU32 col, float spacing)
{
    const std::vector<Glyph>* g = Glyphs(f);
    const Sprite* sheet = Sheet(f);
    if (!g || !sheet || !sheet->srv)
        return;
    // Sheet pixel -> texture coordinate, inside the sprite's (already flipped) rectangle.
    ImVec2 du((sheet->uv1.x - sheet->uv0.x) / sheet->size.x, (sheet->uv1.y - sheet->uv0.y) / sheet->size.y);
    float scale = px / font::Glyph, x = pos.x;
    for (const char* p = text; *p;) {
        unsigned c = 0;
        p += ImTextCharFromUtf8(&c, p, nullptr);
        const Glyph* gl = GlyphOf(*g, c);
        if (gl && c != ' ') {
            float sx = (float)gl->column * font::Box + gl->offset, sy = (float)gl->row * font::Box;
            ImVec2 uv0(sheet->uv0.x + sx * du.x, sheet->uv0.y + sy * du.y);
            ImVec2 uv1(sheet->uv0.x + (sx + gl->width) * du.x, sheet->uv0.y + (sy + font::Glyph) * du.y);
            dl->AddImage((ImTextureID)sheet->srv, ImVec2(x, pos.y), ImVec2(x + gl->width * scale, pos.y + px), uv0,
                uv1, col);
        }
        x += Advance(*g, gl) * scale + spacing;
    }
}

#ifdef MD_SPRITE_PACK
// tools/ui-preview only: the same sets from a local folder of decoded sheets (made by a research script, never
// shipped). index.txt: "tex <set> <n> <file>" and "spr <set> <name> <tex> <x> <y> <w> <h>" (top-down pixels).
bool LoadPack(const char* dir)
{
    std::string base = dir;
    FILE* f = fopen((base + "\\index.txt").c_str(), "r");
    if (!f)
        return false;
    std::map<std::pair<uint32_t, int>, Texture> textures;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned set = 0;
        int tex = 0;
        char name[256];
        float x, y, w, h;
        if (sscanf(line, "tex %u %d %255s", &set, &tex, name) == 3) {
            Texture t;
            if (LoadTextureFromFile(GetDevice(), (base + "\\" + name).c_str(), t))
                textures[{ set, tex }] = t;
        } else if (sscanf(line, "spr %u %255s %d %f %f %f %f", &set, name, &tex, &x, &y, &w, &h) == 7) {
            auto it = textures.find({ set, tex });
            if (it == textures.end())
                continue;
            const Texture& t = it->second;
            Sprite s;
            s.srv = t.srv;
            s.uv0 = ImVec2(x / t.width, y / t.height);
            s.uv1 = ImVec2((x + w) / t.width, (y + h) / t.height);
            s.size = ImVec2(w, h);
            s_sets[set].sprites[name] = s;
            s_sets[set].done = true;
        }
    }
    fclose(f);
    const char* tables[] = { "\\font_thin.bin", "\\font_bold.bin" };
    for (int i = 0; i < 2; i++) {
        FILE* t = fopen((base + tables[i]).c_str(), "rb");
        if (!t)
            continue;
        std::vector<Glyph>& g = s_fonts[i].glyphs;
        g.resize(font::MaxChars);
        g.resize(fread(g.data(), sizeof(Glyph), g.size(), t));
        s_fonts[i].tried = true;
        fclose(t);
    }
    return !s_sets.empty();
}
#endif

static bool s_test = false;
void SetTest(bool on) { s_test = on; }
bool TestEnabled() { return s_test; }

void DrawTest()
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float s = vp->Size.y / 1080.0f;
    if (!Ready(set::MainMenu)) {
        dl->AddText(ImVec2(vp->Pos.x + 40, vp->Pos.y + 40), IM_COL32(255, 80, 80, 255),
            "Sprite test: open the main menu once so its sprites can be read.");
        return;
    }
    // A selected button (glow + plate + label), an unselected one, a dialog button and the footer.
    struct Item { const char* name; float x, y; } items[] = {
        { "SELECTOR_RHYTHMGAME_EFF", 40, 120 }, { "SELECTOR_RHYTHMGAME", 120, 140 }, { "SELECTOR_RHYTHMGAME_TXT", 120, 140 },
        { "SELECTOR_OPTION", 120, 340 }, { "SELECTOR_OPTION_TXT", 120, 340 },
        { "BTN_TXT_YES_SEL", 1100, 160 }, { "BTN_TXT_NO", 1100, 240 }, { "SELECTOR_ARCADE_TXT_SEL", 1100, 330 },
        { "FOOTER_RIGHT", 1142, 900 },
    };
    for (const Item& it : items) {
        const Sprite* sp = Find(set::MainMenu, it.name);
        if (!sp)
            continue;
        ImVec2 a(vp->Pos.x + it.x * s, vp->Pos.y + it.y * s);
        Draw(dl, *sp, a, ImVec2(a.x + sp->size.x * s, a.y + sp->size.y * s));
    }
    // A jacket the game is asked to load for us (song 1), which proves request/wait/copy work.
    {
        ImVec2 a(vp->Pos.x + 1500 * s, vp->Pos.y + 100 * s), b(a.x + 300 * s, a.y + 300 * s);
        if (const Sprite* j = Jacket(1))
            Draw(dl, *j, a, b);
        else
            dl->AddText(a, IM_COL32(255, 80, 80, 255), "jacket of song 1: loading...");
    }
    // Text in the game's font on a white plate: song select's title look (bold), then the thin font.
    static const char* lines[] = { "Weekender Girl", "Miku Bot - ready  (2)  1,234,567 pt  98.76%",
        (const char*)u8"初音ミク  é è ß Ñ  Дива  ★♪" };
    ImVec2 at(vp->Pos.x + 420 * s, vp->Pos.y + 560 * s);
    dl->AddRectFilled(at, ImVec2(at.x + 1000 * s, at.y + 300 * s), IM_COL32(250, 250, 250, 235), 8 * s);
    for (int f = 0; f < 2; f++) {
        Font font = f ? Font::Thin : Font::Bold;
        float y = at.y + (12 + f * 145) * s;
        if (!TextReady(font)) {
            dl->AddText(ImVec2(at.x + 20 * s, y), IM_COL32(200, 40, 40, 255), f ? "thin font not ready" : "bold font not ready");
            continue;
        }
        for (int i = 0; i < 3; i++) {
            float px = (i ? 34 : 43) * s;
            DrawText(dl, font, ImVec2(at.x + 20 * s, y), lines[i], px, IM_COL32(40, 40, 40, 255), px * TitleSpacingOfSize);
            y += px + 6 * s;
        }
    }
}

void LogStatus()
{
    for (uint32_t id : s_wanted) {
        auto it = s_sets.find(id);
        Log("sprites: set %u: %s", id,
            it != s_sets.end() && it->second.done ? (std::to_string(it->second.sprites.size()) + " sprites").c_str()
                                                  : "not seen loaded yet");
    }
}

} // namespace md::ui::sprites
