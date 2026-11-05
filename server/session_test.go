package main

import (
	"testing"
	"time"
)

// playRound finishes a started round: both players load, then each finishes with the given score.
func playRound(t *testing.T, host, guest *testClient, hostScore, guestScore int) {
	t.Helper()
	host.send("loaded", nil)
	guest.send("loaded", nil)
	host.expect("go", nil)
	guest.expect("go", nil)
	host.send("finish", FinishMsg{Result: Result{Score: Score{Score: hostScore}, Grade: 3}})
	guest.send("finish", FinishMsg{Result: Result{Score: Score{Score: guestScore}, Grade: 3}})
	host.expect("results", nil)
	guest.expect("results", nil)
}

func TestCountdownBeforeGo(t *testing.T) {
	host, guest := startedRound(t)
	host.send("loaded", nil)
	guest.send("loaded", nil)
	for _, want := range []int{3, 2, 1} {
		var c CountdownMsg
		guest.expect("countdown", &c)
		if c.N != want {
			t.Fatalf("countdown %d, want %d", c.N, want)
		}
	}
	guest.expect("go", nil)
	host.expect("go", nil)
}

func TestCountdownStopsWhenRoundEnds(t *testing.T) {
	url, hub := startServerHub(t)
	hub.countStep = 80 * time.Millisecond
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", validPick(54))
	host.expect("start", nil)
	host.send("loaded", nil)
	guest.send("loaded", nil)
	host.expect("countdown", nil)
	// Everyone quits during the 3-2-1: no `go` may follow.
	host.send("finish", FinishMsg{Result: Result{Failed: true}})
	guest.send("finish", FinishMsg{Result: Result{Failed: true}})
	host.expect("results", nil)
	host.expectNoneBeforeMarkerAfter("go", 400*time.Millisecond)
}

func TestSessionWins(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")

	round := func(hostScore, guestScore int) RoomMsg {
		guest.send("ready", ReadyMsg{Ready: true})
		host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
		host.send("start", validPick(54))
		host.expect("start", nil)
		guest.expect("start", nil)
		playRound(t, host, guest, hostScore, guestScore)
		return host.expectRoom(func(r RoomMsg) bool { return r.State == StateLobby && !r.Players[0].Playing })
	}
	r := round(900000, 800000)
	if r.Rounds != 1 || r.Players[0].Wins != 1 || r.Players[1].Wins != 0 {
		t.Fatalf("after round 1: %+v", r)
	}
	r = round(500000, 700000)
	if r.Rounds != 2 || r.Players[0].Wins != 1 || r.Players[1].Wins != 1 {
		t.Fatalf("after round 2: %+v", r)
	}
	r = round(900000, 100000)
	if r.Rounds != 3 || r.Players[0].Wins != 2 {
		t.Fatalf("after round 3: %+v", r)
	}
}

// A round played alone has nobody to beat: it doesn't count.
func TestSoloRoundIsNotAWin(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	host.send("join", JoinMsg{Room: "solo"})
	host.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	host.send("start", validPick(54))
	host.expect("start", nil)
	host.send("loaded", nil)
	host.expect("go", nil)
	host.send("finish", FinishMsg{Result: Result{Score: Score{Score: 1000}}})
	host.expect("results", nil)
	r := host.expectRoom(func(r RoomMsg) bool { return r.State == StateLobby })
	if r.Rounds != 0 || r.Players[0].Wins != 0 {
		t.Fatalf("solo round counted: %+v", r)
	}
}

func TestLagFlag(t *testing.T) {
	url, hub := startServerHub(t)
	hub.lagAfter = 150 * time.Millisecond
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", validPick(54))
	host.expect("start", nil)
	guest.expect("start", nil)

	// Before the release: the player who hasn't loaded is "loading", not lagging.
	host.send("loaded", nil)
	var s ScoresMsg
	for {
		s = ScoresMsg{}
		host.expect("scores", &s)
		if len(s.Players) == 2 && s.Players[0].Loading != s.Players[1].Loading {
			break
		}
	}
	for _, p := range s.Players {
		if p.Lag || (p.Loading && p.Name != "Guest") {
			t.Fatalf("loading phase: %+v", s)
		}
	}
	guest.send("loaded", nil)
	host.expect("go", nil)

	// After it: the guest keeps reporting, the host goes quiet and gets flagged.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		guest.send("score", Score{Score: 10})
		s = ScoresMsg{}
		host.expect("scores", &s)
		var hostLag, guestLag bool
		for _, p := range s.Players {
			if p.Name == "Host" {
				hostLag = p.Lag
			} else {
				guestLag = p.Lag
			}
		}
		if guestLag {
			t.Fatalf("a player that keeps reporting was flagged: %+v", s)
		}
		if hostLag {
			// ...and clears again when the host reports.
			host.send("score", Score{Score: 20})
			for time.Now().Before(deadline) {
				s = ScoresMsg{}
				host.expect("scores", &s)
				if !s.Players[0].Lag && !s.Players[1].Lag {
					return
				}
				guest.send("score", Score{Score: 11})
			}
			t.Fatal("lag flag never cleared")
		}
	}
	t.Fatal("silent player was never flagged as lagging")
}

func TestModeOnlyHostInLobby(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")

	guest.send("mode", ModeMsg{Mode: ModeAccuracy})
	guest.expectError("only the host")
	host.send("mode", ModeMsg{Mode: "chaos"})
	host.expectError("unknown mode")
	host.send("mode", ModeMsg{Mode: ModeAccuracy})
	r := guest.expectRoom(func(r RoomMsg) bool { return r.Mode == ModeAccuracy })
	if r.State != StateLobby {
		t.Fatalf("room = %+v", r)
	}
	host.send("mode", ModeMsg{Mode: ModeScore})
	guest.expectRoom(func(r RoomMsg) bool { return r.Mode == "" }) // score is the default and is left out
}

// In accuracy mode the live ranking, the results and the wins follow the completion percentage, not the score.
func TestAccuracyMode(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	host.send("mode", ModeMsg{Mode: ModeAccuracy})
	guest.expectRoom(func(r RoomMsg) bool { return r.Mode == ModeAccuracy })
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", validPick(54))
	host.expect("start", nil)
	guest.expect("start", nil)
	host.send("loaded", nil)
	guest.send("loaded", nil)
	host.expect("go", nil)

	// The host scores more points, the guest hits more notes: the guest leads the live board.
	host.send("score", Score{Score: 900000, Completion: 80})
	guest.send("score", Score{Score: 700000, Completion: 95})
	for {
		var s ScoresMsg
		host.expect("scores", &s)
		if len(s.Players) == 2 && s.Players[0].Completion == 95 && s.Players[1].Completion == 80 {
			if s.Players[0].Name != "Guest" {
				t.Fatalf("scores = %+v", s)
			}
			break
		}
	}
	host.send("finish", FinishMsg{Result: Result{Score: Score{Score: 900000, Completion: 80}, Grade: 3}})
	guest.send("finish", FinishMsg{Result: Result{Score: Score{Score: 700000, Completion: 95}, Grade: 3}})
	var res ResultsMsg
	host.expect("results", &res)
	if res.Players[0].Name != "Guest" || res.Players[0].Place != 1 {
		t.Fatalf("results = %+v", res)
	}
	r := host.expectRoom(func(r RoomMsg) bool { return r.State == StateLobby && !r.Players[0].Playing })
	if r.Players[1].Wins != 1 || r.Players[0].Wins != 0 || r.Mode != ModeAccuracy {
		t.Fatalf("after round: %+v", r)
	}
}

// The host can start although a player isn't ready, with `force`; without it the start is refused.
func TestForceStart(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")

	host.send("start", validPick(54))
	host.expectError("Guest is not ready")
	guest.send("start", PickMsg{Block: validPick(54).Block, PV: 54, Force: true})
	guest.expectError("only the host")

	forced := validPick(54)
	forced.Force = true
	host.send("start", forced)
	var start PickMsg
	guest.expect("start", &start)
	if start.PV != 54 || start.Force {
		t.Fatalf("start = %+v", start)
	}
	host.expectRoom(func(r RoomMsg) bool { return r.State == StatePlaying })
}
