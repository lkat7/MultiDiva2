package main

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"sync"
	"time"
)

// Steam profile pictures for the spectator page. The game clients fetch these themselves; the server never held them,
// so the page asks Steam's public profile feed (no key needed) once per player and caches the picture's address. Only
// that address goes to the page, never the Steam ID. Lookups run in the background: a player has no picture on the page
// until the first one finishes.

const (
	steamAvatarTTL    = 12 * time.Hour   // a found picture is kept this long
	steamAvatarRetry  = 10 * time.Minute // a failed lookup (private, offline, rate limited) is not retried sooner
	steamAvatarMax    = 2000             // entries kept; the cache is dropped when it grows past this
	steamAvatarParall = 4                // lookups at once
)

type steamEntry struct {
	url     string // "" = none
	at      time.Time
	pending bool
}

type steamAvatars struct {
	mu    sync.Mutex
	cache map[string]steamEntry
	slots chan struct{}
	// fetch returns the picture address for a Steam ID; replaced in tests.
	fetch func(id string) (string, error)
}

func newSteamAvatars() *steamAvatars {
	return &steamAvatars{cache: map[string]steamEntry{}, slots: make(chan struct{}, steamAvatarParall), fetch: fetchSteamAvatar}
}

// URL returns the cached picture address for a Steam ID ("" if unknown yet) and starts a lookup when there is none
// or it has expired. It never blocks.
func (s *steamAvatars) URL(id string) string {
	if !isSteamID(id) {
		return ""
	}
	s.mu.Lock()
	e, ok := s.cache[id]
	ttl := steamAvatarTTL
	if ok && e.url == "" {
		ttl = steamAvatarRetry
	}
	stale := !ok || time.Since(e.at) > ttl
	if stale && !e.pending {
		if len(s.cache) > steamAvatarMax {
			s.cache = map[string]steamEntry{}
		}
		e.pending = true
		s.cache[id] = e
		go s.lookup(id)
	}
	s.mu.Unlock()
	return e.url
}

func (s *steamAvatars) lookup(id string) {
	s.slots <- struct{}{}
	defer func() { <-s.slots }()
	addr, err := s.fetch(id)
	if err != nil {
		addr = ""
	}
	s.mu.Lock()
	s.cache[id] = steamEntry{url: addr, at: time.Now()}
	s.mu.Unlock()
}

var avatarTag = regexp.MustCompile(`<avatarMedium><!\[CDATA\[(.*?)\]\]></avatarMedium>`)

// steamAvatarURL accepts only an https address on Steam's own image hosts, whatever the feed says.
func steamAvatarURL(raw string) (string, bool) {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || u.Scheme != "https" || u.User != nil {
		return "", false
	}
	host := strings.ToLower(u.Hostname())
	if host != "steamstatic.com" && !strings.HasSuffix(host, ".steamstatic.com") {
		return "", false
	}
	return u.String(), true
}

func fetchSteamAvatar(id string) (string, error) {
	client := http.Client{Timeout: 5 * time.Second}
	resp, err := client.Get(fmt.Sprintf("https://steamcommunity.com/profiles/%s/?xml=1", id)) // id is digits only
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("steam: %s", resp.Status)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	if err != nil {
		return "", err
	}
	m := avatarTag.FindSubmatch(body)
	if m == nil {
		return "", errors.New("steam: no avatar in the profile")
	}
	addr, ok := steamAvatarURL(string(m[1]))
	if !ok {
		return "", errors.New("steam: unexpected avatar address")
	}
	return addr, nil
}
