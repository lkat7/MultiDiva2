package main

import (
	"context"
	"encoding/json"
	"github.com/coder/websocket"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestSpectatePage(t *testing.T) {
	url, hub := startServerHub(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "Miku")

	// Lobby: the room, its players and the pick.
	host.send("pick", validPick(54))
	host.expectRoom(func(r RoomMsg) bool { return r.Pick != nil })
	m := hub.spectateList()
	if len(m.Rooms) != 1 || m.Rooms[0].Name != "Miku" || m.Rooms[0].State != StateLobby || len(m.Rooms[0].Players) != 2 ||
		m.Rooms[0].Pick == nil || m.Rooms[0].Pick.PV != 54 || !m.Rooms[0].Players[0].Host {
		t.Fatalf("lobby: %+v", m)
	}

	// Playing: live scores, best first.
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", nil)
	host.expect("start", nil)
	host.send("score", Score{Score: 1000, Combo: 10, Completion: 12.5})
	guest.send("score", Score{Score: 5000, Combo: 50, Completion: 40})
	for {
		var s ScoresMsg
		host.expect("scores", &s)
		if len(s.Players) == 2 && s.Players[0].Score == 5000 {
			break
		}
	}
	rec := httptest.NewRecorder()
	hub.ServeSpectateAPI(rec, httptest.NewRequest("GET", "/api/rooms", nil))
	var live SpectateMsg
	if err := json.Unmarshal(rec.Body.Bytes(), &live); err != nil {
		t.Fatal(err)
	}
	r := live.Rooms[0]
	if r.State != StatePlaying || r.Players[0].Name != "Guest" || r.Players[0].Score != 5000 || r.Players[0].Completion != 40 ||
		!r.Players[0].Playing || r.Players[1].Score != 1000 {
		t.Fatalf("playing: %+v", r)
	}

	// Round over: back to the lobby with the ranking kept as the last round.
	host.send("finish", FinishMsg{Result: Result{Score: Score{Score: 400000}}})
	guest.send("finish", FinishMsg{Result: Result{Score: Score{Score: 380000}}})
	guest.expect("results", nil)
	guest.expectRoom(func(r RoomMsg) bool { return r.State == StateLobby })
	r = hub.spectateList().Rooms[0]
	if r.State != StateLobby || r.Last == nil || len(r.Last.Results) != 2 || r.Last.Results[0].Name != "Host" || r.Last.Pick.PV != 54 {
		t.Fatalf("after round: %+v", r)
	}
}

func TestSpectatePageServed(t *testing.T) {
	hub := NewHub(0, 0)
	h := httpHandler(hub, true)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest("GET", "/", nil))
	if rec.Code != 200 || !strings.Contains(rec.Body.String(), "MultiDiva2 Live") {
		t.Fatalf("page: %d", rec.Code)
	}
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest("GET", "/nope", nil))
	if rec.Code != 404 {
		t.Fatalf("unknown path: %d", rec.Code)
	}
	rec = httptest.NewRecorder()
	httpHandler(hub, false).ServeHTTP(rec, httptest.NewRequest("GET", "/api/rooms", nil))
	if rec.Code != 404 {
		t.Fatalf("spectate off: %d", rec.Code)
	}
}

func TestSpectateWebSocket(t *testing.T) {
	url, hub := startServerHub(t)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, strings.TrimSuffix(url, "/ws")+"/spectate-ws", nil)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.CloseNow()
	next := func(pred func(SpectateMsg) bool) SpectateMsg {
		t.Helper()
		for {
			_, data, err := conn.Read(ctx)
			if err != nil {
				t.Fatalf("read: %v", err)
			}
			var m SpectateMsg
			if err := json.Unmarshal(data, &m); err != nil {
				t.Fatal(err)
			}
			if pred(m) {
				return m
			}
		}
	}
	next(func(m SpectateMsg) bool { return len(m.Rooms) == 0 }) // first snapshot arrives without asking

	// A change is pushed by itself.
	host := connect(t, url, "Host")
	host.send("join", JoinMsg{Room: "Miku"})
	m := next(func(m SpectateMsg) bool { return len(m.Rooms) == 1 })
	if m.Rooms[0].Players[0].Name != "Host" {
		t.Fatalf("pushed: %+v", m)
	}
	host.send("score", Score{Score: 1})
	host.conn.Close(websocket.StatusNormalClosure, "")
	next(func(m SpectateMsg) bool { return len(m.Rooms) == 0 })

	// Closing the page stops the sampling goroutine.
	conn.Close(websocket.StatusNormalClosure, "")
	deadline := time.Now().Add(2 * time.Second)
	for {
		hub.spectators.mu.Lock()
		idle := !hub.spectators.running
		hub.spectators.mu.Unlock()
		if idle {
			return
		}
		if time.Now().After(deadline) {
			t.Fatal("sampling goroutine still running after the last page left")
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func TestSteamAvatarURLOnlyFromSteamHosts(t *testing.T) {
	for raw, want := range map[string]bool{
		"https://avatars.fastly.steamstatic.com/abc_medium.jpg": true,
		"https://avatars.akamai.steamstatic.com/abc.jpg":        true,
		"http://avatars.fastly.steamstatic.com/abc.jpg":         false, // not https
		"https://evil.example/steamstatic.com/abc.jpg":          false,
		"https://steamstatic.com.evil.example/abc.jpg":          false,
		"https://user@avatars.fastly.steamstatic.com/abc.jpg":   false,
		"javascript:alert(1)":                                   false,
	} {
		if _, ok := steamAvatarURL(raw); ok != want {
			t.Errorf("steamAvatarURL(%q) = %v, want %v", raw, ok, want)
		}
	}
}

// The page gets a player's Steam picture address once the background lookup has finished, and never the Steam ID.
func TestSpectateSteamAvatar(t *testing.T) {
	url, hub := startServerHub(t)
	var mu sync.Mutex
	asked := map[string]int{}
	hub.steamPics.fetch = func(id string) (string, error) {
		mu.Lock()
		asked[id]++
		mu.Unlock()
		return "https://avatars.fastly.steamstatic.com/pic_" + id + "_medium.jpg", nil
	}
	c := connectHello(t, url, HelloMsg{Name: "Steamy", Version: ProtocolVersion, Steam: "76561197960287930"})
	c.send("join", JoinMsg{Room: "r"})
	c.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })

	deadline := time.Now().Add(2 * time.Second)
	for {
		m := hub.spectateList()
		if got := m.Rooms[0].Players[0].AvatarURL; got != "" {
			if got != "https://avatars.fastly.steamstatic.com/pic_76561197960287930_medium.jpg" {
				t.Fatalf("avatar url = %q", got)
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("the Steam picture never arrived")
		}
		time.Sleep(10 * time.Millisecond)
	}
	raw, _ := json.Marshal(hub.spectateList())
	if strings.Contains(string(raw), `"steam"`) {
		t.Fatalf("the Steam ID is in the page data: %s", raw)
	}
	for i := 0; i < 5; i++ {
		hub.spectateList() // cached: no further lookups
	}
	mu.Lock()
	defer mu.Unlock()
	if asked["76561197960287930"] != 1 {
		t.Fatalf("looked up %d times, want 1", asked["76561197960287930"])
	}
}
