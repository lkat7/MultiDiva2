#pragma once
#include <string>

namespace md {

// Player settings, stored in %LOCALAPPDATA%\MultiDiva2\multidiva.ini (key=value lines).
struct Config {
    std::string name;
    std::string server = "127.0.0.1";
    int port = 3939;
    std::string room = "miku";
    bool showLeaderboard = true;
    bool showGaps = true; // the other players show their score difference to you instead of their whole score
    bool countdownSound = false; // play the game's start sound on each 3-2-1 tick before a round starts
    bool ownOutfits = true; // in a round, the performers wear my own outfits for the song, not the host's
    bool research = false;  // Ctrl+F8: show the reverse-engineering tools too ("research=1" in the ini; off for players)

    // Optional permanent hosting address (a named Cloudflare tunnel). The token is a secret:
    // it only lives in this local file.
    std::string tunnelUrl;   // e.g. wss://multidiva.example.com
    std::string tunnelToken;
};

Config& GetConfig();
void LoadConfig();
void SaveConfig();

} // namespace md
