package main

import (
	"encoding/base64"
	"fmt"
	"log"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

const (
	StateLobby   = "lobby"
	StatePlaying = "playing"
)

type member struct {
	c        *Client
	ready    bool
	playing  bool // part of the current round
	finished bool
	loaded   bool // reached the loading barrier this round
	live     Score
	result   *Result
	havePV   int // last `have` report: song id and whether this player has it (-1 = none yet)
	haveOK   bool
	away     bool        // connection lost; the slot is held until `expiry` fires or the player resumes
	expiry   *time.Timer // set while away
}

// Room holds one lobby. Every method takes the room lock; Client.Send never blocks,
// so broadcasting under the lock is safe.
type Room struct {
	hub  *Hub
	key  string
	name string

	mu         sync.Mutex
	closed     bool
	members    []*member // join order, used for host succession
	state      string
	host       int
	pick       *PickMsg
	stopTick   chan struct{}
	round      int  // increments every round, so timers from old rounds do nothing
	goSent     bool // the loading barrier was released this round
	hover      int  // pv the host has highlighted, -1 = none (lobby only)
	hoverTitle string
	hoverDiff  int // the chart the host has selected for it, -1 = unknown
	hoverEd    int

	lastResults []FinalResult // the previous round's ranking and what was played, for the spectator page
	lastPick    *PickInfo
}

func newRoom(h *Hub, key, name string) *Room {
	return &Room{hub: h, key: key, name: name, state: StateLobby, hover: -1, hoverDiff: -1, hoverEd: -1}
}

func (r *Room) Empty() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.members) == 0
}

func (r *Room) Close() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.closed = true
	r.stopTicker()
}

func (r *Room) find(c *Client) *member {
	for _, m := range r.members {
		if m.c == c {
			return m
		}
	}
	return nil
}

func (r *Room) findID(id int) *member {
	for _, m := range r.members {
		if m.c.id == id {
			return m
		}
	}
	return nil
}

func (r *Room) broadcast(msg []byte) {
	for _, m := range r.members {
		m.c.Send(msg)
	}
}

func (r *Room) system(format string, args ...any) {
	r.broadcast(encode("chat", ChatOutMsg{Text: fmt.Sprintf(format, args...), System: true}))
}

// focus is the song players report having or not: the host's highlighted song in the lobby, else the pick.
func (r *Room) focus() int {
	if r.state == StateLobby && r.hover >= 0 {
		return r.hover
	}
	if r.pick != nil {
		return r.pick.PV
	}
	return -1
}

// lacks reports whether m said they don't have song pv.
func (m *member) lacks(pv int) bool { return pv >= 0 && m.havePV == pv && !m.haveOK }

func (r *Room) snapshot() RoomMsg {
	focus := r.focus()
	msg := RoomMsg{Name: r.name, State: r.state, Host: r.host, Hover: r.hover, Focus: focus, Players: []PlayerInfo{},
		HoverDiff: -1, HoverEd: -1}
	if r.hover >= 0 {
		msg.HoverTitle = r.hoverTitle
		msg.HoverDiff, msg.HoverEd = r.hoverDiff, r.hoverEd
	}
	if r.pick != nil {
		msg.Pick = &PickInfo{PV: r.pick.PV, Difficulty: r.pick.Difficulty, Edition: r.pick.Edition, Title: r.pick.Title}
	}
	for _, m := range r.members {
		msg.Players = append(msg.Players, PlayerInfo{ID: m.c.id, Name: m.c.name, Ready: m.ready, Playing: m.playing,
			Loaded: m.loaded, Steam: m.c.steam, Avatar: m.c.hasAvatar, Missing: m.lacks(focus), Away: m.away})
	}
	return msg
}

func (r *Room) broadcastState() {
	r.broadcast(encode("room", r.snapshot()))
	r.hub.roomsChanged() // players, host or state changed: the room list shows them
}

// Join adds c. If the room was closed in the meantime, c gets a fresh room of the same name.
func (r *Room) Join(c *Client) {
	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		c.room = r.hub.Room(r.name)
		c.room.Join(c)
		return
	}
	defer r.mu.Unlock()

	if len(r.members) >= MaxPlayers {
		c.room = nil
		c.SendError(fmt.Sprintf("room %q is full (%d players)", r.name, MaxPlayers))
		r.hub.enterLobby(c) // back to the room list
		return
	}
	// A same-name player whose connection dropped is most likely this one without a resume token (older client):
	// free the name instead of numbering around them.
	for _, m := range append([]*member(nil), r.members...) {
		if m.away && strings.EqualFold(m.c.baseName, c.baseName) {
			r.remove(m.c, "timed out")
			r.hub.setAvatar(m.c.id, nil)
			r.hub.dropSession(m.c)
		}
	}
	c.name = r.uniqueName(c.baseName)
	r.members = append(r.members, &member{c: c, havePV: -1})
	if len(r.members) == 1 {
		r.host = c.id
	}
	log.Printf("room %q: %s joined (%d players)", r.name, c.name, len(r.members))
	r.system("%s joined", c.name)
	r.broadcastState()
}

// uniqueName numbers a name already used in the room (case-insensitive): "Miku", "Miku (2)", "Miku (3)".
// The base is shortened if needed so the result stays within MaxNameLen. Caller holds r.mu.
func (r *Room) uniqueName(base string) string {
	taken := map[string]bool{}
	for _, m := range r.members {
		taken[strings.ToLower(m.c.name)] = true
	}
	if !taken[strings.ToLower(base)] {
		return base
	}
	runes := []rune(base)
	for n := 2; ; n++ {
		suffix := fmt.Sprintf(" (%d)", n)
		keep := min(len(runes), MaxNameLen-utf8.RuneCountInString(suffix))
		name := strings.TrimSpace(string(runes[:keep])) + suffix
		if !taken[strings.ToLower(name)] {
			return name
		}
	}
}

// Detach keeps c's slot for a resume after its connection dropped, and gives up the host role if it had it. It
// reports false if c isn't a member (nothing to hold). After hub.resumeGrace the slot is freed as if c had left.
func (r *Room) Detach(c *Client) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	m := r.find(c)
	if m == nil || r.closed {
		return false
	}
	m.away = true
	log.Printf("room %q: %s lost connection (%d players)", r.name, c.name, len(r.members))
	r.system("%s lost connection", c.name)
	if r.host == c.id {
		r.passHost(c)
	}
	m.expiry = time.AfterFunc(r.hub.grace(), func() { r.expire(m) })
	r.broadcastState()
	return true
}

// passHost gives the host role to the first connected member other than `from` (nobody if they are all away).
func (r *Room) passHost(from *Client) {
	for _, m := range r.members {
		if m.c != from && !m.away {
			r.host = m.c.id
			r.hover = -1 // the new host's game reports its own highlight
			r.system("%s is now the host", m.c.name)
			return
		}
	}
}

// expire frees the slot of a player who didn't come back.
func (r *Room) expire(m *member) {
	r.mu.Lock()
	if !m.away || r.find(m.c) != m {
		r.mu.Unlock()
		return // resumed, or already removed
	}
	c := m.c
	removed := r.remove(c, "timed out")
	empty := len(r.members) == 0
	r.mu.Unlock()

	r.hub.setAvatar(c.id, nil)
	r.hub.dropSession(c)
	if removed && empty {
		r.hub.removeIfEmpty(r)
	}
}

// Resume puts new connection c in the place of old, a member who dropped (or whose connection is half dead): same id
// and name, round state kept. It sends c its welcome and the room. False if old isn't a member of this room.
func (r *Room) Resume(old, c *Client) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	m := r.find(old)
	if m == nil || r.closed {
		return false
	}
	if m.expiry != nil {
		m.expiry.Stop()
		m.expiry = nil
	}
	m.away = false
	m.c = c
	c.id, c.name, c.baseName = old.id, old.name, old.baseName
	old.superseded.Store(true)
	go old.conn.CloseNow()

	log.Printf("room %q: %s resumed (%d players)", r.name, c.name, len(r.members))
	c.Send(encode("welcome", WelcomeMsg{ID: c.id, Version: ProtocolVersion, Token: old.token, Server: ServerVersion}))
	r.system("%s reconnected", c.name)
	r.broadcastState()
	if r.state == StatePlaying && r.goSent && m.playing && !m.finished {
		c.Send(encode("go", struct{}{})) // the barrier was released while they were away
	}
	return true
}

func (r *Room) Leave(c *Client) {
	r.mu.Lock()
	removed := r.remove(c, "left")
	empty := len(r.members) == 0
	r.mu.Unlock()

	if removed && empty {
		r.hub.removeIfEmpty(r)
	}
}

// Contains reports whether c is still a member (a kick removes players from outside their own goroutine).
func (r *Room) Contains(c *Client) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.find(c) != nil
}

// remove takes c out of the room and tells the others ("left" / "was kicked"). Caller holds r.mu.
func (r *Room) remove(c *Client, how string) bool {
	idx := -1
	for i, m := range r.members {
		if m.c == c {
			idx = i
		}
	}
	if idx < 0 {
		return false
	}
	if e := r.members[idx].expiry; e != nil {
		e.Stop()
	}
	r.members = append(r.members[:idx], r.members[idx+1:]...)
	log.Printf("room %q: %s %s (%d players)", r.name, c.name, how, len(r.members))

	if len(r.members) > 0 {
		r.system("%s %s", c.name, how)
		if r.host == c.id {
			r.passHost(c)
			if r.host == c.id { // everyone left is away: still better than a host who is gone
				r.host = r.members[0].c.id
			}
		}
		if r.state == StatePlaying {
			r.checkAllLoaded()
			r.checkRoundOver()
		}
		r.broadcastState()
	}
	return true
}

func (r *Room) Handle(c *Client, env Envelope) {
	r.mu.Lock()
	defer r.mu.Unlock()

	me := r.find(c)
	if me == nil {
		c.SendError("not in this room")
		return
	}
	isHost := r.host == c.id

	switch env.Type {
	case "chat":
		m, ok := decode[ChatMsg](c, env.Data)
		if !ok {
			return
		}
		if text := cleanText(m.Text, MaxChatLen); text != "" {
			r.broadcast(encode("chat", ChatOutMsg{From: c.id, Name: c.name, Text: text}))
		}

	case "host", "kick":
		m, ok := decode[PlayerRefMsg](c, env.Data)
		if !ok {
			return
		}
		target := r.findID(m.ID)
		switch {
		case !isHost:
			c.SendError("only the host can do that")
		case target == nil || target.c == c:
			c.SendError("no such player")
		case env.Type == "host":
			r.host = target.c.id
			r.hover = -1
			r.system("%s is now the host", target.c.name)
			r.broadcastState()
		default:
			target.c.Send(encode("kicked", KickedMsg{Room: r.name}))
			r.remove(target.c, "was kicked")
			r.hub.enterLobby(target.c) // back to the room list
		}

	case "hover":
		m, ok := decode[HoverMsg](c, env.Data)
		if !ok || !isHost || r.state != StateLobby {
			return // a stale highlight from a non-host or mid-round is just dropped
		}
		if m.PV < -1 {
			m.PV = -1
		}
		title := cleanText(m.Title, MaxTitleLen)
		diff, ed := -1, -1
		if m.Difficulty != nil && *m.Difficulty >= 0 && *m.Difficulty <= 4 {
			diff, ed = *m.Difficulty, 0
			if m.Edition != nil && (*m.Edition == 0 || *m.Edition == 1) {
				ed = *m.Edition
			}
		}
		if m.PV != r.hover || title != r.hoverTitle || diff != r.hoverDiff || ed != r.hoverEd {
			r.hover, r.hoverTitle, r.hoverDiff, r.hoverEd = m.PV, title, diff, ed
			r.broadcastState()
		}

	case "have":
		m, ok := decode[HaveMsg](c, env.Data)
		if !ok {
			return
		}
		changed := me.havePV != m.PV || me.haveOK != m.OK
		me.havePV, me.haveOK = m.PV, m.OK
		// Found out mid-load that the round's song is missing: out of this round, don't hold the others.
		if r.state == StatePlaying && me.playing && r.pick != nil && me.lacks(r.pick.PV) {
			me.playing = false
			r.system("%s doesn't have this song and sits this round out", c.name)
			r.checkAllLoaded()
			r.checkRoundOver()
			changed = true
		}
		if changed {
			r.broadcastState()
		}

	case "ready":
		m, ok := decode[ReadyMsg](c, env.Data)
		if !ok {
			return
		}
		if r.state != StateLobby {
			c.SendError("round in progress")
			return
		}
		me.ready = m.Ready
		r.broadcastState()

	case "pick":
		m, ok := decode[PickMsg](c, env.Data)
		if !ok {
			return
		}
		if err := r.setPick(isHost, m); err != "" {
			c.SendError(err)
			return
		}
		r.system("%s picked %s", c.name, describe(r.pick))
		r.broadcastState()

	case "start":
		// Usually carries the launch block the host's game just started with.
		m, ok := decode[PickMsg](c, env.Data)
		if !ok {
			return
		}
		if m.Block != "" {
			if err := r.validatePick(isHost, &m); err != "" {
				c.SendError(err)
				return
			}
			r.pick = &m
		}
		if err := r.startRound(isHost); err != "" {
			c.SendError(err)
		}

	case "title":
		// The host's game only reveals the song title once the song plays.
		m, ok := decode[TitleMsg](c, env.Data)
		if !ok {
			return
		}
		if !isHost || r.pick == nil {
			return
		}
		r.pick.Title = cleanText(m.Title, MaxTitleLen)
		r.broadcastState()

	case "loaded":
		// This player's song is loaded and held on the loading screen.
		if r.state != StatePlaying || !me.playing || me.loaded {
			return
		}
		me.loaded = true
		r.broadcastState()
		r.checkAllLoaded()

	case "score":
		m, ok := decode[Score](c, env.Data)
		if !ok {
			return
		}
		if r.state == StatePlaying && me.playing && !me.finished {
			me.live = m
		}

	case "finish":
		m, ok := decode[FinishMsg](c, env.Data)
		if !ok {
			return
		}
		if r.state != StatePlaying || !me.playing || me.finished {
			return
		}
		me.finished = true
		me.result = &m.Result
		me.live = m.Result.Score
		r.checkAllLoaded() // quitting while loading must not block the others
		r.checkRoundOver()
	}
}

func (r *Room) validatePick(isHost bool, m *PickMsg) string {
	if !isHost {
		return "only the host can pick a song"
	}
	if r.state != StateLobby {
		return "round in progress"
	}
	block, err := base64.StdEncoding.DecodeString(m.Block)
	if err != nil || len(block) != LaunchBlockSize {
		return "invalid launch block"
	}
	if m.PV < 0 || m.Difficulty < 0 || m.Difficulty > 4 || m.Edition < 0 || m.Edition > 1 {
		return "invalid song"
	}
	m.Title = cleanText(m.Title, MaxTitleLen)
	return ""
}

// setPick sets a preview pick; everyone has to ready up again for the new song.
func (r *Room) setPick(isHost bool, m PickMsg) string {
	if err := r.validatePick(isHost, &m); err != "" {
		return err
	}
	r.pick = &m
	for _, mem := range r.members {
		mem.ready = false
	}
	return ""
}

func (r *Room) startRound(isHost bool) string {
	if !isHost {
		return "only the host can start"
	}
	if r.state != StateLobby {
		return "round already in progress"
	}
	if r.pick == nil {
		return "pick a song first"
	}
	// Players known not to have the song sit the round out (and aren't waited for).
	for _, m := range r.members {
		if m.c.id != r.host && !m.ready && !m.lacks(r.pick.PV) {
			return m.c.name + " is not ready"
		}
	}

	r.state = StatePlaying
	r.round++
	r.goSent = false
	r.hover = -1
	for _, m := range r.members {
		m.playing, m.finished, m.ready, m.loaded = !m.lacks(r.pick.PV), false, false, false
		m.live, m.result = Score{}, nil
	}
	log.Printf("room %q: round started, %s, %d players", r.name, describe(r.pick), len(r.members))
	r.system("Round started: %s", describe(r.pick))
	r.broadcast(encode("start", r.pick))
	r.broadcastState()
	r.startTicker()

	// Don't let one stuck player hold everyone on the loading screen forever.
	round := r.round
	time.AfterFunc(r.hub.loadTimeout, func() {
		r.mu.Lock()
		defer r.mu.Unlock()
		if r.round == round && r.state == StatePlaying && !r.goSent {
			log.Printf("room %q: loading timeout, starting without everyone", r.name)
			r.system("Not everyone finished loading, starting anyway")
			r.sendGo()
		}
	})
	return ""
}

// checkAllLoaded releases the loading barrier once every player still in the round has loaded.
func (r *Room) checkAllLoaded() {
	if r.goSent || r.state != StatePlaying {
		return
	}
	for _, m := range r.members {
		if m.playing && !m.finished && !m.loaded {
			return
		}
	}
	r.sendGo()
}

func (r *Room) sendGo() {
	r.goSent = true
	log.Printf("room %q: everyone loaded, go", r.name)
	r.broadcast(encode("go", struct{}{}))
}

func (r *Room) checkRoundOver() {
	for _, m := range r.members {
		if m.playing && !m.finished {
			return
		}
	}

	results := []FinalResult{}
	for _, m := range r.members {
		if m.playing && m.result != nil {
			results = append(results, FinalResult{ID: m.c.id, Name: m.c.name, Result: *m.result})
		}
	}
	sort.SliceStable(results, func(i, j int) bool { return results[i].Result.Score.Score > results[j].Result.Score.Score })
	for i := range results {
		results[i].Place = i + 1
	}

	r.stopTicker()
	r.state = StateLobby
	r.lastResults = append([]FinalResult(nil), results...)
	if r.pick != nil {
		r.lastPick = &PickInfo{PV: r.pick.PV, Difficulty: r.pick.Difficulty, Edition: r.pick.Edition, Title: r.pick.Title}
	}
	for _, m := range r.members {
		m.playing, m.finished = false, false
	}
	log.Printf("room %q: round over", r.name)
	if r.pick != nil {
		r.hub.plays.Add(RoundRecord{Time: time.Now(), Room: r.name, Pick: *r.pick, Results: append([]FinalResult(nil), results...)})
	}
	r.broadcast(encode("results", ResultsMsg{Players: results}))
	r.broadcastState()
}

func (r *Room) liveScores() ScoresMsg {
	out := ScoresMsg{Players: []LiveScore{}}
	for _, m := range r.members {
		if m.playing {
			out.Players = append(out.Players, LiveScore{
				ID: m.c.id, Name: m.c.name, Score: m.live.Score, Combo: m.live.Combo,
				Completion: m.live.Completion, Finished: m.finished, Failed: m.result != nil && m.result.Failed,
			})
		}
	}
	sort.SliceStable(out.Players, func(i, j int) bool { return out.Players[i].Score > out.Players[j].Score })
	return out
}

func (r *Room) startTicker() {
	stop := make(chan struct{})
	r.stopTick = stop
	go func() {
		t := time.NewTicker(r.hub.tickEvery)
		defer t.Stop()
		for {
			select {
			case <-stop:
				return
			case <-t.C:
				r.mu.Lock()
				if r.state == StatePlaying {
					r.broadcast(encode("scores", r.liveScores()))
				}
				r.mu.Unlock()
			}
		}
	}()
}

func (r *Room) stopTicker() {
	if r.stopTick != nil {
		close(r.stopTick)
		r.stopTick = nil
	}
}

var difficultyNames = []string{"Easy", "Normal", "Hard", "Extreme", "Extra Extreme"}

func describe(p *PickMsg) string {
	diff := difficultyNames[p.Difficulty]
	if p.Edition == 1 {
		diff = "Extra " + diff
	}
	title := p.Title
	if title == "" {
		title = fmt.Sprintf("PV %d", p.PV)
	}
	return fmt.Sprintf("%s (%s)", title, diff)
}
