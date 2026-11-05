# MultiDiva2

Online multiplayer for **Hatsune Miku: Project DIVA Mega Mix+** (Steam), in the spirit of osu! multiplayer lobbies.

Join a room with friends, the host picks a song, and it starts for everyone at the same moment, with each player's own outfits. You get a live leaderboard while you play and shared results at the end.

> Work in progress. It works and you can play with it, but expect some rough edges.

## Features

- Rooms with chat, a host, and a ready check. Leaving song select unreadies you, and readying, unreadying and a new host play the game's own sounds
- The host plays any song in song select and it starts for everyone in the room, all at the same moment, after a 3-2-1 countdown (with an optional tick sound, in Ctrl+F8)
- In a room only the host can start a song, and only when everyone is ready: confirm in song select is refused otherwise, with a notice on screen (the host presses confirm again within 5 seconds to start anyway)
- A live leaderboard during the song (it can be hidden): your own card is wider and outlined, the others show how far ahead or behind you they are (Score gaps, on by default), and a player whose updates stop for 2 seconds shows a LAG chip while one still loading shows "loading". Shared results afterwards, and round wins counted per room ("2 wins")
- The host can rank the room by score (default) or by accuracy ("Win by" in the room options)
- The host's highlighted song shows with its cover, BPM and the charts it has as muted difficulty chips, with the chart the host selected filled in; browsing the list and changing the difficulty plays the game's cursor sounds for everyone else
- Everyone plays with their own outfits, or with the host's
- Warns when someone in the room doesn't have the song the host is looking at (custom song packs for example)
- The host can hand over host or kick players (right-click a player)
- Steam profile pictures, or your own: drop an `avatar.png` next to `MultiDiva.dll` (it is shrunk to fit)
- **Reconnecting:** if your connection drops (mobile data, wifi hiccup), the mod retries for about a minute and puts you back in your room and round with your score. A host whose connection is down when they start a song plays it solo instead of waiting on the loading screen, and live scores slow down from 10 to 5 to 3 per second on a bad connection. Needs the 2.2.0 server and client
- Pausing is blocked during a multiplayer round (press Esc twice to pause anyway), and alt-tab doesn't pause either
- **Spectator page:** the server also serves a live web page (`http://<server address>/`) with every room, its players and Steam pictures, the song, and the live leaderboard, for friends who just want to watch. It never shows chat. `-spectate=false` turns it off ([docs/hosting.md](docs/hosting.md))
- **Host a server** in one click: runs a small server on your PC with a free Cloudflare tunnel, so friends can join from anywhere without router setup
- Miku Bot, a practice opponent for testing alone (if u dont have friends lol)
- **Coming next (probably the next release):** multiplayer through Steam directly, so you can join and invite friends from Steam instead of sharing an address

## How to Install

1. Install [Diva Mod Manager](https://github.com/TekkaGB/DivaModManager) (it sets up DivaModLoader for you).
2. Drag the MultiDiva2 zip from the [releases](../../releases) onto Diva Mod Manager, make sure MultiDiva2 is ticked, and
   launch the game.

## Play

| Key | |
|---|---|
| F8 | MultiDiva2 window: connect, host, room, chat |
| Ctrl+F8 | extra options (countdown sound, work-in-progress screens) |
| F9 | hide everything MultiDiva2 shows |

- **Joining:** paste the address your friend sends you (`wss://...`) into *Server* and click *Connect*.
- **Hosting:** click *Host a server* and send your friends the invite address (*Copy invite*).
- Everyone clicks *Ready* and waits on the song select screen. The host starts any song and it starts for everyone.
  Pick songs everyone owns (same DLC, same song mods).

More on hosting, including a permanent address of your own: [docs/hosting.md](docs/hosting.md).

Settings are kept in `%LOCALAPPDATA%\MultiDiva2`, so updates don't reset them.

## For later

Things I'd like to add (no promises on when):

- Song preview in the room: hear the song the host is looking at, like osu! (stuck for now: the game's own song files are encrypted, only mod songs' are plain)
- Full-screen multiplayer screens drawn with the game's own art (already in the works, press Ctrl+F8 to try them, the option is called "Full-screen F8 screens (work in progress)")
- Results screen and round banners in the game's style
- Restyled leaderboard cards
- A Multiplayer entry in the main menu (edit: if that's even possible lol)
- Room list scrolling for busy servers
- Team mode (2v2, summed scores, 2 players on the right, 2 on the left, maybe a tug-of-war with a crown just like osu!)
- Song select search bar and F2 for a random song (other features for osu! players like me)

## Building

- Client (the mod DLL): Visual Studio 2022 with C++, then
  `cmake --preset x64-release && cmake --build --preset x64-release`.
  Set `DIVA_DIR` to the game folder to have it copied into `mods/MultiDiva`.
- Server: [Go](https://go.dev), then `cd server && go build -o bin/multidiva-server.exe .` (tests: `go test ./...`).

Protocol: [docs/protocol.md](docs/protocol.md). Game internals the mod relies on: [docs/re-notes.md](docs/re-notes.md).

## Credits

- **[MultiDiva](https://github.com/zvandermeer/MultiDiva)** by Zoey VanderMeer (zvandermeer), a 2022 proof of concept
  for multiplayer in Mega Mix+. This project started from it and kept its name, but the code is written from scratch.
- [DivaModLoader](https://github.com/blueskythlikesclouds/DivaModLoader) by Skyth, which the mod runs on.
  `client/src/core/helpers.h` is its helper header (MIT licence, notice in the file).
- File formats and game structures were figured out with the help of public research projects:
  [MikuMikuLibrary](https://github.com/blueskythlikesclouds/MikuMikuLibrary) and
  [ReDIVA](https://github.com/korenkonder/ReDIVA). No code from them is included, apart from the colour conversion for
  the game's YCbCr textures (the constants from MikuMikuLibrary's texture decoder, MIT licence), used by the shader
  that draws song jackets.
- **A correction about versions 2.1.0 and 2.1.1:** they still contained pieces carried over from the original
  MultiDiva without my knowing: a few game addresses, hook signatures and a score struct, which it had in turn taken
  from [ShareDiva](https://github.com/RocketRacer/MegaMixPlusShareDivaMod) by RocketRacer and from research by
  Braasileiro. They weren't credited here, which they should have been. Since 2.1.2 all of it is removed and
  replaced with things found with this project's own tools (how each one was found is in
  [docs/re-notes.md](docs/re-notes.md)). Apart from the DivaModLoader header above, there's no code from any other
  mod.
- Libraries: [Dear ImGui](https://github.com/ocornut/imgui), [Microsoft Detours](https://github.com/microsoft/Detours),
  [nlohmann/json](https://github.com/nlohmann/json), [stb](https://github.com/nothings/stb),
  [coder/websocket](https://github.com/coder/websocket), [cloudflared](https://github.com/cloudflare/cloudflared)
  (downloaded when you host).
- The mod ships no game files. Everything it shows from the game (sprites, fonts, jackets) is read from the running game.

## About AI

I want to be upfront about this: MultiDiva2 was made with a LOT of AI assistance. Most of the code was written by Claude. I rarely added code in this project, I mostly planned what to build and how it should look and feel, testing every feature in the game (often with my girlfriend or with the bot), and going back and forth until it worked the way I wanted.

I'm an osu! player, and I made this so my girlfriend and I could play Project DIVA together the way osu! lobbies work.
Finding the game's addresses and figuring out its file formats by hand would have taken me months of digging. I have immense respect for the modders who have been paving the way in this community for years, all the people who have been doing this work by hand before AI was ever a thing, and not just on Project Diva but any other game.

And to be absolutely clear, I'm strictly against AI-generated art. Any creative expression that a human should be making has no business being outsourced to AI. But in my opinion there is a massive difference between generating slop and using AI strictly as a technical tool to accelerate the exhausting, time-consuming parts of reverse engineering.

I don't copy other people's stuff, with AI or without. If you see something here that comes from your work without credit, tell me and I'll fix it.

If you are strictly against any form of AI-assisted programming that's completely fair and I respect your stance, please feel free to steer clear of this mod. I made this out of love for the game and to play with my girlfriend. It’s here for anyone who wants to enjoy it. If it's not for you then no hard feelings, just skip it and don't harass me or anyone willing to use it.

## Licence

GPL-3.0. See [LICENCE](LICENCE). Bundled libraries keep their own licences.
