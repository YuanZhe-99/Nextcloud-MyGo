// Command test-soak is the soak's interface: it connects the interface's own
// pipe client to the soak daemon and disconnects again, in cycles, for the
// whole run (tools/test-soak.ps1), printing its running totals as one JSON line
// after every cycle.
// Inputs: -profile and -executable name the daemon's pipe and its process
// image; -for is how long to keep cycling.
// Side effects: none beyond its pipe connections; it never starts a daemon.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"sync/atomic"
	"time"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
)

// summary counts what the cycles saw.
type summary struct {
	Cycles      int   `json:"cycles"`
	Connected   int   `json:"connected"`
	Unavailable int   `json:"unavailable"`
	Diagnostics int   `json:"diagnostics"`
	Events      int64 `json:"events"`
}

// main runs connect, hold and disconnect cycles until the run ends.
func main() {
	profile := flag.String("profile", "", "the daemon's pipe profile")
	executable := flag.String("executable", "", "the daemon's process image")
	duration := flag.Duration("for", time.Hour, "how long to cycle")
	flag.Parse()
	if *profile == "" || *executable == "" {
		fmt.Fprintln(os.Stderr, "test-soak: -profile and -executable are required")
		os.Exit(2)
	}
	var s summary
	for end := time.Now().Add(*duration); time.Now().Before(end); {
		s.cycle(*profile, *executable)
		s.Cycles++
		// The running totals after every cycle: a stopped run keeps its last line.
		out, _ := json.Marshal(s)
		fmt.Println(string(out))
		time.Sleep(3 * time.Second)
	}
}

// cycle opens one connection like a window does, waits for the daemon's two
// accounts, stays a short or a long while (every fourth cycle 30 s), reads
// diagnostics and closes again; background sync must not notice.
func (s *summary) cycle(profile, executable string) {
	ctx, cancel := context.WithTimeout(context.Background(), 75*time.Second)
	defer cancel()
	r := syncd.ConnectRemote(ctx, profile, executable)
	defer r.Close()
	var events atomic.Int64
	defer r.Subscribe(func() { events.Add(1) })()
	ready := false
	for wait := time.Now().Add(20 * time.Second); time.Now().Before(wait); time.Sleep(100 * time.Millisecond) {
		if snap := r.Snapshot(); snap.Daemon.Connected && len(snap.Accounts) == 2 {
			ready = true
			break
		}
	}
	if !ready {
		// The daemon's restart leaves a gap with no pipe.
		s.Unavailable++
		return
	}
	s.Connected++
	stay := 2 * time.Second
	if s.Cycles%4 == 3 {
		stay = 30 * time.Second
	}
	time.Sleep(stay)
	if _, err := r.Diagnostics(ctx); err == nil {
		s.Diagnostics++
	}
	s.Events += events.Load()
}
