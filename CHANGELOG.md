# Changelog

## 2.2.1-dev.1 (02.11.2026)
- Server: a spectator web page at the server's address (`/`): every room with its state, players and song, a live
  leaderboard while a round plays (score, combo, completion bar, finished/failed) and the previous round's ranking.
  The page listens on a WebSocket (`/spectate-ws`): the server pushes a snapshot only when something changed, and
  stops sampling when no page is open. Read-only, no chat. `GET /api/rooms` gives the same data as JSON once;
  `-spectate=false` turns it all off. See `docs/hosting.md`.

## 2.2.1 (02.11.2026)

### Added
- **Difficulty with level and colour.** The picked song shows its difficulty (Easy blue, Normal green, Hard orange,
  Extreme red, Extra purple) with its level in stars and its BPM, read from the game's song database. They appear only
  if the value looks sane.
- **Live chart of the host.** While the host browses song select, everyone sees the host's highlighted song with its
  cover (the game's own jacket, also for mod songs) and the charts it has, each with its level. The chart the host has
  selected with left/right keeps its colour (a tinted tab in the full-screen room, brackets in the F8 window), the others
  are grey, and the plate's difficulty chip follows it. Charts a song doesn't really have (the game's empty placeholder
  slots) are not listed. Older clients and servers keep working and simply don't show it (`hover` gains optional
  `difficulty` and `edition`).
- The live leaderboard stays on screen after you fail or quit a song while the others are still playing.
- **Profile picture from a file:** put `avatar.png` (or `.jpg`) next to `MultiDiva.dll` (or in
  `%LOCALAPPDATA%\MultiDiva2`). It is shrunk to fit and shown instead of your Steam picture, so players without Steam get
  one too.
- A message when a non-host starts a song on their own: it plays solo and isn't part of the room's round.
- F8 window: Add/Remove Miku Bot moved into the options section, a divider separates the room status from the options,
  the highlighted song's title is white and its BPM greyer, and "Last song" is one line ("Title (Hard)").
- Covers of mod songs whose jacket sprite isn't named like the game's own fall back to any `SONG_JK...` sprite in the
  set, and `multidiva.log` says why when a cover can't be shown (`jackets:` lines).

### Server
- Every finished round is appended to `multidiva-plays.txt` (song, difficulty, ranking with scores, grades and
  judgements, failed players marked). `-play-log <file>` moves it, `-play-log ""` turns it off.

### Research (Ctrl+F8 with `research=1`)
- Findings from reverse engineering the game's song select and preview audio are kept in `docs/re-notes.md`. The
  unfinished research tools were removed again.

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
