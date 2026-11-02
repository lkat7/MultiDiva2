#include "config.h"
#include "log.h"
#include "mod.h"

#include <fstream>
#include <windows.h>

namespace md {

static Config s_config;

Config& GetConfig() { return s_config; }

static std::wstring ConfigPath() { return DataFile(L"multidiva.ini"); }

static std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

static std::string WindowsUserName()
{
    wchar_t name[128];
    DWORD len = 128;
    if (!GetUserNameW(name, &len))
        return "Player";
    char utf8[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return n > 1 ? std::string(utf8) : "Player";
}

void LoadConfig()
{
    std::ifstream in(ConfigPath());
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#' || line[0] == ';')
            continue;
        std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "name") s_config.name = value;
        else if (key == "server") s_config.server = value;
        else if (key == "port") s_config.port = atoi(value.c_str());
        else if (key == "room") s_config.room = value;
        else if (key == "leaderboard") s_config.showLeaderboard = value != "0";
        else if (key == "gaps") s_config.showGaps = value == "1";
        else if (key == "countdown_sound") s_config.countdownSound = value == "1";
        else if (key == "own_outfits") s_config.ownOutfits = value != "0";
        else if (key == "tunnel_url") s_config.tunnelUrl = value;
        else if (key == "tunnel_token") s_config.tunnelToken = value;
        else if (key == "research") s_config.research = value == "1";
    }
    if (s_config.name.empty())
        s_config.name = WindowsUserName();
    if (s_config.port <= 0 || s_config.port > 65535)
        s_config.port = 3939;
}

void SaveConfig()
{
    std::ofstream out(ConfigPath(), std::ios::trunc);
    if (!out) {
        Log("config: could not write multidiva.ini");
        return;
    }
    out << "# MultiDiva settings\n"
        << "name=" << s_config.name << "\n"
        << "server=" << s_config.server << "\n"
        << "port=" << s_config.port << "\n"
        << "room=" << s_config.room << "\n"
        << "leaderboard=" << (s_config.showLeaderboard ? 1 : 0) << "\n"
        << "gaps=" << (s_config.showGaps ? 1 : 0) << "\n"
        << "countdown_sound=" << (s_config.countdownSound ? 1 : 0) << "\n"
        << "own_outfits=" << (s_config.ownOutfits ? 1 : 0) << "\n"
        << "tunnel_url=" << s_config.tunnelUrl << "\n"
        << "tunnel_token=" << s_config.tunnelToken << "\n";
    if (s_config.research) // only written for whoever turned it on by hand
        out << "research=1\n";
}

} // namespace md
