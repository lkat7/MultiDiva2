# Changelog

## 2.2.1-dev.4 (03.11.2026)
- **Fixes the host stuck alone on the loading screen.** When the host's game launched a song while the connection was
  down or very slow, the `start` message was lost but the loading hold was armed anyway, and nothing ever released it
  (the release only ran for players the server had put in the round). Now: a launch while reconnecting plays solo at
  once; a start the server doesn't confirm within 6 s releases the hold and plays solo; and a room update that says
  the round is on counts as the confirmation if the echo was lost. A start that arrives after the host gave up is
  counted as the host's play, so the room isn't left waiting for them.
- **Adaptive live-score rate.** Scores go out at 10 Hz on a healthy link. When the socket can't keep up (messages pile
  up in its outbox) the rate drops to 5 Hz and then 3 Hz, and climbs back after 4 calm seconds; no new score is queued
  behind unsent ones. The log says `slow link ... live scores now N Hz`.
- **Win by accuracy.** The host can set the room to rank by accuracy (the completion %) instead of score: a "Win by"
  choice in the F8 options and in the full-screen settings (host, in the lobby). The server then orders the live
  leaderboard, the results, the session wins and the play log by accuracy (score breaks a tie); the spectator page
  shows the percentage as the big number. On the in-game leaderboard cards the score moves to the first line (dimmer)
  and the second line shows accuracy: your own %, and with Score gaps on the others' gap to you in points (+1.39%).
  New `mode` message and `mode` on the room (`docs/protocol.md`).
- **Only the host starts, and only when everyone is ready.** In a room, song select's confirm button (the one that
  starts a song, `0x140669B42`) is refused for everyone but the host, and for the host while a player isn't ready (a
  player who lacks the song doesn't count). A notice is drawn over the game (it works with a controller, where the F8
  window isn't on screen) and the game's "no" sound plays. The host presses confirm again within 5 seconds to start
  anyway: the `start` message carries `force` and the server skips the ready check. Outside a room nothing is blocked.
- **Ready follows song select:** a player who is ready and leaves song select for more than 1.5 s is unreadied
  automatically (with a notice over the game), so the host isn't waiting on someone in a menu. Readying and unreadying
  (by anyone in the room, not when a round starts or ends) plays the game's confirm / cancel sound.
- **Host change:** when the host role passes to someone else, everyone in the lobby hears the game's item sound
  (`se_ft_custom_item_01`) and sees "X is now the host" over the game. (The game has no named hit sounds to use; the
  sound is one constant in `multiplayer.cpp` if you want another.)
- **Steam pictures on the spectator page.** The page showed only custom avatars (`avatar.png`), because the Steam ones
  are fetched by each player's game. The server now looks a Steam player's picture up once from Steam's public profile
  feed (background lookup, cached for 12 h, a failed lookup is retried after 10 min) and gives the page only the
  picture's address (an https address on Steam's own image hosts), never the Steam ID. A custom avatar still wins.

## 2.2.1-dev.3 (02.11.2026)
- Fixes the crash when opening the full-screen multiplayer screens: they drew text with a font that was never set up
  (`sceneButton`, a null pointer) whenever the game's own font sheet wasn't available, which the logs show was always
  the case. The screens now fall back to the overlay font and log `screens: the game's font isn't available yet`.
- Your own leaderboard card is 5% wider than the others (was 15%), and the BPM now follows the song title in the F8
  window.

## 2.2.1-dev.2 (02.11.2026)
- **Session scoreboard:** the server counts the rounds each player wins in a room (rounds with two or more players, won
  by someone who didn't fail). The F8 player list and the full-screen room show "2 wins" next to the player, and the
  spectator page shows it too. It's kept while the room exists, by player name.
- **3-2-1 countdown:** once everyone has loaded, the server sends 3, 2, 1 half a second apart (1.5 s in all), the
  number is drawn large and sharp (its own digit font, baked at the drawn size) over the loading screen, and `go` follows. The **Countdown sound** setting (on by default, in
  Ctrl+F8) plays the game's sounds: the song cursor sound on 3 and 2, the enter sound on 1. Older clients ignore
  the countdown and start 1.5 s later.
- **Live leaderboard:** narrower (20% less width, so it covers less of the notes), (the accuracy % stays). A player whose score updates stopped for more than 2 s (slow game, stall, lost connection) shows
  a **LAG** chip and a dimmed card, and a player still on the loading screen shows **loading** (the board is now also
  shown while waiting for the others). With **Score gaps** on (now the default for new installs) the other players show
  how far ahead (+, red) or behind (-, teal) they are of you instead of their whole score; your card keeps your score, is 5% wider and brighter, and has a teal outline.
  If *our* scoreboard messages stop for 2 s, "no updates from the server" shows
  below the board.
- **Chart chips** (the BPM now follows the song title): in the F8 window the host's highlighted song lists its charts as rounded chips in the difficulty
  colours, muted. The chart the host selected is filled and outlined, the others are faint grey (all softly tinted
  while the selection is unknown).
- **Browse sounds:** when the host moves the song cursor or changes the difficulty in song select, guests in the lobby
  hear the game's own sounds for it (`se_ft_music_selector_select_01` and `..._difficulty_change_01`). Option
  **Browse sounds** (on by default) in the F8 options and the full-screen settings.
- Server: `scores` gains `loading` and `lag`, players gain `wins`, the room gains `rounds`, and there is a new
  `countdown` message (`docs/protocol.md`). The spectator page shows wins, "lagging" and "loading".

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
