# Changelog

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
