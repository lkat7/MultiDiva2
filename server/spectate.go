package main

import (
	"bytes"
	"context"
	_ "embed"
	"encoding/json"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

// The spectator page: GET / is a small web page that listens on the WebSocket /spectate-ws (the server pushes a
// snapshot whenever something changed; GET /api/rooms returns the same snapshot once) and shows every room, its state and,
// while a round runs, the live leaderboard. Read-only: it exposes what the in-game room list and leaderboard show
// (room, player names, song, scores), never chat. Turn it off with -spectate=false.

//go:embed web/index.html
var spectatePage []byte

type SpectatePlayer struct {
	ID         int     `json:"id"`
	Name       string  `json:"name"`
	Host       bool    `json:"host,omitempty"`
	Ready      bool    `json:"ready,omitempty"`
	Away       bool    `json:"away,omitempty"`
	Avatar     bool    `json:"avatar,omitempty"`    // custom avatar at /avatar/<id>
	AvatarURL  string  `json:"avatarUrl,omitempty"` // the player's Steam picture (an https address on Steam's image hosts)
	Playing    bool    `json:"playing,omitempty"`
	Score      int     `json:"score"`
	Combo      int     `json:"combo"`
	Completion float64 `json:"completion"`
	Finished   bool    `json:"finished,omitempty"`
	Failed     bool    `json:"failed,omitempty"`
	Wins       int     `json:"wins,omitempty"`
	Lag        bool    `json:"lag,omitempty"`     // playing, but no score update for a while (or connection lost)
	Loading    bool    `json:"loading,omitempty"` // still on the loading screen
}

type SpectateRoom struct {
	Name    string           `json:"name"`
	State   string           `json:"state"`
	Pick    *PickInfo        `json:"pick,omitempty"`  // the song being played, or picked for the next round
	Hover   string           `json:"hover,omitempty"` // lobby: the song the host is looking at
	Mode    string           `json:"mode,omitempty"`  // "accuracy" when the room ranks by accuracy
	Rounds  int              `json:"rounds,omitempty"`
	Players []SpectatePlayer `json:"players"`
	Last    *SpectateLast    `json:"last,omitempty"` // the previous round
}

type SpectateLast struct {
	Pick    *PickInfo     `json:"pick,omitempty"`
	Results []FinalResult `json:"results"`
}

type SpectateMsg struct {
	Server string         `json:"server"`
	Rooms  []SpectateRoom `json:"rooms"`
}

// Spectate describes the room for the spectator page (false if it is closed or empty).
func (r *Room) Spectate() (SpectateRoom, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed || len(r.members) == 0 {
		return SpectateRoom{}, false
	}
	out := SpectateRoom{Name: r.name, State: r.state, Rounds: r.rounds, Mode: modeIfAccuracy(r.mode), Players: []SpectatePlayer{}}
	if r.pick != nil {
		out.Pick = &PickInfo{PV: r.pick.PV, Difficulty: r.pick.Difficulty, Edition: r.pick.Edition, Title: r.pick.Title}
	}
	if r.state == StateLobby && r.hover >= 0 {
		out.Hover = r.hoverTitle
	}
	live := map[int]LiveScore{}
	if r.state == StatePlaying {
		for _, ls := range r.liveScores().Players {
			live[ls.ID] = ls
		}
	}
	for _, m := range r.members {
		ls := live[m.c.id]
		out.Players = append(out.Players, SpectatePlayer{ID: m.c.id, Name: m.c.name, Host: m.c.id == r.host, Ready: m.ready,
			Away: m.away, Avatar: m.c.hasAvatar, Playing: m.playing, Score: m.live.Score, Combo: m.live.Combo,
			Completion: m.live.Completion, Finished: m.finished, Failed: m.result != nil && m.result.Failed,
			Wins: r.wins[m.c.name], Lag: ls.Lag, Loading: ls.Loading, AvatarURL: r.hub.steamPics.URL(m.c.steam)})
	}
	// Playing players by score, the others after them in join order.
	sort.SliceStable(out.Players, func(i, j int) bool {
		a, b := out.Players[i], out.Players[j]
		if a.Playing != b.Playing {
			return a.Playing
		}
		return a.Playing && r.ranksFirst(Score{Score: a.Score, Completion: a.Completion}, Score{Score: b.Score, Completion: b.Completion})
	})
	if r.lastResults != nil {
		out.Last = &SpectateLast{Pick: r.lastPick, Results: r.lastResults}
	}
	return out, true
}

func (h *Hub) spectateList() SpectateMsg {
	h.mu.Lock()
	rooms := make([]*Room, 0, len(h.rooms))
	for _, r := range h.rooms {
		rooms = append(rooms, r)
	}
	h.mu.Unlock()
	msg := SpectateMsg{Server: ServerVersion, Rooms: []SpectateRoom{}}
	for _, r := range rooms {
		if s, ok := r.Spectate(); ok {
			msg.Rooms = append(msg.Rooms, s)
		}
	}
	sort.Slice(msg.Rooms, func(i, j int) bool { return strings.ToLower(msg.Rooms[i].Name) < strings.ToLower(msg.Rooms[j].Name) })
	return msg
}

func (h *Hub) ServeSpectateAPI(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	json.NewEncoder(w).Encode(h.spectateList())
}

func serveSpectatePage(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	w.Write(spectatePage)
}

// spectators fans one snapshot stream out to every open spectator page. A single goroutine samples the rooms every
// tick while at least one page is connected and sends only when the snapshot changed, so an idle server costs nothing
// and many pages cost one snapshot per tick.
type spectators struct {
	mu      sync.Mutex
	conns   map[*spectator]struct{}
	last    []byte
	running bool
}

type spectator struct{ send chan []byte } // holds at most the latest snapshot

func (sp *spectator) push(msg []byte) {
	for {
		select {
		case sp.send <- msg:
			return
		default:
			select { // full: drop the stale snapshot, keep the new one
			case <-sp.send:
			default:
			}
		}
	}
}

func (h *Hub) addSpectator(sp *spectator) {
	h.spectators.mu.Lock()
	defer h.spectators.mu.Unlock()
	if h.spectators.conns == nil {
		h.spectators.conns = map[*spectator]struct{}{}
	}
	h.spectators.conns[sp] = struct{}{}
	if h.spectators.last != nil {
		sp.push(h.spectators.last)
	}
	if !h.spectators.running {
		h.spectators.running = true
		go h.spectateLoop()
	}
}

func (h *Hub) removeSpectator(sp *spectator) {
	h.spectators.mu.Lock()
	delete(h.spectators.conns, sp)
	h.spectators.mu.Unlock()
}

func (h *Hub) spectateLoop() {
	every := h.tickEvery
	if every < 50*time.Millisecond {
		every = 50 * time.Millisecond
	}
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		msg, _ := json.Marshal(h.spectateList())
		h.spectators.mu.Lock()
		if len(h.spectators.conns) == 0 {
			h.spectators.running, h.spectators.last = false, nil // nobody watching: the next page gets a fresh one
			h.spectators.mu.Unlock()
			return
		}
		if !bytes.Equal(msg, h.spectators.last) {
			h.spectators.last = msg
			for sp := range h.spectators.conns {
				sp.push(msg)
			}
		}
		h.spectators.mu.Unlock()
		<-t.C
	}
}

func (h *Hub) ServeSpectateWS(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, nil) // same-origin pages only
	if err != nil {
		return
	}
	defer conn.CloseNow()
	ctx := conn.CloseRead(r.Context()) // the page never sends; this notices it leaving
	sp := &spectator{send: make(chan []byte, 1)}
	h.addSpectator(sp)
	defer h.removeSpectator(sp)
	keepalive := time.NewTicker(25 * time.Second) // proxies close idle sockets
	defer keepalive.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case msg := <-sp.send:
			wctx, cancel := context.WithTimeout(ctx, 5*time.Second)
			err := conn.Write(wctx, websocket.MessageText, msg)
			cancel()
			if err != nil {
				return
			}
		case <-keepalive.C:
			pctx, cancel := context.WithTimeout(ctx, 5*time.Second)
			err := conn.Ping(pctx)
			cancel()
			if err != nil {
				return
			}
		}
	}
}

func modeIfAccuracy(mode string) string {
	if mode == ModeAccuracy {
		return ModeAccuracy
	}
	return ""
}
