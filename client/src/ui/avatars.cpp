#include "avatars.h"
#include "overlay.h"
#include "texture.h"
#include "../core/log.h"
#include "../game/steam.h"

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <winhttp.h>

#include <stb_image.h>

namespace md::ui {

// Two sources: the Steam client (instant for friends and yourself, often never for strangers),
// and the public Steam Community profile (works for any public profile, needs an HTTP fetch).
struct Download {
    std::mutex mutex;
    bool done = false;
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
};

struct Entry {
    Texture texture;
    DWORD nextTry = 0; // the Steam client fetches asynchronously: poll at most once a second
    int tries = 0;
    std::shared_ptr<Download> web;
};

static std::unordered_map<uint64_t, Entry> s_avatars;
static constexpr int ClientTriesBeforeWeb = 2;
static constexpr int MaxClientTries = 30;

static bool HttpGet(const std::wstring& url, std::string& body)
{
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
        return false;

    HINTERNET session = WinHttpOpen(L"MultiDiva", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session)
        return false;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, nullptr, nullptr, flags) : nullptr;
    bool ok = false;
    if (request && WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
        char buf[16 * 1024];
        DWORD read = 0;
        ok = status == 200;
        while (ok && WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0 && body.size() < (4u << 20))
            body.append(buf, read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok && !body.empty();
}

// <avatarMedium><![CDATA[https://avatars...._medium.jpg]]></avatarMedium>
static std::string MediumAvatarUrl(const std::string& xml)
{
    const std::string open = "<avatarMedium><![CDATA[";
    size_t a = xml.find(open);
    if (a == std::string::npos)
        return {};
    a += open.size();
    size_t b = xml.find("]]>", a);
    std::string url = b == std::string::npos ? std::string() : xml.substr(a, b - a);
    return url.rfind("https://", 0) == 0 ? url : std::string();
}

// Downloads an image (empty url: nothing) and decodes it into `out`, which is marked done either way.
static void FetchImage(const std::string& url, std::shared_ptr<Download> out)
{
    std::string image;
    int w = 0, h = 0;
    unsigned char* pixels = nullptr;
    if (!url.empty() && HttpGet(std::wstring(url.begin(), url.end()), image))
        pixels = stbi_load_from_memory((const unsigned char*)image.data(), (int)image.size(), &w, &h, nullptr, 4);

    std::lock_guard lock(out->mutex);
    if (pixels && w > 0 && h > 0 && w <= 512 && h <= 512) {
        out->rgba.assign(pixels, pixels + (size_t)w * h * 4);
        out->width = w;
        out->height = h;
    }
    if (pixels)
        stbi_image_free(pixels);
    out->done = true;
}

static void FetchFromCommunity(uint64_t steamId, std::shared_ptr<Download> out)
{
    std::string xml;
    std::wstring profile = L"https://steamcommunity.com/profiles/" + std::to_wstring(steamId) + L"/?xml=1";
    FetchImage(HttpGet(profile, xml) ? MediumAvatarUrl(xml) : std::string(), out);
}

// Texture of a finished download, created on the render thread. Null while pending or if it failed.
static ID3D11ShaderResourceView* Upload(ID3D11Device* device, Entry& e)
{
    if (!e.texture.srv && e.web) {
        std::lock_guard lock(e.web->mutex);
        if (e.web->done && !e.web->rgba.empty())
            CreateTextureRGBA(device, e.web->rgba.data(), e.web->width, e.web->height, e.texture);
    }
    return e.texture.srv;
}

// Custom avatars from the game server, keyed by URL (player ids restart with each server run).
static std::unordered_map<std::string, Entry> s_custom;

static ID3D11ShaderResourceView* CustomAvatar(const std::string& url)
{
    ID3D11Device* device = GetDevice();
    if (!device)
        return nullptr;
    Entry& e = s_custom[url];
    if (!e.web) {
        e.web = std::make_shared<Download>();
        std::thread(FetchImage, url, e.web).detach();
    }
    return Upload(device, e);
}

ID3D11ShaderResourceView* PlayerAvatar(const mp::Player& p)
{
    // A picture the player supplied (avatar.png) wins over their Steam one; the Steam picture covers the moment before
    // it has downloaded, and players without one.
    ID3D11ShaderResourceView* tex = p.hasAvatar ? CustomAvatar(mp::AvatarUrl(p.id)) : nullptr;
    if (!tex)
        tex = Avatar(p.steamId);
    return tex;
}

ID3D11ShaderResourceView* Avatar(uint64_t steamId)
{
    ID3D11Device* device = GetDevice();
    if (!steamId || !device)
        return nullptr;
    Entry& e = s_avatars[steamId];
    if (e.texture.srv)
        return e.texture.srv;

    // Community download finished: turn it into a texture (here, on the render thread).
    if (Upload(device, e))
        return e.texture.srv;

    if (e.tries >= MaxClientTries || GetTickCount() < e.nextTry)
        return nullptr;
    e.tries++;
    e.nextTry = GetTickCount() + 1000;

    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (steam::AvatarRGBA(steamId, rgba, w, h) && CreateTextureRGBA(device, rgba.data(), (int)w, (int)h, e.texture))
        return e.texture.srv;

    if (!e.web && e.tries >= ClientTriesBeforeWeb) {
        e.web = std::make_shared<Download>();
        std::thread(FetchFromCommunity, steamId, e.web).detach();
        Log("avatar: fetching %llu from Steam Community", (unsigned long long)steamId);
    }
    return nullptr;
}

} // namespace md::ui
