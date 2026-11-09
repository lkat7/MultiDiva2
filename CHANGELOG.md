# Changelog

## 2.2.3-dev.1 (06.11.2026)
- **The wait for slow loaders is 90 s (was 30 s).** The server's `-load-timeout` default is now 90 s, so a slow
  computer has time to finish loading. The client's own "the server never answered" release moved from 45 s to 120 s so
  it doesn't fire first.
- **Start without slow loaders.** When the host has loaded and others are still loading, after 6 s the waiting card
  adds "X is taking time to load. Press Enter to start without them". Enter sends the new `skip` message: the server
  leaves out whoever is still loading (they sit this round out, the room says so in chat), and the 3-2-1 begins at
  once instead of waiting for the 90 s timeout. The left-out player sees "The host started without you: your song
  plays on its own". Enter works from the keyboard only for now (no controller button).
- **Late starters show as lagging.** A slow computer keeps sending zero scores from its loading screen, so the server saw
  it as alive and never showed LAG while its song started late. Clients now report whether the song is really playing
  (`score.started`), and a player who still isn't playing 2 s after `go` gets the LAG chip until it starts. Older
  clients are still judged by their updates only.
- **A sound when someone leaves the room** (also when kicked or timed out): the game's dialog-close sound. When the
  host leaves, the new-host sound plays instead.

## 2.2.2 (05.11.2026)

### Added
- **Spectator page.** The server serves a live web page at its own address: every room with its state, players (Steam
  pictures or custom avatars), the song, a live leaderboard while a round plays, the previous round's ranking, wins and
  LAG / loading marks. It updates over a WebSocket (`/spectate-ws`), never shows chat, and `GET /api/rooms` gives the
  same data as JSON. `-spectate=false` turns it off. The server looks Steam pictures up from Steam's public profile
  feed (cached) and gives the page only the picture's address, never the Steam ID. See `docs/hosting.md`.
- **Only the host starts, and only when everyone is ready.** In a room, song select's confirm button is refused for
  everyone but the host, and for the host while a player isn't ready (players who lack the song don't count), with a
  notice over the game and the game's "no" sound (it works with a controller). The host presses confirm again within
  5 seconds to start anyway. Outside a room nothing is blocked.
- **3-2-1 countdown.** Once everyone has loaded the server sends 3, 2, 1 half a second apart, drawn large and sharp
  over the loading screen, and then `go`. The **Countdown sound** setting (on by default, in Ctrl+F8) plays the game's
  cursor sound on 3 and 2 and its enter sound on 1.
- **Session wins.** The server counts the rounds each player wins in a room (two or more players, won by someone who
  didn't fail), shown as "2 wins" in the F8 list and the full-screen room.
- **Win by accuracy.** The host can rank the room by accuracy (the completion %) instead of score ("Win by" in the F8
  options and the full-screen settings). The live leaderboard, the results, the wins and the play log follow it.
- **Live leaderboard.** Narrower, with the accuracy % on each card. Your own card is 5% wider, brighter and outlined.
  With **Score gaps** (now on by default) the others show how far ahead (+) or behind (-) of you they are, in score or
  in accuracy points, instead of their whole score. A player whose updates stopped for 2 s shows a **LAG** chip and a
  dimmed card, one still on the loading screen shows **loading** (the board is shown while waiting for the others), and
  "no updates from the server" shows if the scoreboard messages stop reaching you.
- **Chart chips** in the F8 window: the host's highlighted song lists its charts as muted difficulty-coloured chips, the
  host's selected chart filled and outlined, with the BPM after the title.
- **Sounds.** Guests in the lobby hear the game's cursor and difficulty-change sounds when the host browses song select
  (**Browse sounds**, on by default), a confirm / cancel sound when someone readies or unreadies, and a sound and a
  notice over the game when the host changes.
- **Ready follows song select:** a ready player who leaves song select for more than 1.5 s is unreadied
  automatically, so the host isn't waiting on someone in a menu.

### Fixed
- **Host stuck alone on the loading screen.** When the host's game launched a song while the connection was down or
  very slow, the `start` message was lost but the loading hold was still armed and nothing released it. Now a launch
  while reconnecting plays solo at once, a start the server doesn't confirm within 6 s releases the hold, and a room
  update saying the round is on counts as the confirmation if the echo was lost. A start that arrives after the host
  gave up is counted as the host's play, so the room isn't left waiting.
- **Crash when opening the full-screen multiplayer screens:** they drew text with a font that was never set up (a null
  pointer) whenever the game's own font sheet wasn't available. They now fall back to the overlay font.
- **Live scores on a bad connection:** scores go out at 10 Hz on a healthy link; when the socket backs up the rate drops
  to 5 and then 3 per second and recovers after 4 calm seconds, and no score is queued behind unsent ones.

### Server
- New messages and fields (all additive, the protocol version stays 1): `countdown`, `mode`, `start.force`, `wins`,
  `rounds` and `mode` on the room, `loading` and `lag` on `scores`. Details in `docs/protocol.md`.

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
