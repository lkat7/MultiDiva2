# MultiDiva protocol (v1)

WebSocket at `ws://<host>:3939/ws`, one JSON text message per frame:

```json
{ "type": "chat", "data": { "text": "hi" } }
```

Times are milliseconds. Unknown message types are ignored, so either side can add types later.

## Round start in practice

The host starts a song in their own game as usual. Their client captures the complete launch block
(song, chart, performers, outfits) at launch and sends it as `start` data. Everyone else auto-starts
on the `start` broadcast; the host's client ignores it because it is already playing.

Every player's game (host included) then holds on NOW LOADING once the song is loaded and sends
`loaded`. When all players in the round have loaded, the server sends `go` and everyone starts at
the same moment. Players who leave or quit while loading don't block the others, and after 90 s
the server sends `go` anyway.

## Room lifecycle

```
lobby --(host start, everyone ready, song picked)--> playing --(everyone finished/left)--> lobby
```

- The first player in a room is the host. If the host leaves, the player who joined next becomes host.
- A new pick clears everyone's ready flag.
- While `playing`, players who join wait in the lobby state until the round ends.

## Client -> server

| type | data | notes |
|---|---|---|
| `hello` | `{name, version, steam?, avatar?, resume?, resumable?}` | first message; server answers `welcome`. `steam`: Steam ID (decimal) for the Steam avatar. `avatar`: base64 PNG/JPEG (max 32 KB, 256x256) for players without one. `resume`: the `token` of a connection that dropped; if the server still holds that player's room slot, this connection takes it over (see below) |
| `join` | `{room}` | joins or creates the room (name 1-32 chars) |
| `leave` | `{}` | back to no room |
| `chat` | `{text}` | max 300 chars |
| `ready` | `{ready}` | lobby only |
| `skip` | `{}` | host only, while the round waits on the loading screen (after the host has loaded itself): the players still loading sit this round out and the countdown starts. Refused if nobody is being waited for |
| `mode` | `{mode}` | host only, lobby only: how the room ranks players, `"score"` (default) or `"accuracy"` (the completion percentage; the score breaks a tie). It changes the order of the live `scores`, of the `results` and of who gets a round win |
| `hover` | `{pv, title, difficulty?, edition?}` | host only, lobby only: the song highlighted in song select (`-1` = none), its title and the chart the host has selected for it (`difficulty` 0..3, `edition` 1 = the Extra chart); both optional, older clients send neither |
| `have` | `{pv, ok}` | whether this player has song `pv` (the room's `focus`); players without it sit the round out |
| `host` | `{id}` | host only: makes that player the host |
| `kick` | `{id}` | host only: removes that player (they get `kicked`; not a ban, they may join again). Counts as leaving during a round |
| `pick` | `{block, pv, difficulty, edition, title}` | host only, lobby only. `block` = base64 of the 276-byte launch block. Optional preview, clears ready flags |
| `start` | `{}` or pick data | host only. Needs a pick (or pick data in this message) and every player ready (host counts as ready). Sending pick data here does not clear ready flags. `force: true` skips the ready check (the host started anyway); the unready players get the song like the rest |
| `title` | `{title}` | host only: the current pick's song title, sent once the host's game shows it |
| `loaded` | `{}` | this player's song finished loading and is held on NOW LOADING |
| `score` | `{score, combo, completion, cool, fine, safe, sad, worst, started?}` | while playing, ~10 Hz. Live values. `started` (optional): the song is really playing (not still loading); the leaderboard shows a player who hasn't started 2 s after `go` as lagging |
| `finish` | `{result}` | song over. `result` = same fields as `score` plus `grade`, `maxCombo`, `failed` (bool) |

## Server -> client

| type | data | notes |
|---|---|---|
| `welcome` | `{id, version, token, server}` | your player id, the server release (`server`, e.g. `2.2.0`, shown in the client), and the session token to send as `hello.resume` after a dropped connection. After a resume the id is the old one |
| `room` | room state, see below | sent on every change |
| `chat` | `{from, name, text, system}` | `system` = server notices (joins, host changes) |
| `start` | `{block, pv, difficulty, edition, title}` | auto-start now |
| `countdown` | `{n}` | everyone has loaded (or the load timeout passed): `n` = 3, 2, 1, one step (0.5 s) apart, then `go`. Older clients ignore it and just start 1.5 s later |
| `go` | `{}` | the countdown is over: release the hold and play |
| `scores` | `{players: [{id, name, score, combo, completion, finished, failed?, loading?, lag?}]}` | ~10 Hz while playing, sorted by score. `loading`: still on the loading screen (before `go`). `lag`: after `go`, no live score for 2 s, or the song still isn't playing 2 s after `go` (`score.started`), or the connection is lost |
| `results` | `{players: [{id, name, result, place}]}` | end of round, sorted by score |
| `kicked` | `{room}` | the host removed you; you are in no room now |
| `rooms` | `{rooms: [{name, host, players, playing}]}` | the open rooms, sent to players connected but in no room (after `welcome`, after leaving or a kick) and again ~150 ms after any room change; sorted by name |
| `error` | `{message}` | refused request, connection stays open (e.g. a room is full: 8 players max) |
| `ping` | `{}` | keepalive every 10 s; clients drop the connection after 30 s of silence |

Room state:

```json
{
  "name": "miku", "state": "lobby", "host": 1,
  "pick": { "pv": 54, "difficulty": 3, "edition": 0, "title": "Roshin Yukai" },
  "players": [ { "id": 1, "name": "Lemm", "ready": true, "playing": false, "loaded": false } ]
}
```

`pick` is `null` until the host picks. `playing` = in the current round. `hover` / `hoverTitle`: the host's highlighted song (`-1` = none). `hoverDifficulty` / `hoverEdition`: the chart the host selected for it (`-1` = unknown). `focus`: the song players report on with `have` (the hovered one in the lobby, else the pick); players who reported not having it carry `missing: true`. Names are unique per room (case-insensitive): a repeat is shown as `Name (2)`, `Name (3)`. Players whose connection dropped carry `away: true` while their slot is held. `wins` on a player is the rounds they won in this room (a round with at least two players, won by a player who did not fail; kept by name while the room exists) and `rounds` on the room how many such rounds were played. `mode` on the room is `"accuracy"` when the room ranks by accuracy (absent = by score). Players may also carry `steam` (Steam ID) and `avatar: true` (custom avatar at `GET /avatar/<id>`, same host as `/ws`).

## Dropped connections and resuming

The server pings every connection at the WebSocket level (every 10 s, 10 s to answer); a peer that doesn't answer is dropped. A connection that ends without a close frame (network loss, killed app) doesn't remove a player whose `hello` said `resumable: true` at once: their room slot is held for 60 s as `away`, the host role passes to the next connected player, and a round goes on with their score and state kept. Sending `hello` with `resume` set to the `token` from their earlier `welcome` gives the new connection that slot (same `id`, name and round state); the server sends `welcome`, the current `room`, and `go` again if the loading barrier was released meanwhile. After the grace period, or if the player left with a close frame, the slot is freed as for a normal leave and `resume` is ignored (the `hello` is handled as a fresh one). A `join` without a token while an `away` player of the same name is in the room replaces that entry instead of getting a `Name (2)`. Clients that don't send `resumable` (older versions) are removed immediately when their connection ends, as before.
