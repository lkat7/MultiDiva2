// MultiDiva server: rooms, chat, host song pick and live scores for the MultiDiva game mod.
// Protocol: docs/protocol.md.
package main

import (
	"flag"
	"log"
	"net/http"
	"time"
)

func main() {
	addr := flag.String("addr", ":3939", "listen address")
	tick := flag.Duration("tick", 100*time.Millisecond, "live score broadcast interval")
	loadTimeout := flag.Duration("load-timeout", 30*time.Second, "max wait on the loading screen for slow players")
	playLog := flag.String("play-log", "multidiva-plays.txt", "append every finished round and its ranking to this text file (empty: off)")
	spectate := flag.Bool("spectate", true, "serve the spectator web page (rooms and live leaderboard) at / (live over a WebSocket, /spectate-ws; JSON at /api/rooms)")
	flag.Parse()

	hub := NewHub(*tick, *loadTimeout)
	hub.plays = newPlayLog(*playLog)

	log.Printf("MultiDiva server %s (protocol v%d) listening on %s", ServerVersion, ProtocolVersion, *addr)
	log.Fatal(http.ListenAndServe(*addr, httpHandler(hub, *spectate)))
}

func httpHandler(hub *Hub, spectate bool) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", hub.ServeWS)
	mux.HandleFunc("/avatar/", hub.ServeAvatar)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) { w.Write([]byte("ok")) })
	if spectate {
		mux.HandleFunc("/api/rooms", hub.ServeSpectateAPI)
		mux.HandleFunc("/spectate-ws", hub.ServeSpectateWS)
		mux.HandleFunc("/", serveSpectatePage)
	}
	return mux
}
