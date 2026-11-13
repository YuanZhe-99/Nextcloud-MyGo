package syncd

import (
	"context"
	"strconv"
	"testing"
	"time"
)

// TestRealPipeConcurrentSourceR12 bootstraps the compiled daemon while a second state source, its own crash
// backoff ending on a timer, changes the state (PLAN.md §20.4 R12): the change must reach the subscription with
// a later seq although the client never asked again, the seq a client sees never goes back, and a client
// mutation committed between two bootstraps of the same batch is followed by an event that shows it.
// Side effects: owns and removes only a uniquely named local profile; no account or server request.
func TestRealPipeConcurrentSourceR12(t *testing.T) {
	t.Parallel()
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	profile, exe, start := realDaemon(t, ctx, "ipc-race")
	crashed := start()
	first := dialRaw(t, ctx, profile, exe, true)
	first.negotiate(t)
	first.conn.Close()
	// An unclean end: the next start records a crash and activates itself after its 2-second backoff.
	if err := crashed.Process.Kill(); err != nil {
		t.Fatal(err)
	}
	_ = crashed.Wait()
	start()
	p := dialRaw(t, ctx, profile, exe, true)
	p.send(t, request("hello", "session.hello", map[string]any{"protocol": map[string]int{"major": 1, "minor": protocolMinor}}))
	if m := p.reply(t, "hello", nil); m.Error != nil {
		t.Fatalf("hello refused: %s", m.reason())
	}
	seqOf := func(s pipeState) int64 {
		n, err := strconv.ParseInt(s.Seq, 10, 64)
		if err != nil {
			t.Fatalf("seq %q is not decimal", s.Seq)
		}
		return n
	}
	p.send(t, request("b0", "state.bootstrap", map[string]any{}))
	base := decode(t, p.reply(t, "b0", nil).Result)
	if base.Snapshot.Daemon.StartupState != StartupBackoff {
		t.Fatalf("the restarted daemon was not in its crash backoff (state %q)", base.Snapshot.Daemon.StartupState)
	}
	// Bootstraps keep arriving while the daemon activates on its own; every message carries a seq that never
	// goes back, and the activation reaches the subscription after the last bootstrap that still saw backoff.
	last, lastBackoff, activated := seqOf(base), seqOf(base), false
	sent := 1
	for deadline := time.Now().Add(15 * time.Second); !activated; {
		if time.Now().After(deadline) {
			t.Fatal("the daemon's own activation never reached the subscription")
		}
		id := "b" + strconv.Itoa(sent)
		sent++
		p.send(t, request(id, "state.bootstrap", map[string]any{}))
		for {
			m, ok := p.next(2 * time.Second)
			if !ok {
				t.Fatalf("no reply %s from the real daemon", id)
			}
			raw := m.Result
			if m.Method == "event" {
				raw = m.Params
			}
			s := decode(t, raw)
			seq := seqOf(s)
			if seq < last {
				t.Fatalf("seq went back from %d to %d", last, seq)
			}
			last = seq
			normal := s.Snapshot.Daemon.StartupState == StartupNormal
			if m.Method == "event" && normal && seq > lastBackoff {
				activated = true
			}
			if m.Method != "event" && !normal {
				lastBackoff = seq
			}
			if m.ID == id {
				break
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	// A batch bootstrap, pause, bootstrap: the pause is in the second bootstrap and in an event after the first.
	p.send(t, request("c1", "state.bootstrap", map[string]any{}))
	before := decode(t, p.reply(t, "c1", nil).Result)
	p.send(t, request("c2", "state.bootstrap", map[string]any{}), pause("c3", operationKey(), before.Revision, true), request("c4", "state.bootstrap", map[string]any{}))
	var event *pipeState
	collect := func(m rpcMessage) {
		if s := decode(t, m.Params); s.Snapshot.Daemon.AllPaused {
			event = &s
		}
	}
	p.reply(t, "c2", collect)
	if m := p.reply(t, "c3", collect); m.Error != nil {
		t.Fatalf("pause refused: %s", m.reason())
	}
	after := decode(t, p.reply(t, "c4", collect).Result)
	if !after.Snapshot.Daemon.AllPaused || seqOf(after) <= seqOf(before) {
		t.Fatalf("bootstrap after the pause did not show it with a later seq (%s after %s)", after.Seq, before.Seq)
	}
	for deadline := time.Now().Add(5 * time.Second); event == nil; {
		m, ok := p.next(time.Until(deadline))
		if !ok {
			t.Fatal("the committed pause never reached the subscription")
		}
		if m.Method == "event" {
			collect(m)
		}
	}
	if seqOf(*event) <= seqOf(before) {
		t.Fatalf("pause event seq %s is not after bootstrap seq %s", event.Seq, before.Seq)
	}
}
