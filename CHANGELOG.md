# Changelog

## 2.4.5 (02.11.2026)
- Research: "Watch highlight write" (Ctrl+F8, `research=1`) logs every write to the song list's highlighted song, with
  the code around it and the call chain, to find the function that moves song select's cursor. The write watch also
  logs the 64 bytes after each write now.

## 2.4.4 (02.11.2026)
- Add/Remove Miku Bot moved into the options section of the F8 window; the cover there is half the size.
- Covers of mod songs: a song whose jacket sprite isn't named like the game's own now falls back to any `SONG_JK...`
  sprite in its set, and `multidiva.log` says why when a cover can't be shown (`jackets:` lines).

## 2.4.3 (02.11.2026)
- The F8 window shows the cover (the game's own jacket) of the host's highlighted song, and of the last song played,
  next to its lines. The full-screen room already did.

## 2.4.2 (02.11.2026)
- Charts a song doesn't really have are no longer listed. The game keeps an empty placeholder in those slots (level 0,
  greyed out in song select) and the song check counted it as present, so Easy showed up for songs without one.

## 2.4.1 (02.11.2026)
- In the chart row only the host's selected chart keeps its colour, the others are grey (all stay coloured while the
  selection is unknown, e.g. with an older host). The F8 window has a divider between the room status and the options.

## 2.4.0 (02.11.2026)
- While the host browses song select, everyone now sees which chart the host has selected with left/right: the chart row
  marks it with a tinted tab (brackets in the F8 window) and the plate's difficulty chip follows it. The host's client
  reads it from the game's stage data and sends it with the highlighted song (`hover` gains optional `difficulty` and
  `edition`; older clients and servers keep working and simply don't show it).

## 2.3.11 (02.11.2026)
- Fixes the crash of the song select watch in 2.3.9 and 2.3.10: the new 0x40 byte stage data scan copied 0x100 bytes
  into its 0x40 byte buffer (a buffer overrun in my code). The earlier guess about the selection record in 2.3.10 was
  wrong; that scan stays at 0x600 bytes.

## 2.3.10 (02.11.2026)
- Research: the song select watch crashed the game in 2.3.9. The selection record scan is back to the 0x600 bytes
  that worked before (2.3.8 had widened it to 0x2000, the likeliest cause), and nothing else changed in the watch.

## 2.3.9 (02.11.2026)
- Research: the difficulty hunt found stage data `+0x0` equal to the difficulty at every confirm. The song select watch
  now logs changes to the start of the stage data, to check it follows left/right live.

## 2.3.8 (02.11.2026)
- Research: "Watch difficulty write" now also runs a hunt for the field that holds the selected difficulty: start
  several rounds on different difficulties in any order and the `diffhunt` lines in `multidiva.log` list the fields that
  matched every time. The song select watch scans the selection record out to 0x2000 bytes.

## 2.3.7 (02.11.2026)
- Research: "Watch difficulty write" (Ctrl+F8, `research=1`) logs the registers and the code around each write to the
  stage entry's difficulty, to find where song select keeps the selected difficulty. The write watch logs this for every
  watch. The tag experiment of 2.3.6 is removed (the tag was not the difficulty).

## 2.3.6 (02.11.2026)
- Research: song select's left/right tag (`p_02`..`p_04`, probably the selected difficulty) is logged when it changes
  and next to the difficulty of every round started, to map one to the other.

## 2.3.5 (02.11.2026)
- Research: the input watch names the buttons in each change, and the song select watch lists which charts each
  highlighted song has. The 2.3.3 log showed the arrow keys only move the song list, so the key that changes the
  difficulty still has to be captured.

## 2.3.4 (02.11.2026)
- Fixes a build error in 2.3.3 on MSVC (`near` is a macro in `windows.h`).

## 2.3.3 (02.11.2026)
- Research: the song select watch (Ctrl+F8, `research=1`) now also logs every change next to the highlighted song's
  field and in the selection record, and turns the button log on with it, so one run (press left/right/up/down a few
  times) shows where the game keeps the selected difficulty. Nothing changes for players.

## 2.3.2 (02.11.2026)
- Level and BPM are confirmed to read correctly on Mega Mix+ (chart `+0x40`, in half stars; song entry `+0x4C`). A
  level of 0.5 stars is now accepted, and every rejected level is logged.
- Research (Ctrl+F8, `research=1`): "Watch song select for the difficulty" logs which values change when you change
  difficulty in song select, to find where the game keeps the host's selected difficulty so everyone can see it.

## 2.3.1 (02.11.2026)
- While the host is only highlighting a song, the room now lists the charts that song has (Easy, Normal, Hard, Extreme,
  Extra), each in its difficulty's colour, with the level in stars and the BPM when the game's database gives them. A
  highlight has no single difficulty, so this shows all of them; the one being played shows once the round is picked.
- The first level and BPM lookups are written to `multidiva.log` (`songs: level of pv ...`) with the raw value read, so
  a log shows whether the layout taken from the Arcade game holds on your game version.

## 2.3.0 (01.11.2026)

### Added
- The picked song shows its difficulty in a colour (blue, green, orange, red, purple), with its level in stars and its
  BPM when the game's song database gives them. These two read the game's memory using layouts taken from the Arcade
  game's decompilation, so they appear only if the value looks sane; if they don't show on your game version, run the
  research buttons below and send the log.
- The live leaderboard stays on screen after you fail or quit a song while the others are still playing.
- A profile picture from a file: put `avatar.png` (or `.jpg`) next to `MultiDiva.dll` (or in
  `%LOCALAPPDATA%\MultiDiva2`). It is shrunk to fit and shown instead of your Steam picture, so players without Steam
  get one too.
- A message when a non-host starts a song on their own: it plays solo and isn't part of the room's round.
- Server: every finished round is appended to `multidiva-plays.txt` (song, difficulty, ranking with scores, grades and
  judgements, failed players marked). `-play-log <file>` moves it, `-play-log ""` turns it off.

### Research (Ctrl+F8 with `research=1` in the ini)
Buttons that log what is needed to finish the next features, because the layouts aren't known on Mega Mix+: the song
entry and chart structs (level, BPM, credits), which button check handles song select's confirm button (to stop
non-hosts starting alone), the input state bytes (a held-button indicator on the leaderboard), and where the in-game
profile name lives (to default your player name to it).

## 2.2.0 (29.10.2026)

### Fixed
- Players on an unstable connection (mobile data) no longer end up as a ghost entry plus a `Name (2)` copy of themselves,
  and a host who drops no longer leaves the room without a working host until the room is recreated or the server
  restarted.

### Added
- Reconnecting: a connection that drops inside a room is retried automatically for about a minute. If it comes back in
  time you keep your place: same player id, score and round, even mid-song. The host role passes to another player
  while you're gone and isn't handed back. If the server no longer holds your place, you rejoin the room as a new
  player.
- Players whose connection dropped show as "Reconnecting..." in the player list (greyed bar in the full-screen room),
  and your own window says so while the mod tries to get you back.
- The client shows its own version and the server's in the multiplayer window ("Client 2.2.0   Server 2.2.0"), so a
  version mismatch is easy to spot. An older server that doesn't report one shows as "unknown (older)".

### Server
- Connections are pinged every 10 s and dropped if the peer doesn't answer, so a vanished client is noticed within
  about 20 s instead of whenever the operating system gives up.
- A connection that ends without a close frame keeps the player's room slot for 60 s (shown as `away`), instead of
  removing the player at once. Leaving on purpose still frees the slot immediately. Only clients that say they can
  resume (`hello.resumable`) get this; older clients are still removed at once, so they never stall a round.
- `welcome` carries a session `token` and the server release (`server`); `hello` accepts `resume`. These are
  additive: the protocol version stays 1, and older clients and servers keep working (older clients just can't resume).
- A rejoin without a token (older client) replaces a dropped entry of the same name instead of becoming `Name (2)`.
- Details in [docs/protocol.md](docs/protocol.md).

## 2.1.2 (23.10.2026)
- Removed every piece carried over from the original MultiDiva and ShareDiva (game addresses, hook signatures, score
  struct) and replaced them with things found with this project's own tools; see [docs/re-notes.md](docs/re-notes.md)
  and the correction in the README.
- Crash logging, settings kept in `%LOCALAPPDATA%\MultiDiva2` so updates don't reset them, and the work-in-progress
  screens moved to Ctrl+F8.
