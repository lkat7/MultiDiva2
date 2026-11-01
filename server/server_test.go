package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"image"
	"image/png"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/coder/websocket"
)

type testClient struct {
	t     *testing.T
	conn  *websocket.Conn
	id    int
	token string
}

func startServer(t *testing.T) string {
	url, _ := startServerHub(t)
	return url
}

func startServerHub(t *testing.T) (string, *Hub) {
	hub := NewHub(20*time.Millisecond, 300*time.Millisecond)
	srv := httptest.NewServer(httpHandler(hub))
	t.Cleanup(srv.Close)
	return "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws", hub
}

func connect(t *testing.T, url, name string) *testClient {
	t.Helper()
	return connectResume(t, url, name, "")
}

// connectResume says hello with a resume token from an earlier connection ("" = a fresh player).
func connectResume(t *testing.T, url, name, token string) *testClient {
	t.Helper()
	return connectHello(t, url, HelloMsg{Name: name, Version: ProtocolVersion, Resume: token, Resumable: true})
}

func connectHello(t *testing.T, url string, hello HelloMsg) *testClient {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.CloseNow() })
	c := &testClient{t: t, conn: conn}
	c.send("hello", hello)
	var w WelcomeMsg
	c.expect("welcome", &w)
	c.id, c.token = w.ID, w.Token
	if w.Server != ServerVersion {
		t.Fatalf("welcome server version = %q, want %q", w.Server, ServerVersion)
	}
	return c
}

func (c *testClient) send(typ string, data any) {
	c.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := c.conn.Write(ctx, websocket.MessageText, encode(typ, data)); err != nil {
		c.t.Fatalf("write %s: %v", typ, err)
	}
}

// expect reads until a message of type typ arrives (skipping others) and decodes it into out.
func (c *testClient) expect(typ string, out any) {
	c.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	for {
		_, data, err := c.conn.Read(ctx)
		if err != nil {
			c.t.Fatalf("waiting for %s: %v", typ, err)
		}
		var env Envelope
		if err := json.Unmarshal(data, &env); err != nil {
			c.t.Fatalf("bad envelope: %v", err)
		}
		if env.Type == typ {
			if out != nil {
				if err := json.Unmarshal(env.Data, out); err != nil {
					c.t.Fatalf("decode %s: %v", typ, err)
				}
			}
			return
		}
	}
}

// expectRoom waits for a room update matching pred.
func (c *testClient) expectRoom(pred func(RoomMsg) bool) RoomMsg {
	c.t.Helper()
	for {
		var r RoomMsg
		c.expect("room", &r)
		if pred(r) {
			return r
		}
	}
}

func (c *testClient) expectError(contains string) {
	c.t.Helper()
	var e ErrorMsg
	c.expect("error", &e)
	if !strings.Contains(e.Message, contains) {
		c.t.Fatalf("error %q does not contain %q", e.Message, contains)
	}
}

// joinInOrder makes host the room's host: separate connections race, so wait for each join.
func joinInOrder(host, guest *testClient, room string) {
	host.t.Helper()
	host.send("join", JoinMsg{Room: room})
	host.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	guest.send("join", JoinMsg{Room: room})
	guest.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 2 })
}

func validPick(pv int) PickMsg {
	return PickMsg{Block: base64.StdEncoding.EncodeToString(make([]byte, LaunchBlockSize)), PV: pv, Difficulty: 3, Title: "Roshin Yukai"}
}

func TestFullRound(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")

	host.send("join", JoinMsg{Room: "Miku"})
	host.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 && r.Host == host.id })
	guest.send("join", JoinMsg{Room: "miku"}) // room names are case-insensitive
	guest.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 2 && r.Host == host.id })

	// Chat reaches everyone, including the sender.
	guest.send("chat", ChatMsg{Text: "  hello\x07 "})
	var chat ChatOutMsg
	for chat.From != guest.id {
		host.expect("chat", &chat)
	}
	if chat.Text != "hello" || chat.Name != "Guest" {
		t.Fatalf("chat = %+v", chat)
	}

	// Only the host picks; a pick clears ready flags.
	guest.send("pick", validPick(1))
	guest.expectError("only the host")
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("pick", validPick(54))
	r := host.expectRoom(func(r RoomMsg) bool { return r.Pick != nil })
	if r.Pick.PV != 54 || r.Players[1].Ready {
		t.Fatalf("after pick: %+v", r)
	}

	// Start needs everyone ready.
	host.send("start", nil)
	host.expectError("Guest is not ready")
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", nil)

	var start PickMsg
	guest.expect("start", &start)
	if start.PV != 54 || start.Block == "" {
		t.Fatalf("start = %+v", start)
	}
	host.expect("start", nil)

	// Live scores are broadcast sorted by score.
	host.send("score", Score{Score: 1000, Combo: 10})
	guest.send("score", Score{Score: 5000, Combo: 50})
	for {
		var s ScoresMsg
		host.expect("scores", &s)
		if len(s.Players) == 2 && s.Players[0].Score == 5000 {
			if s.Players[0].ID != guest.id || s.Players[1].Score != 1000 {
				t.Fatalf("scores = %+v", s)
			}
			break
		}
	}

	// Round ends when everyone finished.
	host.send("finish", FinishMsg{Result: Result{Score: Score{Score: 400000}, Grade: 4}})
	guest.send("finish", FinishMsg{Result: Result{Score: Score{Score: 380000}, Grade: 3}})
	var res ResultsMsg
	guest.expect("results", &res)
	if len(res.Players) != 2 || res.Players[0].ID != host.id || res.Players[0].Place != 1 || res.Players[1].Result.Grade != 3 {
		t.Fatalf("results = %+v", res)
	}
	guest.expectRoom(func(r RoomMsg) bool { return r.State == StateLobby && !r.Players[0].Playing })
}

func TestHostLeavesMidRound(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")

	// The pick clears ready flags, so ready up only after it arrived.
	host.send("pick", validPick(54))
	guest.expectRoom(func(r RoomMsg) bool { return r.Pick != nil })
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", nil)
	guest.expect("start", nil)

	host.conn.Close(websocket.StatusNormalClosure, "")
	r := guest.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	if r.Host != guest.id {
		t.Fatalf("host not handed over: %+v", r)
	}

	// The remaining player finishing ends the round with only their result.
	guest.send("finish", FinishMsg{Result: Result{Score: Score{Score: 1}}})
	var res ResultsMsg
	guest.expect("results", &res)
	if len(res.Players) != 1 || res.Players[0].ID != guest.id {
		t.Fatalf("results = %+v", res)
	}
}

// The host's game started a song: the start message carries the launch block, no separate pick.
func TestStartWithBlock(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 2 && r.Players[1].Ready })

	host.send("start", validPick(728))
	var start PickMsg
	guest.expect("start", &start)
	if start.PV != 728 {
		t.Fatalf("start = %+v", start)
	}
	r := guest.expectRoom(func(r RoomMsg) bool { return r.State == StatePlaying })
	if r.Pick == nil || r.Pick.PV != 728 {
		t.Fatalf("room = %+v", r)
	}
}

func startedRound(t *testing.T) (host, guest *testClient) {
	url := startServer(t)
	host = connect(t, url, "Host")
	guest = connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", validPick(54))
	guest.expect("start", nil)
	host.expect("start", nil)
	return host, guest
}

// expectNoneBeforeMarker sends a chat marker and fails if a `forbidden` message arrives before
// the marker comes back. (A read deadline would close the connection, so no timeouts here.)
func (c *testClient) expectNoneBeforeMarker(forbidden string) {
	c.t.Helper()
	c.send("chat", ChatMsg{Text: "marker"})
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	for {
		_, data, err := c.conn.Read(ctx)
		if err != nil {
			c.t.Fatalf("waiting for marker: %v", err)
		}
		var env Envelope
		if json.Unmarshal(data, &env) != nil {
			continue
		}
		if env.Type == forbidden {
			c.t.Fatalf("unexpected %s", forbidden)
		}
		var chat ChatOutMsg
		if env.Type == "chat" && json.Unmarshal(env.Data, &chat) == nil && chat.Text == "marker" {
			return
		}
	}
}

func TestLoadingBarrier(t *testing.T) {
	host, guest := startedRound(t)

	host.send("loaded", nil)
	guest.expectRoom(func(r RoomMsg) bool { return r.Players[0].Loaded && !r.Players[1].Loaded })
	guest.expectNoneBeforeMarker("go")

	guest.send("loaded", nil)
	host.expect("go", nil)
	guest.expect("go", nil)
}

func TestLoadingTimeout(t *testing.T) {
	host, guest := startedRound(t)
	host.send("loaded", nil)
	// The guest never loads: the server starts without them after the (test) timeout.
	host.expect("go", nil)
	guest.expect("go", nil)
}

func TestLoadingPlayerLeaves(t *testing.T) {
	host, guest := startedRound(t)
	host.send("loaded", nil)
	guest.conn.Close(websocket.StatusNormalClosure, "")
	host.expect("go", nil)
}

func TestValidation(t *testing.T) {
	url := startServer(t)

	ctx := context.Background()
	conn, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.CloseNow()
	raw := &testClient{t: t, conn: conn}
	raw.send("join", JoinMsg{Room: "x"})
	raw.expectError("hello first")
	raw.send("hello", HelloMsg{Name: "Old", Version: 0})
	raw.expectError("version mismatch")

	c := connect(t, url, "A")
	c.send("chat", ChatMsg{Text: "hi"})
	c.expectError("not in a room")
	c.send("join", JoinMsg{Room: "x"})
	c.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	bad := validPick(1)
	bad.Block = base64.StdEncoding.EncodeToString([]byte("short"))
	c.send("pick", bad)
	c.expectError("invalid launch block")
	c.send("start", nil)
	c.expectError("pick a song first")
}

func TestTitleUpdate(t *testing.T) {
	host, guest := startedRound(t)
	guest.send("title", TitleMsg{Title: "Hijack"}) // ignored: not the host
	host.send("title", TitleMsg{Title: "Freely Tomorrow"})
	r := guest.expectRoom(func(r RoomMsg) bool { return r.Pick != nil && r.Pick.Title != "Roshin Yukai" })
	if r.Pick.Title != "Freely Tomorrow" {
		t.Fatalf("title = %q (only the host may set it)", r.Pick.Title)
	}
}

func TestSteamIDRelayed(t *testing.T) {
	url := startServer(t)
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.CloseNow()
	c := &testClient{t: t, conn: conn}
	c.send("hello", HelloMsg{Name: "A", Version: ProtocolVersion, Steam: "76561198000000000"})
	c.expect("welcome", nil)
	c.send("join", JoinMsg{Room: "s"})
	r := c.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	if r.Players[0].Steam != "76561198000000000" {
		t.Fatalf("steam = %q", r.Players[0].Steam)
	}

	bad := connect(t, url, "B") // no Steam ID, and junk would be dropped
	bad.send("join", JoinMsg{Room: "s"})
	r = bad.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 2 })
	if r.Players[1].Steam != "" {
		t.Fatalf("steam = %q", r.Players[1].Steam)
	}
}

func TestAvatarServed(t *testing.T) {
	url := startServer(t)
	img := image.NewRGBA(image.Rect(0, 0, 8, 8))
	var buf bytes.Buffer
	png.Encode(&buf, img)
	b64 := base64.StdEncoding.EncodeToString(buf.Bytes())

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.CloseNow()
	c := &testClient{t: t, conn: conn}
	c.send("hello", HelloMsg{Name: "Bot", Version: ProtocolVersion, Avatar: b64})
	var w WelcomeMsg
	c.expect("welcome", &w)
	c.send("join", JoinMsg{Room: "a"})
	r := c.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	if !r.Players[0].Avatar {
		t.Fatal("avatar flag not set")
	}

	base := "http" + strings.TrimSuffix(strings.TrimPrefix(url, "ws"), "/ws")
	resp, err := http.Get(base + "/avatar/" + strconv.Itoa(w.ID))
	if err != nil {
		t.Fatal(err)
	}
	got, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 200 || resp.Header.Get("Content-Type") != "image/png" || !bytes.Equal(got, buf.Bytes()) {
		t.Fatalf("GET avatar: %d %q, %d bytes", resp.StatusCode, resp.Header.Get("Content-Type"), len(got))
	}
	if resp, err := http.Get(base + "/avatar/999"); err != nil || resp.StatusCode != 404 {
		t.Fatalf("unknown id should 404")
	}

	// Junk and oversized images are dropped.
	big := image.NewRGBA(image.Rect(0, 0, MaxAvatarSide+1, 4))
	buf.Reset()
	png.Encode(&buf, big)
	for _, bad := range []string{"bm90IGFuIGltYWdl", base64.StdEncoding.EncodeToString(buf.Bytes())} {
		if _, ok := parseAvatar(bad); ok {
			t.Fatalf("accepted bad avatar %.20q", bad)
		}
	}
}

func TestRepeatedNamesNumbered(t *testing.T) {
	url := startServer(t)
	long := strings.Repeat("あ", MaxNameLen)
	names := []string{"初音ミク", "初音ミク", "初音ミク", "MIKU", "miku", long, long}
	want := []string{"初音ミク", "初音ミク (2)", "初音ミク (3)", "MIKU", "miku (2)", long, strings.Repeat("あ", MaxNameLen-4) + " (2)"}
	var last RoomMsg
	for i, n := range names {
		c := connect(t, url, n)
		c.send("join", JoinMsg{Room: "dupes"})
		last = c.expectRoom(func(r RoomMsg) bool { return len(r.Players) == i+1 })
	}
	for i, p := range last.Players {
		if p.Name != want[i] {
			t.Errorf("player %d: name %q, want %q", i, p.Name, want[i])
		}
		if utf8.RuneCountInString(p.Name) > MaxNameLen {
			t.Errorf("player %d: %q longer than %d", i, p.Name, MaxNameLen)
		}
	}

	// The number belongs to the room: in another room the player has their own name again.
	c := connect(t, url, "初音ミク")
	c.send("join", JoinMsg{Room: "dupes"})
	c.expectRoom(func(r RoomMsg) bool { return len(r.Players) == len(names)+1 })
	c.send("join", JoinMsg{Room: "elsewhere"})
	r := c.expectRoom(func(r RoomMsg) bool { return r.Name == "elsewhere" })
	if r.Players[0].Name != "初音ミク" {
		t.Fatalf("in a new room: %q", r.Players[0].Name)
	}
}

func TestGiveHostAndKick(t *testing.T) {
	url := startServer(t)
	a := connect(t, url, "A")
	b := connect(t, url, "B")
	joinInOrder(a, b, "k")

	// Only the host may hand over host or kick.
	b.send("host", PlayerRefMsg{ID: b.id})
	b.expectError("only the host")
	b.send("kick", PlayerRefMsg{ID: a.id})
	b.expectError("only the host")
	a.send("kick", PlayerRefMsg{ID: 999})
	a.expectError("no such player")

	a.send("host", PlayerRefMsg{ID: b.id})
	a.expectRoom(func(r RoomMsg) bool { return r.Host == b.id })

	// B is the host now and kicks A, who is told and leaves the room.
	b.send("kick", PlayerRefMsg{ID: a.id})
	var k KickedMsg
	a.expect("kicked", &k)
	if k.Room != "k" {
		t.Fatalf("kicked from %q", k.Room)
	}
	b.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 && r.Players[0].ID == b.id })
	a.send("chat", ChatMsg{Text: "still here?"})
	a.expectError("not in a room")

	// A kick is not a ban.
	a.send("join", JoinMsg{Room: "k"})
	a.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 2 })
}

func TestMissingSong(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "m")

	// The host's highlight becomes the focus; only the host's counts.
	guest.send("hover", HoverMsg{PV: 7})
	host.send("hover", HoverMsg{PV: 54, Title: "Roshin Yukai"})
	r := host.expectRoom(func(r RoomMsg) bool { return r.Hover == 54 })
	if r.Focus != 54 || r.HoverTitle != "Roshin Yukai" {
		t.Fatalf("focus = %d, title %q", r.Focus, r.HoverTitle)
	}

	// The guest doesn't have it: flagged, and the round starts without them (no ready needed).
	guest.send("have", HaveMsg{PV: 54, OK: false})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Missing })
	host.send("pick", validPick(54))
	host.send("start", nil)
	host.expect("start", nil)
	r = host.expectRoom(func(r RoomMsg) bool { return r.State == StatePlaying })
	if !r.Players[0].Playing || r.Players[1].Playing || r.Hover != -1 {
		t.Fatalf("round players: %+v", r)
	}
	host.send("loaded", struct{}{})
	host.expect("go", nil)
	host.send("finish", FinishMsg{Result: Result{Score: Score{Score: 100}}})
	var res ResultsMsg
	host.expect("results", &res)
	if len(res.Players) != 1 || res.Players[0].ID != host.id {
		t.Fatalf("results: %+v", res)
	}

	// Next round the guest is ready but only finds out while loading: dropped, nobody waits for them.
	guest.send("have", HaveMsg{PV: 54, OK: true})
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.State == StateLobby && r.Players[1].Ready })
	host.send("start", nil)
	host.expect("start", nil)
	guest.send("have", HaveMsg{PV: 54, OK: false})
	host.expectRoom(func(r RoomMsg) bool { return r.State == StatePlaying && !r.Players[1].Playing })
	host.send("loaded", struct{}{})
	host.expect("go", nil)
}

// expectRooms waits for a room list matching pred.
func (c *testClient) expectRooms(pred func(RoomsMsg) bool) RoomsMsg {
	c.t.Helper()
	for {
		var r RoomsMsg
		c.expect("rooms", &r)
		if pred(r) {
			return r
		}
	}
}

func TestRoomList(t *testing.T) {
	url := startServer(t)
	a := connect(t, url, "A")
	a.expectRooms(func(r RoomsMsg) bool { return len(r.Rooms) == 0 })

	// B opens a room: A, still in the lobby, sees it with its host and size.
	b := connect(t, url, "B")
	b.send("join", JoinMsg{Room: "Miku"})
	got := a.expectRooms(func(r RoomsMsg) bool { return len(r.Rooms) == 1 })
	if room := got.Rooms[0]; room.Name != "Miku" || room.Host != "B" || room.Players != 1 || room.Playing {
		t.Fatalf("room list entry %+v", room)
	}

	// A joins: the list stops (A is in a room). C, in the lobby, sees two players.
	a.send("join", JoinMsg{Room: "miku"})
	a.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 2 })
	c := connect(t, url, "C")
	c.expectRooms(func(r RoomsMsg) bool { return len(r.Rooms) == 1 && r.Rooms[0].Players == 2 })

	// Everyone leaves: the room closes and C's list empties.
	a.send("leave", nil)
	b.send("leave", nil)
	c.expectRooms(func(r RoomsMsg) bool { return len(r.Rooms) == 0 })
	// A is back in the lobby and gets the list again.
	a.expectRooms(func(r RoomsMsg) bool { return len(r.Rooms) == 0 })
}

func TestRoomFull(t *testing.T) {
	url := startServer(t)
	var players []*testClient
	for i := 0; i < MaxPlayers; i++ {
		p := connect(t, url, "P")
		p.send("join", JoinMsg{Room: "full"})
		p.expectRoom(func(r RoomMsg) bool { return len(r.Players) == i+1 })
		players = append(players, p)
	}
	late := connect(t, url, "Late")
	late.send("join", JoinMsg{Room: "full"})
	late.expectError("is full")
	late.send("chat", ChatMsg{Text: "am i in?"})
	late.expectError("not in a room")
	// A seat frees up: now it works.
	players[0].send("leave", nil)
	late.send("join", JoinMsg{Room: "full"})
	late.expectRoom(func(r RoomMsg) bool { return len(r.Players) == MaxPlayers })
}

// roundOf starts a round for a fresh host and guest, returning the server URL as well.
func roundOf(t *testing.T) (url string, hub *Hub, host, guest *testClient) {
	url, hub = startServerHub(t)
	host = connect(t, url, "Host")
	guest = connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", validPick(54))
	guest.expect("start", nil)
	host.expect("start", nil)
	return url, hub, host, guest
}

// A dropped connection (no close frame) keeps the player's slot: the host role passes on, the round goes on, and
// the same player resumes with their token: same id and name (no "(2)"), score kept.
func TestResumeMidRound(t *testing.T) {
	url, _, host, guest := roundOf(t)
	host.send("score", Score{Score: 1234})
	oldID := host.id

	host.conn.CloseNow() // vanishes without leaving
	r := guest.expectRoom(func(r RoomMsg) bool { return r.Players[0].Away })
	if r.Host != guest.id || len(r.Players) != 2 {
		t.Fatalf("after drop: %+v", r)
	}

	back := connectResume(t, url, "Host", host.token)
	if back.id != oldID || back.token != host.token {
		t.Fatalf("resumed as id %d token %q, want id %d token %q", back.id, back.token, oldID, host.token)
	}
	r = guest.expectRoom(func(r RoomMsg) bool { return !r.Players[0].Away })
	if len(r.Players) != 2 || r.Players[0].Name != "Host" || r.Host != guest.id {
		t.Fatalf("after resume: %+v", r)
	}

	// Both finishing ends the round with both results (the resumed player's slot was never lost).
	guest.send("finish", FinishMsg{Result: Result{Score: Score{Score: 1}}})
	back.send("finish", FinishMsg{Result: Result{Score: Score{Score: 2}}})
	var res ResultsMsg
	back.expect("results", &res)
	if len(res.Players) != 2 || res.Players[0].ID != oldID {
		t.Fatalf("results = %+v", res)
	}
}

// A player who doesn't come back within the grace period is removed, which lets the round end without them.
func TestDroppedPlayerExpires(t *testing.T) {
	_, hub, host, guest := roundOf(t)
	hub.smu.Lock()
	hub.resumeGrace = 100 * time.Millisecond
	hub.smu.Unlock()
	host.conn.CloseNow()
	guest.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })

	guest.send("finish", FinishMsg{Result: Result{Score: Score{Score: 1}}})
	var res ResultsMsg
	guest.expect("results", &res)
	if len(res.Players) != 1 {
		t.Fatalf("results = %+v", res)
	}
}

// An older client rejoins without a token: the dropped entry of the same name is replaced, not numbered around.
func TestRejoinWithoutTokenFreesName(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Miku")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")

	host.conn.CloseNow()
	guest.expectRoom(func(r RoomMsg) bool { return r.Players[0].Away })

	back := connect(t, url, "Miku")
	back.send("join", JoinMsg{Room: "r"})
	r := back.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 2 && r.Players[0].Name == "Guest" })
	if r.Players[1].Name != "Miku" || r.Players[1].Away {
		t.Fatalf("room = %+v", r)
	}
	if r.Host != guest.id {
		t.Fatalf("host = %d, want the guest %d", r.Host, guest.id)
	}
}

// Leaving on purpose (close frame) frees the slot at once; only a silent drop is held for a resume.
func TestCleanCloseLeavesAtOnce(t *testing.T) {
	url := startServer(t)
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	host.conn.Close(websocket.StatusNormalClosure, "")
	guest.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	if back := connectResume(t, url, "Host", host.token); back.token == host.token {
		t.Fatal("a player who left resumed their old session")
	}
}

// A peer that stops answering pings is dropped (a half-open connection never reports an error by itself).
func TestSilentPeerDropped(t *testing.T) {
	url, hub := startServerHub(t)
	hub.smu.Lock()
	hub.probeEvery, hub.probeWait = 50*time.Millisecond, 100*time.Millisecond
	hub.smu.Unlock()
	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	// host never reads again, so its library never answers the server's pings; guest keeps reading.
	r := guest.expectRoom(func(r RoomMsg) bool { return r.Players[0].Away })
	if r.Host != guest.id {
		t.Fatalf("host not passed on: %+v", r)
	}
}

func testAvatar(t *testing.T) string {
	var buf bytes.Buffer
	png.Encode(&buf, image.NewRGBA(image.Rect(0, 0, 8, 8)))
	return base64.StdEncoding.EncodeToString(buf.Bytes())
}

func avatarStatus(t *testing.T, url string, id int) int {
	t.Helper()
	base := "http" + strings.TrimSuffix(strings.TrimPrefix(url, "ws"), "/ws")
	resp, err := http.Get(base + "/avatar/" + strconv.Itoa(id))
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	return resp.StatusCode
}

// The old connection is still open (a half-dead socket the server hasn't noticed) when the player comes back with
// their token: the new connection takes over the same id and slot, stays host, and the old one closing afterwards
// must neither remove the new member nor wipe its avatar.
func TestResumeFromLiveConnection(t *testing.T) {
	url := startServer(t)
	img := testAvatar(t)
	old := connectHello(t, url, HelloMsg{Name: "Miku", Version: ProtocolVersion, Avatar: img, Resumable: true})
	guest := connect(t, url, "Guest")
	joinInOrder(old, guest, "r")

	back := connectHello(t, url, HelloMsg{Name: "Miku", Version: ProtocolVersion, Avatar: img, Resume: old.token, Resumable: true})
	if back.id != old.id || back.token != old.token {
		t.Fatalf("resumed as id %d token %q, want %d %q", back.id, back.token, old.id, old.token)
	}

	// The superseded connection is closed by the server.
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	for {
		if _, _, err := old.conn.Read(ctx); err != nil {
			if ctx.Err() != nil {
				t.Fatal("old connection was not closed")
			}
			break
		}
	}
	time.Sleep(100 * time.Millisecond) // its server-side cleanup runs after the close

	guest.send("ready", ReadyMsg{Ready: true}) // forces a fresh room update
	r := guest.expectRoom(func(r RoomMsg) bool {
		return len(r.Players) > 0 && r.Players[0].Ready || len(r.Players) > 1 && r.Players[1].Ready
	})
	if len(r.Players) != 2 || r.Players[0].Name != "Miku" || r.Players[0].Away || r.Host != back.id {
		t.Fatalf("room after takeover: %+v", r)
	}
	if got := avatarStatus(t, url, back.id); got != 200 {
		t.Fatalf("avatar of the resumed player: status %d, want 200", got)
	}
}

// A player who was away while the loading barrier was released gets the `go` they missed.
func TestResumeGetsMissedGo(t *testing.T) {
	url, _, host, guest := roundOf(t)
	guest.conn.CloseNow() // drops before loading
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Away })
	host.send("loaded", nil)
	host.expect("go", nil) // released by the load timeout, without the absent guest

	back := connectResume(t, url, "Guest", guest.token)
	back.expect("go", nil)
}

// A token the server doesn't know (expired, or from before a restart) is just a fresh hello.
func TestUnknownResumeTokenIsFresh(t *testing.T) {
	url := startServer(t)
	c := connectResume(t, url, "Miku", "00000000000000000000000000000000")
	if c.token == "" || c.token == "00000000000000000000000000000000" {
		t.Fatalf("token = %q", c.token)
	}
	c.send("join", JoinMsg{Room: "r"})
	r := c.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	if r.Host != c.id || r.Players[0].Name != "Miku" {
		t.Fatalf("room = %+v", r)
	}
}

// A dropped host's slot expiring leaves the room to the others; a room whose only player drops is closed.
func TestExpiryOfDroppedHost(t *testing.T) {
	url, hub := startServerHub(t)
	hub.smu.Lock()
	hub.resumeGrace = 100 * time.Millisecond
	hub.smu.Unlock()

	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	host.conn.CloseNow()
	r := guest.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	if r.Host != guest.id || r.Players[0].Name != "Guest" {
		t.Fatalf("room = %+v", r)
	}

	solo := connect(t, url, "Solo")
	solo.send("join", JoinMsg{Room: "solo"})
	solo.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	solo.conn.CloseNow()
	deadline := time.Now().Add(2 * time.Second)
	for {
		hub.mu.Lock()
		_, open := hub.rooms["solo"]
		hub.mu.Unlock()
		if !open {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("room with only a dropped player was never closed")
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// A client that doesn't say it resumes (an older version) is removed as soon as its connection ends, not held.
func TestNonResumableDropRemovedAtOnce(t *testing.T) {
	url := startServer(t)
	host := connectHello(t, url, HelloMsg{Name: "Old", Version: ProtocolVersion})
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")

	host.conn.CloseNow()
	r := guest.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 1 })
	if r.Host != guest.id || r.Players[0].Away {
		t.Fatalf("room = %+v", r)
	}
	// Rejoining under the same name is a plain join: no ghost, no "(2)".
	back := connectHello(t, url, HelloMsg{Name: "Old", Version: ProtocolVersion})
	back.send("join", JoinMsg{Room: "r"})
	r = back.expectRoom(func(r RoomMsg) bool { return len(r.Players) == 2 })
	if r.Players[1].Name != "Old" {
		t.Fatalf("room = %+v", r)
	}
}

func TestFormatRound(t *testing.T) {
	rec := RoundRecord{
		Time: time.Date(2026, 11, 1, 14, 3, 22, 0, time.UTC), Room: "miku",
		Pick: PickMsg{PV: 54, Difficulty: 2, Title: "Roshin Yukai"},
		Results: []FinalResult{
			{Name: "Alice", Place: 1, Result: Result{Score: Score{Score: 400000, Cool: 100, Completion: 100}, Grade: 4, MaxCombo: 150}},
			{Name: "Bob", Place: 2, Result: Result{Score: Score{Score: 210000, Worst: 9, Completion: 43.2}, Failed: true}},
		},
	}
	got := formatRound(rec)
	for _, want := range []string{`2026-11-01 14:03:22 | room "miku" | Roshin Yukai (Hard) | pv=54`, "#1 Alice", "Excellent", "#2 Bob", "FAILED", "43.2%"} {
		if !strings.Contains(got, want) {
			t.Errorf("log entry lacks %q:\n%s", want, got)
		}
	}
	if strings.Index(got, "Alice") > strings.Index(got, "Bob") {
		t.Errorf("ranking out of order:\n%s", got)
	}
}

// A finished round, with a failed player, ends up in the play log file.
func TestPlayLogWritten(t *testing.T) {
	path := t.TempDir() + "/plays.txt"
	url, hub := startServerHub(t)
	hub.plays = newPlayLog(path)

	host := connect(t, url, "Host")
	guest := connect(t, url, "Guest")
	joinInOrder(host, guest, "r")
	guest.send("ready", ReadyMsg{Ready: true})
	host.expectRoom(func(r RoomMsg) bool { return r.Players[1].Ready })
	host.send("start", validPick(54))
	host.expect("start", nil)
	host.send("finish", FinishMsg{Result: Result{Score: Score{Score: 5000}, Grade: 3, MaxCombo: 20}})
	guest.send("finish", FinishMsg{Result: Result{Score: Score{Score: 100}, Failed: true}})
	var res ResultsMsg
	host.expect("results", &res)
	hub.plays.Close()

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	got := string(data)
	for _, want := range []string{`room "r"`, "Roshin Yukai", "#1 Host", "Great", "#2 Guest", "FAILED"} {
		if !strings.Contains(got, want) {
			t.Errorf("play log lacks %q:\n%s", want, got)
		}
	}
}
