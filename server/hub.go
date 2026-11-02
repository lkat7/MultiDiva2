package main

import (
	"context"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

// Hub owns the rooms and hands out player ids.
type Hub struct {
	avatars     map[int]Avatar
	mu          sync.Mutex
	rooms       map[string]*Room
	nextID      int
	tickEvery   time.Duration
	loadTimeout time.Duration
	lobby       lobby // connected players not in a room, who get the room list

	smu         sync.Mutex    // leaf lock: never held while taking another
	resumeGrace time.Duration // how long a dropped player's slot is held for a resume
	probeEvery  time.Duration // WebSocket ping interval, and how long a pong may take
	probeWait   time.Duration
	sessions    map[string]*Client

	spectators spectators // spectator pages listening on /spectate-ws

	lagAfter  time.Duration // how long a player's score updates may stay away before the leaderboard marks them as lagging
	countStep time.Duration // one tick of the 3-2-1 before `go` (the whole countdown is three of them)

	plays *PlayLog // finished rounds appended to a text file; nil = off
}

func NewHub(tickEvery, loadTimeout time.Duration) *Hub {
	return &Hub{rooms: map[string]*Room{}, avatars: map[int]Avatar{}, tickEvery: tickEvery, loadTimeout: loadTimeout,
		lobby: lobby{clients: map[*Client]struct{}{}}, resumeGrace: 60 * time.Second, countStep: DefaultCountStep, lagAfter: 2 * time.Second, probeEvery: 10 * time.Second, probeWait: 10 * time.Second, sessions: map[string]*Client{}}
}

func (h *Hub) grace() time.Duration {
	h.smu.Lock()
	defer h.smu.Unlock()
	return h.resumeGrace
}

func (h *Hub) probing() (every, wait time.Duration) {
	h.smu.Lock()
	defer h.smu.Unlock()
	return h.probeEvery, h.probeWait
}

func (h *Hub) setSession(c *Client) {
	h.smu.Lock()
	h.sessions[c.token] = c
	h.smu.Unlock()
}

// dropSession forgets c's token, unless a newer connection already took it over.
func (h *Hub) dropSession(c *Client) {
	h.smu.Lock()
	if h.sessions[c.token] == c {
		delete(h.sessions, c.token)
	}
	h.smu.Unlock()
}

// resume gives c the place of the dropped connection that was issued token: same id, name and room slot. It sends
// c its welcome and the room, and reports false (touching nothing) if there is nothing to resume.
func (h *Hub) resume(c *Client, token string) bool {
	h.smu.Lock()
	old := h.sessions[token]
	h.smu.Unlock()
	if old == nil || old == c {
		return false
	}

	h.mu.Lock()
	rooms := make([]*Room, 0, len(h.rooms))
	for _, r := range h.rooms {
		rooms = append(rooms, r)
	}
	h.mu.Unlock()

	for _, r := range rooms {
		if !r.Resume(old, c) {
			continue
		}
		c.room = r
		h.smu.Lock()
		delete(h.sessions, c.token) // c's own fresh token
		h.sessions[token] = c
		h.smu.Unlock()
		c.token = token
		h.leaveLobby(c)
		return true
	}
	return false
}

// Room returns the room with this name (case-insensitive), creating it if needed.
func (h *Hub) Room(name string) *Room {
	h.mu.Lock()
	defer h.mu.Unlock()
	key := strings.ToLower(name)
	r, ok := h.rooms[key]
	if !ok {
		r = newRoom(h, key, name)
		h.rooms[key] = r
		log.Printf("room %q created", name)
	}
	return r
}

// removeIfEmpty is called by a room after its last player left.
func (h *Hub) removeIfEmpty(r *Room) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.rooms[r.key] == r && r.Empty() {
		delete(h.rooms, r.key)
		r.Close()
		log.Printf("room %q closed", r.name)
		h.roomsChanged()
	}
}

func (h *Hub) newID() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.nextID++
	return h.nextID
}

func (h *Hub) ServeWS(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// The game client isn't a browser; there is no origin to check.
		InsecureSkipVerify: true,
	})
	if err != nil {
		return
	}

	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	c := &Client{id: h.newID(), hub: h, conn: conn, send: make(chan []byte, sendBuffer), token: newToken()}
	h.setSession(c)
	log.Printf("client %d connected from %s", c.id, r.RemoteAddr)

	go c.writeLoop(ctx)
	go c.probeLoop(ctx)
	err = c.readLoop(ctx)

	if c.superseded.Load() {
		// The player reconnected and the new connection owns the id, room slot and avatar now.
		conn.CloseNow()
		log.Printf("client %d (%s) replaced by a new connection", c.id, c.name)
		return
	}
	h.leaveLobby(c)
	// A connection that ends without a close frame (network drop, app killed) keeps the player's slot for a while
	// so a client that can resume gets it back; leaving on purpose (close frame), or a client that doesn't resume,
	// frees it at once.
	left := websocket.CloseStatus(err) == websocket.StatusNormalClosure || websocket.CloseStatus(err) == websocket.StatusGoingAway
	if !left && c.resumable && c.room != nil && c.room.Detach(c) {
		conn.CloseNow()
		log.Printf("client %d (%s) lost connection, slot held for %s", c.id, c.name, h.grace())
		return
	}
	h.setAvatar(c.id, nil)
	h.dropSession(c)
	if c.room != nil {
		c.room.Leave(c)
	}
	conn.Close(websocket.StatusNormalClosure, "")
	log.Printf("client %d (%s) disconnected", c.id, c.name)
}

// DefaultCountStep is one tick of the 3-2-1 before a round is released: 0.5 s each, 1.5 s in all.
const DefaultCountStep = 500 * time.Millisecond
