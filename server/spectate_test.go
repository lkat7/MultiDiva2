package main

import (
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"
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
