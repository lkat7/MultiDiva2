package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"log"
	"strings"
	"sync/atomic"
	"time"
	"unicode/utf8"

	"github.com/coder/websocket"
)

const (
	sendBuffer   = 64
	writeTimeout = 5 * time.Second
	pingInterval = 10 * time.Second
	readLimit    = 64 << 10 // a hello with an avatar is ~44 KB
)

// A WebSocket ping every Hub.probeInterval; no pong within Hub.probeTimeout means the peer is gone (a phone that
// dropped off the network sends no close, and the TCP connection can look healthy for minutes).

// Client is one connected player. Only its own read goroutine mutates name/room;
// rooms read them under the room lock.
type Client struct {
	id         int
	name       string // shown in the current room: baseName, numbered if someone there has it too
	baseName   string // from hello
	steam      string // Steam ID (digits only) or empty
	hasAvatar  bool   // a custom avatar is served at /avatar/<id>
	hub        *Hub
	conn       *websocket.Conn
	send       chan []byte
	room       *Room
	token      string      // identifies this session for a resume after a dropped connection
	resumable  bool        // the client said it resumes (HelloMsg.Resumable)
	superseded atomic.Bool // a newer connection took over this one's place
}

func newToken() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

// Send queues a message. A client that can't keep up is disconnected instead of
// stalling the room.
func (c *Client) Send(msg []byte) {
	select {
	case c.send <- msg:
	default:
		// Callers may hold a room lock, and Close waits for the close handshake.
		log.Printf("client %d: send buffer full, dropping connection", c.id)
		go c.conn.Close(websocket.StatusPolicyViolation, "too slow")
	}
}

func (c *Client) SendError(message string) {
	c.Send(encode("error", ErrorMsg{Message: message}))
}

func (c *Client) writeLoop(ctx context.Context) {
	// Keepalive: the game client treats 30 s without any message as a dead connection.
	ping := time.NewTicker(pingInterval)
	defer ping.Stop()
	for {
		var msg []byte
		select {
		case <-ctx.Done():
			return
		case <-ping.C:
			msg = encode("ping", struct{}{})
		case msg = <-c.send:
		}
		wctx, cancel := context.WithTimeout(ctx, writeTimeout)
		err := c.conn.Write(wctx, websocket.MessageText, msg)
		cancel()
		if err != nil {
			return
		}
	}
}

// probeLoop closes a connection whose peer stops answering pings, so the usual disconnect handling runs.
func (c *Client) probeLoop(ctx context.Context) {
	interval, timeout := c.hub.probing()
	t := time.NewTicker(interval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
		pctx, cancel := context.WithTimeout(ctx, timeout)
		err := c.conn.Ping(pctx)
		cancel()
		if err != nil && ctx.Err() == nil {
			log.Printf("no answer to ping, dropping connection")
			c.conn.CloseNow() // no close handshake: the peer isn't there
			return
		}
	}
}

// readLoop returns why the connection ended.
func (c *Client) readLoop(ctx context.Context) error {
	c.conn.SetReadLimit(readLimit)
	for {
		typ, data, err := c.conn.Read(ctx)
		if err != nil {
			return err
		}
		if typ != websocket.MessageText {
			continue
		}
		var env Envelope
		if err := json.Unmarshal(data, &env); err != nil {
			c.SendError("malformed message")
			continue
		}
		c.handle(env)
	}
}

func decode[T any](c *Client, raw json.RawMessage) (T, bool) {
	var v T
	if len(raw) > 0 {
		if err := json.Unmarshal(raw, &v); err != nil {
			c.SendError("malformed data")
			return v, false
		}
	}
	return v, true
}

// isSteamID accepts a 64-bit Steam ID written in decimal.
func isSteamID(s string) bool {
	if len(s) == 0 || len(s) > 20 {
		return false
	}
	for _, r := range s {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}

func cleanText(s string, max int) string {
	s = strings.TrimSpace(strings.ToValidUTF8(s, ""))
	s = strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, s)
	if utf8.RuneCountInString(s) > max {
		s = string([]rune(s)[:max])
	}
	return s
}

func (c *Client) handle(env Envelope) {
	if c.name == "" && env.Type != "hello" {
		c.SendError("say hello first")
		return
	}

	switch env.Type {
	case "hello":
		m, ok := decode[HelloMsg](c, env.Data)
		if !ok {
			return
		}
		if m.Version != ProtocolVersion {
			c.SendError("protocol version mismatch: update MultiDiva")
			return
		}
		name := cleanText(m.Name, MaxNameLen)
		if name == "" {
			c.SendError("name required")
			return
		}
		if c.room == nil {
			c.name = name
		}
		c.baseName = name
		c.resumable = m.Resumable
		c.steam = ""
		if isSteamID(m.Steam) {
			c.steam = m.Steam
		}
		a, ok := parseAvatar(m.Avatar)
		c.hasAvatar = ok
		// A reconnecting player takes over their old slot (same id, name and round state).
		resumed := m.Resume != "" && c.room == nil && c.hub.resume(c, m.Resume)
		if ok {
			c.hub.setAvatar(c.id, &a)
		} else {
			c.hub.setAvatar(c.id, nil)
		}
		if resumed {
			return // the welcome and the room went out with the takeover
		}
		c.Send(encode("welcome", WelcomeMsg{ID: c.id, Version: ProtocolVersion, Token: c.token, Server: ServerVersion}))
		if c.room == nil {
			c.hub.enterLobby(c)
		}

	case "join":
		m, ok := decode[JoinMsg](c, env.Data)
		if !ok {
			return
		}
		name := cleanText(m.Room, MaxRoomLen)
		if name == "" {
			c.SendError("room name required")
			return
		}
		if c.room != nil {
			c.room.Leave(c)
		}
		c.hub.leaveLobby(c)
		c.room = c.hub.Room(name)
		c.room.Join(c)

	case "leave":
		if c.room != nil {
			c.room.Leave(c)
			c.room = nil
		}
		c.hub.enterLobby(c)

	default:
		if c.room != nil && !c.room.Contains(c) {
			c.room = nil // kicked
		}
		if c.room == nil {
			c.SendError("not in a room")
			return
		}
		c.room.Handle(c, env)
	}
}
