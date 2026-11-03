package main

import "encoding/json"

// Wire format: see docs/protocol.md.

const (
	ProtocolVersion = 1
	ServerVersion   = "2.2.1-dev.4" // release the server belongs to, shown in the client
	LaunchBlockSize = 276           // stage entry [0x2C, 0x140), see docs/re-notes.md
	MaxNameLen      = 24
	MaxRoomLen      = 32
	MaxPlayers      = 8 // per room
	MaxChatLen      = 300
	MaxTitleLen     = 64
)

type Envelope struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data,omitempty"`
}

// Client -> server

type HelloMsg struct {
	Name    string `json:"name"`
	Version int    `json:"version"`
	Steam   string `json:"steam,omitempty"`  // 64-bit Steam ID as decimal text, for avatars
	Avatar  string `json:"avatar,omitempty"` // base64 PNG/JPEG, for players without a Steam avatar
	Resume  string `json:"resume,omitempty"` // WelcomeMsg.Token of a dropped connection: take its place in the room
	// Resumable: this client reconnects and sends Resume, so its room slot is held after a dropped connection.
	// Without it (older clients) a dropped player is removed at once, as before.
	Resumable bool `json:"resumable,omitempty"`
}

type JoinMsg struct {
	Room string `json:"room"`
}

type ChatMsg struct {
	Text string `json:"text"`
}

// Win conditions a room can play by (ModeMsg, RoomMsg.Mode): who ranks first in the live leaderboard, the results and
// the session scoreboard.
const (
	ModeScore    = "score"    // the highest score wins (the default)
	ModeAccuracy = "accuracy" // the highest completion percentage wins; score breaks a tie
)

// ModeMsg: the host sets the room's win condition (lobby only).
type ModeMsg struct {
	Mode string `json:"mode"`
}

type ReadyMsg struct {
	Ready bool `json:"ready"`
}

// PlayerRefMsg names another player in the room: `host` (give them host) and `kick`, host only.
type PlayerRefMsg struct {
	ID int `json:"id"`
}

// PickMsg is also sent back to clients as the `start` payload.
type PickMsg struct {
	Block      string `json:"block"` // base64 launch block
	PV         int    `json:"pv"`
	Difficulty int    `json:"difficulty"`
	Edition    int    `json:"edition"`
	Title      string `json:"title"`
	// Force (start, host only): begin the round although some players are not ready. They get the song like the rest.
	Force bool `json:"force,omitempty"`
}

type TitleMsg struct {
	Title string `json:"title"`
}

type Score struct {
	Score      int     `json:"score"`
	Combo      int     `json:"combo"`
	Completion float64 `json:"completion"`
	Cool       int     `json:"cool"`
	Fine       int     `json:"fine"`
	Safe       int     `json:"safe"`
	Sad        int     `json:"sad"`
	Worst      int     `json:"worst"`
}

type Result struct {
	Score
	Grade    int  `json:"grade"`
	MaxCombo int  `json:"maxCombo"`
	Failed   bool `json:"failed"`
}

type FinishMsg struct {
	Result Result `json:"result"`
}

// Server -> client

// RoomsMsg is the server's open rooms, sent to players who are connected but not in a room (and again whenever a
// room changes).
type RoomsMsg struct {
	Rooms []RoomInfo `json:"rooms"`
}

type RoomInfo struct {
	Name    string `json:"name"`
	Host    string `json:"host"`
	Players int    `json:"players"`
	Playing bool   `json:"playing"`
}

type WelcomeMsg struct {
	ID      int    `json:"id"`
	Version int    `json:"version"`
	Token   string `json:"token,omitempty"`  // send as HelloMsg.Resume after a dropped connection
	Server  string `json:"server,omitempty"` // server release, e.g. "2.2.0"
}

type PickInfo struct {
	PV         int    `json:"pv"`
	Difficulty int    `json:"difficulty"`
	Edition    int    `json:"edition"`
	Title      string `json:"title"`
}

type PlayerInfo struct {
	ID      int    `json:"id"`
	Name    string `json:"name"`
	Ready   bool   `json:"ready"`
	Playing bool   `json:"playing"`
	Loaded  bool   `json:"loaded"`
	Steam   string `json:"steam,omitempty"`
	Avatar  bool   `json:"avatar,omitempty"`  // GET /avatar/<id>
	Missing bool   `json:"missing,omitempty"` // reported not having the song in focus (RoomMsg.Focus)
	Away    bool   `json:"away,omitempty"`    // connection lost; the slot is held for a short while for a resume
	Wins    int    `json:"wins,omitempty"`    // rounds won in this room since it was created (session scoreboard)
}

type RoomMsg struct {
	Name       string       `json:"name"`
	State      string       `json:"state"`
	Host       int          `json:"host"`
	Pick       *PickInfo    `json:"pick"`
	Hover      int          `json:"hover"`                // pv the host has highlighted in song select, -1 = none
	HoverTitle string       `json:"hoverTitle,omitempty"` // its title, as the host's game names it
	HoverDiff  int          `json:"hoverDifficulty"`      // the chart selected for it, 0..3, -1 = unknown
	HoverEd    int          `json:"hoverEdition"`         // 1 = the Extra chart, 0 = not, -1 = unknown
	Focus      int          `json:"focus"`                // pv players report on with `have`: the hovered one, else the pick, -1 = none
	Mode       string       `json:"mode,omitempty"`       // ModeAccuracy when the room ranks by accuracy; absent = ModeScore
	Rounds     int          `json:"rounds,omitempty"`     // rounds finished with at least two players (what Wins counts out of)
	Players    []PlayerInfo `json:"players"`
}

// HoverMsg: the host's song select highlight moved (host only, lobby only).
type HoverMsg struct {
	PV    int    `json:"pv"`
	Title string `json:"title,omitempty"` // the host's name for it, for players who don't have the song
	// The chart the host's song select has selected (left/right): Difficulty 0..3, Edition 1 for the Extra chart.
	// Absent from older clients.
	Difficulty *int `json:"difficulty,omitempty"`
	Edition    *int `json:"edition,omitempty"`
}

// HaveMsg: whether this player has song `pv` (installed, so it can be played).
type HaveMsg struct {
	PV int  `json:"pv"`
	OK bool `json:"ok"`
}

type ChatOutMsg struct {
	From   int    `json:"from"`
	Name   string `json:"name"`
	Text   string `json:"text"`
	System bool   `json:"system"`
}

type LiveScore struct {
	ID         int     `json:"id"`
	Name       string  `json:"name"`
	Score      int     `json:"score"`
	Combo      int     `json:"combo"`
	Completion float64 `json:"completion"`
	Finished   bool    `json:"finished"`
	Failed     bool    `json:"failed,omitempty"`  // finished by failing (life ran out, or quit)
	Loading    bool    `json:"loading,omitempty"` // still on the loading screen (the round hasn't been released yet)
	Lag        bool    `json:"lag,omitempty"`     // no score update for LagAfter, or the connection is lost
}

// CountdownMsg: the 3-2-1 before the round is released; `go` follows after the last number.
type CountdownMsg struct {
	N int `json:"n"`
}

type ScoresMsg struct {
	Players []LiveScore `json:"players"`
}

type FinalResult struct {
	ID     int    `json:"id"`
	Name   string `json:"name"`
	Result Result `json:"result"`
	Place  int    `json:"place"`
}

type ResultsMsg struct {
	Players []FinalResult `json:"players"`
}

// KickedMsg tells a player the host removed them; they are out of the room and may join it again.
type KickedMsg struct {
	Room string `json:"room"`
}

type ErrorMsg struct {
	Message string `json:"message"`
}

func encode(msgType string, data any) []byte {
	raw, err := json.Marshal(data)
	if err != nil {
		panic(err) // only our own types are encoded
	}
	out, _ := json.Marshal(Envelope{Type: msgType, Data: raw})
	return out
}
