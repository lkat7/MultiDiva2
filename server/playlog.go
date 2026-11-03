package main

import (
	"fmt"
	"log"
	"os"
	"strings"
	"sync"
	"time"
)

// PlayLog appends every finished round, with its ranking, to one text file. Rounds are queued and written by a
// single goroutine, so a slow disk never holds a room lock.
type PlayLog struct {
	ch   chan RoundRecord
	done chan struct{}
	once sync.Once
}

// RoundRecord is one finished round, copied out of the room so it can be formatted later.
type RoundRecord struct {
	Time    time.Time
	Room    string
	Mode    string // ModeScore or ModeAccuracy: what ranked the players
	Pick    PickMsg
	Results []FinalResult
}

var gradeNames = []string{"Failed", "Cheap", "Standard", "Great", "Excellent", "Perfect"}

// formatRound renders one round as a block of lines ending in a blank line.
func formatRound(rec RoundRecord) string {
	var b strings.Builder
	fmt.Fprintf(&b, "%s | room %q | %s | pv=%d", rec.Time.Format("2006-01-02 15:04:05"), rec.Room, describe(&rec.Pick), rec.Pick.PV)
	if rec.Mode == ModeAccuracy {
		b.WriteString(" | ranked by accuracy")
	}
	b.WriteString("\n")
	if len(rec.Results) == 0 {
		b.WriteString("  (no results)\n")
	}
	for _, r := range rec.Results {
		grade := "?"
		if r.Result.Grade >= 0 && r.Result.Grade < len(gradeNames) {
			grade = gradeNames[r.Result.Grade]
		}
		status := grade
		if r.Result.Failed {
			status = "FAILED"
		}
		fmt.Fprintf(&b, "  #%d %-24s %8d  %-9s combo %d  cool %d fine %d safe %d sad %d worst %d  %.1f%%\n",
			r.Place, r.Name, r.Result.Score.Score, status, r.Result.MaxCombo,
			r.Result.Cool, r.Result.Fine, r.Result.Safe, r.Result.Sad, r.Result.Worst, r.Result.Completion)
	}
	b.WriteString("\n")
	return b.String()
}

// newPlayLog opens path for appending and starts the writer. An empty path, or a file that can't be opened,
// gives nil (logging off); every method is safe on nil.
func newPlayLog(path string) *PlayLog {
	if path == "" {
		return nil
	}
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		log.Printf("play log disabled: %v", err)
		return nil
	}
	p := &PlayLog{ch: make(chan RoundRecord, 64), done: make(chan struct{})}
	go func() {
		defer close(p.done)
		defer f.Close()
		for rec := range p.ch {
			if _, err := f.WriteString(formatRound(rec)); err != nil {
				log.Printf("play log: %v", err)
			}
		}
	}()
	log.Printf("play log: %s", path)
	return p
}

// Add queues a round. It never blocks: if the writer is far behind the round is dropped.
func (p *PlayLog) Add(rec RoundRecord) {
	if p == nil {
		return
	}
	select {
	case p.ch <- rec:
	default:
		log.Printf("play log: queue full, dropping a round")
	}
}

// Close writes what is queued and closes the file.
func (p *PlayLog) Close() {
	if p == nil {
		return
	}
	p.once.Do(func() { close(p.ch) })
	<-p.done
}
