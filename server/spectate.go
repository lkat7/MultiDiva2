package main

import (
	_ "embed"
	"encoding/json"
	"net/http"
	"sort"
	"strings"
)

// The spectator page: GET / is a small web page that polls GET /api/rooms and shows every room, its state and,
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
	Avatar     bool    `json:"avatar,omitempty"`
	Playing    bool    `json:"playing,omitempty"`
	Score      int     `json:"score"`
	Combo      int     `json:"combo"`
	Completion float64 `json:"completion"`
	Finished   bool    `json:"finished,omitempty"`
	Failed     bool    `json:"failed,omitempty"`
}

type SpectateRoom struct {
	Name    string           `json:"name"`
	State   string           `json:"state"`
	Pick    *PickInfo        `json:"pick,omitempty"`  // the song being played, or picked for the next round
	Hover   string           `json:"hover,omitempty"` // lobby: the song the host is looking at
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
	out := SpectateRoom{Name: r.name, State: r.state, Players: []SpectatePlayer{}}
	if r.pick != nil {
		out.Pick = &PickInfo{PV: r.pick.PV, Difficulty: r.pick.Difficulty, Edition: r.pick.Edition, Title: r.pick.Title}
	}
	if r.state == StateLobby && r.hover >= 0 {
		out.Hover = r.hoverTitle
	}
	for _, m := range r.members {
		out.Players = append(out.Players, SpectatePlayer{ID: m.c.id, Name: m.c.name, Host: m.c.id == r.host, Ready: m.ready,
			Away: m.away, Avatar: m.c.hasAvatar, Playing: m.playing, Score: m.live.Score, Combo: m.live.Combo,
			Completion: m.live.Completion, Finished: m.finished, Failed: m.result != nil && m.result.Failed})
	}
	// Playing players by score, the others after them in join order.
	sort.SliceStable(out.Players, func(i, j int) bool {
		a, b := out.Players[i], out.Players[j]
		if a.Playing != b.Playing {
			return a.Playing
		}
		return a.Playing && a.Score > b.Score
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
