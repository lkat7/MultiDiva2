# Hosting a MultiDiva server

One person runs the server; everyone (including them) connects to it from the F8 window.

## 1. Run the server

Build it once (needs [Go](https://go.dev/dl)):

```
cd server
go build -o bin/multidiva-server.exe .
```

Run it:

```
bin\multidiva-server.exe -addr 127.0.0.1:3939
```

`127.0.0.1` keeps it private to your PC (no firewall prompt). Players on the same PC connect to
`127.0.0.1`, port `3939`.

## 2. Let friends reach it

Pick one.

### A. Cloudflare quick tunnel (easiest, no router setup)

1. Download `cloudflared` from https://github.com/cloudflare/cloudflared/releases (the
   `cloudflared-windows-amd64.exe` file).
2. With the server running:
   ```
   cloudflared-windows-amd64.exe tunnel --url http://127.0.0.1:3939
   ```
3. It prints an address like `https://random-words.trycloudflare.com`. Everyone enters it in the
   **Server** field as `wss://random-words.trycloudflare.com` (note `wss://`, no port needed).

The address changes every time you restart the tunnel, and the tunnel does not survive your PC
sleeping (restart it; the new name can take ~30 s to resolve). Anyone who has it can join, so share it
only with the people you play with.

### B. Port forwarding

Run the server with `-addr :3939`, allow it through Windows Firewall, and forward TCP port 3939 on
your router to your PC. Friends enter your public IP and port 3939.

### C. A VPN like Tailscale

Everyone installs Tailscale and joins your network; friends use your Tailscale IP and port 3939.
Run the server with `-addr :3939`.

## Spectator page

The server also serves a read-only web page: open the server's address in a browser (`http://localhost:3939/`, or
the public address you host on) to see every room, its players and the host's song, and a live leaderboard (score,
combo, completion) while a round plays. It shows the same things the in-game room list and leaderboard show, never
chat. The page updates live over a WebSocket (`/spectate-ws`); `GET /api/rooms` returns the same data as JSON once. Turn it off with `-spectate=false`.

## Testing alone

`server/cmd/bot` is a fake player that joins a room, readies up and plays along:

```
go build -o bin/multidiva-bot.exe ./cmd/bot
bin\multidiva-bot.exe -room miku
```

Join the room yourself first so you are the host.
