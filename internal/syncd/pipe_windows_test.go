package syncd

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"golang.org/x/sys/windows"
)

// realDaemon starts the compiled daemon on a fresh profile; cleanup kills it and removes the profile.
// Side effects: one uniquely named local profile; no account, credential or server request.
func realDaemon(t *testing.T, ctx context.Context, name string) (profile, exe string, start func() *exec.Cmd) {
	t.Helper()
	exe = os.Getenv("SYNCD_TEST_EXE")
	if exe == "" {
		t.Skip("set SYNCD_TEST_EXE to the compiled native daemon")
	}
	exe, err := filepath.Abs(exe)
	if err != nil {
		t.Fatal(err)
	}
	profile = name + "-" + operationKey()
	hash := sha256.Sum256([]byte(profile))
	dir := filepath.Join(os.Getenv("LOCALAPPDATA"), "Nextcloud-MyGo", "syncd", fmt.Sprintf("%x", hash[:12]))
	t.Cleanup(func() {
		if err := removeProfileDir(dir); err != nil {
			t.Error(err)
		}
	})
	start = func() *exec.Cmd {
		cmd := exec.CommandContext(ctx, exe, "--profile", profile)
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = cmd.Process.Kill(); _ = cmd.Wait() })
		return cmd
	}
	return profile, exe, start
}

// removeProfileDir removes a test profile after its daemon exited, retrying for up to 30 s:
// a scanner such as Defender may still hold a just-closed file (seen past 10 s on hosted runners).
func removeProfileDir(dir string) error {
	var err error
	for deadline := time.Now().Add(30 * time.Second); ; time.Sleep(250 * time.Millisecond) {
		if err = os.RemoveAll(dir); err == nil || time.Now().After(deadline) {
			return err
		}
	}
}

// rawPipe is a hostile or careless client: it writes arbitrary bytes and reads whole frames only if asked.
type rawPipe struct {
	conn   io.ReadWriteCloser
	frames chan []byte
	done   chan struct{}
}

// dialRaw connects to the profile's real pipe, verifying its server like the product client, within 15 s.
// Inputs: read starts a reader that collects frames; without it nothing is ever read (a stalled client).
func dialRaw(t *testing.T, ctx context.Context, profile, exe string, read bool) *rawPipe {
	t.Helper()
	deadline := time.Now().Add(15 * time.Second)
	for {
		conn, err := dialPipe(ctx, profile, exe)
		if err == nil {
			p := &rawPipe{conn: conn, frames: make(chan []byte, 1024), done: make(chan struct{})}
			t.Cleanup(func() { conn.Close() })
			if !read {
				return p
			}
			go func() {
				defer close(p.done)
				for {
					b, err := readFrame(conn)
					if err != nil {
						return
					}
					select {
					case p.frames <- b:
					default:
						return
					}
				}
			}()
			return p
		}
		if time.Now().After(deadline) {
			t.Fatalf("real pipe not reachable: %v", err)
		}
		time.Sleep(50 * time.Millisecond)
	}
}

// frame encodes one length-prefixed message from raw JSON bytes.
func frame(body []byte) []byte {
	b := make([]byte, 4+len(body))
	binary.LittleEndian.PutUint32(b, uint32(len(body)))
	copy(b[4:], body)
	return b
}

// request encodes one JSON-RPC request frame.
func request(id, method string, params any) []byte {
	body, err := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": id, "method": method, "params": params})
	if err != nil {
		panic(err)
	}
	return frame(body)
}

// send writes bytes as one write, so several frames arrive stuck together.
func (p *rawPipe) send(t *testing.T, parts ...[]byte) {
	t.Helper()
	var all []byte
	for _, part := range parts {
		all = append(all, part...)
	}
	if _, err := p.conn.Write(all); err != nil {
		t.Fatalf("raw pipe write: %v", err)
	}
}

// rpcMessage is a decoded reply or event of the raw client.
type rpcMessage struct {
	ID     string          `json:"id"`
	Method string          `json:"method"`
	Result json.RawMessage `json:"result"`
	Params json.RawMessage `json:"params"`
	Error  *struct {
		Data struct {
			ReasonCode string `json:"reasonCode"`
		} `json:"data"`
	} `json:"error"`
}

// reason is the reply's stable reason code, or empty for a result.
func (m rpcMessage) reason() string {
	if m.Error == nil {
		return ""
	}
	return m.Error.Data.ReasonCode
}

// next returns the next frame within the timeout; ok is false when none came or the daemon closed the pipe.
func (p *rawPipe) next(timeout time.Duration) (m rpcMessage, ok bool) {
	select {
	case b := <-p.frames:
		return m, json.Unmarshal(b, &m) == nil
	case <-p.done:
		select {
		case b := <-p.frames:
			return m, json.Unmarshal(b, &m) == nil
		default:
			return m, false
		}
	case <-time.After(timeout):
		return m, false
	}
}

// reply waits for the reply with this ID; events seen on the way are passed to event.
func (p *rawPipe) reply(t *testing.T, id string, event func(rpcMessage)) rpcMessage {
	t.Helper()
	for {
		m, ok := p.next(10 * time.Second)
		if !ok {
			t.Fatalf("no reply %s from the real daemon", id)
		}
		if m.Method == "event" {
			if event != nil {
				event(m)
			}
			continue
		}
		if m.ID == id {
			return m
		}
	}
}

// closed reports whether the daemon ended the connection within the timeout.
func (p *rawPipe) closed(timeout time.Duration) bool {
	select {
	case <-p.done:
		return true
	case <-time.After(timeout):
		return false
	}
}

// pipeState is the part of a projection these tests check.
type pipeState struct {
	Seq      string `json:"seq"`
	Revision string `json:"revision"`
	Snapshot struct {
		Daemon struct {
			AllPaused    bool         `json:"allPaused"`
			StartupState StartupState `json:"startupState"`
		} `json:"daemon"`
	} `json:"snapshot"`
}

// decode reads a projection from a bootstrap result or an event's params.
func decode(t *testing.T, raw json.RawMessage) pipeState {
	t.Helper()
	var s pipeState
	if err := json.Unmarshal(raw, &s); err != nil {
		t.Fatalf("projection: %v", err)
	}
	return s
}

// negotiate says hello and bootstraps until the daemon finished its startup (stateAvailable).
func (p *rawPipe) negotiate(t *testing.T) pipeState {
	t.Helper()
	p.send(t, request("hello", "session.hello", map[string]any{"protocol": map[string]int{"major": 1, "minor": protocolMinor}}))
	if m := p.reply(t, "hello", nil); m.Error != nil {
		t.Fatalf("hello refused: %s", m.reason())
	}
	deadline := time.Now().Add(15 * time.Second)
	for i := 0; ; i++ {
		id := fmt.Sprint("bootstrap-", i)
		p.send(t, request(id, "state.bootstrap", map[string]any{}))
		s := decode(t, p.reply(t, id, nil).Result)
		if s.Snapshot.Daemon.StartupState == StartupNormal {
			return s
		}
		if time.Now().After(deadline) {
			t.Fatalf("daemon stayed in startup state %q", s.Snapshot.Daemon.StartupState)
		}
		time.Sleep(50 * time.Millisecond)
	}
}

// pause builds a settings.update request with an explicit key and expected revision.
func pause(id, key, revision string, paused bool) []byte {
	return request(id, "settings.update", map[string]any{"operationKey": key, "expectedRevision": revision, "allPaused": paused})
}

// TestRealPipeFramingR08R12 sends fragmented, stuck-together and malformed frames to the compiled daemon
// (PLAN.md §20.4 R08) and checks a state change committed right after bootstrap reaches the subscription (R12).
// Every malformed frame must end only that connection: the next client is served, and a half frame that
// never completes is dropped by the 15-second stall deadline instead of holding the only pipe instance.
func TestRealPipeFramingR08R12(t *testing.T) {
	t.Parallel()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	profile, exe, start := realDaemon(t, ctx, "ipc-framing")
	daemon := start()

	p := dialRaw(t, ctx, profile, exe, true)
	hello := request("1", "session.hello", map[string]any{"protocol": map[string]int{"major": 1, "minor": protocolMinor}})
	// A frame split inside its length prefix and its body is one message.
	for _, part := range [][]byte{hello[:3], hello[3:9], hello[9:]} {
		p.send(t, part)
		time.Sleep(30 * time.Millisecond)
	}
	if m := p.reply(t, "1", nil); m.Error != nil {
		t.Fatalf("fragmented hello refused: %s", m.reason())
	}
	state := p.negotiate(t)
	// Two frames in one write are two messages, answered in order.
	p.send(t, request("2", "diagnostics.snapshot", map[string]any{}), request("3", "state.bootstrap", map[string]any{}))
	if m := p.reply(t, "2", nil); m.Error != nil {
		t.Fatalf("stuck-together first frame refused: %s", m.reason())
	}
	state = decode(t, p.reply(t, "3", nil).Result)
	// R12: the change committed after this bootstrap arrives as an event with a later seq.
	p.send(t, pause("4", operationKey(), state.Revision, true))
	var event *pipeState
	if m := p.reply(t, "4", func(m rpcMessage) { s := decode(t, m.Params); event = &s }); m.Error != nil {
		t.Fatalf("pause refused: %s", m.reason())
	}
	for deadline := time.Now().Add(5 * time.Second); event == nil || !event.Snapshot.Daemon.AllPaused; {
		m, ok := p.next(time.Until(deadline))
		if !ok {
			t.Fatal("the committed pause never reached the subscription")
		}
		if m.Method == "event" {
			s := decode(t, m.Params)
			event = &s
		}
	}
	if seq, base := event.Seq, state.Seq; len(seq) < len(base) || (len(seq) == len(base) && seq <= base) {
		t.Fatalf("event seq %s is not after bootstrap seq %s", seq, base)
	}
	p.conn.Close()

	valid := []byte(`{"jsonrpc":"2.0","id":"1","method":"session.hello","params":{"protocol":{"major":1}}}`)
	invalid := append([]byte(nil), valid...)
	invalid[strings.Index(string(invalid), "session")] = 0xff
	oversized := make([]byte, 4)
	binary.LittleEndian.PutUint32(oversized, maxFrame+1)
	cases := []struct {
		name string
		raw  []byte
	}{
		{"zero length", make([]byte, 4)},
		{"length above 1 MiB", oversized},
		{"invalid UTF-8", frame(invalid)},
		{"duplicate key", frame([]byte(`{"jsonrpc":"2.0","jsonrpc":"2.0","id":"1","method":"session.hello","params":{"protocol":{"major":1}}}`))},
		{"truncated JSON", frame(valid[:len(valid)-2])},
		{"nesting deeper than 64", frame([]byte(`{"jsonrpc":"2.0","id":"1","method":"session.hello","params":` + strings.Repeat("[", 70) + strings.Repeat("]", 70) + `}`))},
	}
	for _, c := range cases {
		p := dialRaw(t, ctx, profile, exe, true)
		p.send(t, c.raw)
		if m, ok := p.next(5 * time.Second); ok || !p.closed(5*time.Second) {
			t.Fatalf("%s: connection not ended (reply %+v)", c.name, m)
		}
		// The daemon itself keeps serving and keeps its state.
		next := dialRaw(t, ctx, profile, exe, true)
		if s := next.negotiate(t); !s.Snapshot.Daemon.AllPaused {
			t.Fatalf("%s: daemon state lost", c.name)
		}
		next.conn.Close()
	}

	// A half frame that never completes holds the only instance at most until the stall deadline.
	half := dialRaw(t, ctx, profile, exe, true)
	started := time.Now()
	half.send(t, hello[:10])
	if !half.closed(25 * time.Second) {
		t.Fatal("a never-completed frame held the pipe past its stall deadline")
	}
	t.Logf("half frame dropped after %.1f s", time.Since(started).Seconds())
	dialRaw(t, ctx, profile, exe, true).negotiate(t)
	if daemon.ProcessState != nil {
		t.Fatal("daemon exited during malformed input")
	}
}

// TestRealPipeReplayR10R11R14 checks protocol negotiation and operation keys on the compiled daemon:
// a different major is refused and executes nothing (R14), a mutation whose reply was lost is never executed
// again, even after a later opposite change and a daemon restart (R10), and the same key with other input
// is a conflict that leaves the state unchanged (R11).
func TestRealPipeReplayR10R11R14(t *testing.T) {
	t.Parallel()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	profile, exe, start := realDaemon(t, ctx, "ipc-replay")
	daemon := start()

	p := dialRaw(t, ctx, profile, exe, true)
	p.send(t, request("1", "session.hello", map[string]any{"protocol": map[string]int{"major": 2, "minor": 0}}))
	if m := p.reply(t, "1", nil); m.reason() != "PROTOCOL_MISMATCH" {
		t.Fatalf("major 2 accepted: %+v", m)
	}
	p.send(t, pause("2", operationKey(), "0", true), request("3", "state.bootstrap", map[string]any{}))
	for _, id := range []string{"2", "3"} {
		if m := p.reply(t, id, nil); m.reason() != "PROTOCOL_MISMATCH" {
			t.Fatalf("request %s served without a negotiated major: %+v", id, m)
		}
	}
	state := p.negotiate(t)
	if state.Snapshot.Daemon.AllPaused {
		t.Fatal("a mutation sent before negotiation was executed")
	}

	// R10: the reply of a committed pause is never read.
	key := operationKey()
	lost := pause("lost", key, state.Revision, true)
	p.send(t, lost)
	time.Sleep(500 * time.Millisecond)
	p.conn.Close()
	p = dialRaw(t, ctx, profile, exe, true)
	after := p.negotiate(t)
	if !after.Snapshot.Daemon.AllPaused || after.Revision == state.Revision {
		t.Fatal("the pause whose reply was lost did not commit")
	}
	replay := func(p *rawPipe, id string) string {
		p.send(t, pause(id, key, state.Revision, true))
		m := p.reply(t, id, nil)
		var receipt operation
		if m.Error != nil || json.Unmarshal(m.Result, &receipt) != nil || receipt.State != "succeeded" || receipt.ID == "" {
			t.Fatalf("retry with the same key was not answered from its receipt: %+v", m)
		}
		return receipt.ID
	}
	first := replay(p, "retry")
	p.send(t, pause("resume", operationKey(), after.Revision, false))
	if m := p.reply(t, "resume", nil); m.Error != nil {
		t.Fatalf("resume refused: %s", m.reason())
	}
	resumed := p.negotiate(t)
	if again := replay(p, "retry-2"); again != first {
		t.Fatalf("same key gave receipt %s, then %s", first, again)
	}
	if s := p.negotiate(t); s.Snapshot.Daemon.AllPaused || s.Revision != resumed.Revision {
		t.Fatal("a replayed key executed its pause again")
	}

	// R11: the same key with different input.
	p.send(t, pause("conflict", key, resumed.Revision, false))
	if m := p.reply(t, "conflict", nil); m.reason() != "OPERATION_KEY_CONFLICT" {
		t.Fatalf("reused key with other input not refused: %+v", m)
	}
	if s := p.negotiate(t); s.Snapshot.Daemon.AllPaused || s.Revision != resumed.Revision {
		t.Fatal("a key conflict changed the state")
	}

	// The receipt survives an explicit stop and a new boot.
	p.send(t, request("stop", "daemon.shutdown", map[string]any{"operationKey": operationKey(), "reason": "user"}))
	var stop operation
	if m := p.reply(t, "stop", nil); m.Error != nil || json.Unmarshal(m.Result, &stop) != nil {
		t.Fatalf("shutdown refused: %+v", m)
	}
	// Reading the stop's receipt lets it end the process; the daemon may already be closing the pipe.
	_, _ = p.conn.Write(request("stop-status", "operations.get", map[string]any{"operationId": stop.ID}))
	if !p.closed(15 * time.Second) {
		t.Fatal("explicit stop did not end the connection")
	}
	if err := daemon.Wait(); err != nil {
		t.Fatalf("explicit stop: %v", err)
	}
	start()
	p = dialRaw(t, ctx, profile, exe, true)
	if s := p.negotiate(t); s.Snapshot.Daemon.AllPaused {
		t.Fatal("restart lost the resume")
	}
	if again := replay(p, "retry-3"); again != first {
		t.Fatalf("receipt changed across restart: %s, then %s", first, again)
	}
	if s := p.negotiate(t); s.Snapshot.Daemon.AllPaused {
		t.Fatal("a replayed key executed its pause after restart")
	}
}

// TestRealNewerControlSchemaR17 gives the compiled daemon a Control database that a newer SyncD has
// migrated (PLAN.md §20.4 R17): it must refuse to serve that state and leave the database byte for byte.
func TestRealNewerControlSchemaR17(t *testing.T) {
	t.Parallel()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	profile, exe, start := realDaemon(t, ctx, "ipc-newer-schema")
	daemon := start()
	r := ConnectRemote(ctx, profile, exe)
	awaitConnected(t, r, func(s Snapshot) bool { return s.Daemon.StartupState == StartupNormal })
	if err := r.PauseAll(); err != nil {
		t.Fatal(err)
	}
	if err := r.Shutdown(); err != nil {
		t.Fatal(err)
	}
	r.Close()
	if err := daemon.Wait(); err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256([]byte(profile))
	database := filepath.Join(os.Getenv("LOCALAPPDATA"), "Nextcloud-MyGo", "syncd", fmt.Sprintf("%x", hash[:12]), "control.sqlite")
	if info, err := os.Stat(database + "-wal"); err == nil && info.Size() != 0 {
		t.Fatal("a clean stop left Control pages in the write-ahead log")
	}
	b, err := os.ReadFile(database)
	if err != nil || len(b) < 100 || string(b[:16]) != "SQLite format 3\x00" {
		t.Fatalf("Control database unreadable: %v", err)
	}
	// The SQLite header keeps PRAGMA user_version, the Control schema, big-endian at offset 60.
	binary.BigEndian.PutUint32(b[60:], binary.BigEndian.Uint32(b[60:])+1)
	if err := os.WriteFile(database, b, 0o600); err != nil {
		t.Fatal(err)
	}
	newer := start()
	r = ConnectRemote(ctx, profile, exe)
	t.Cleanup(r.Close)
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.StartupState == StartupDatabaseUnavailable })
	if err := r.ResumeAll(); !errors.Is(err, ErrStateUnavailable) {
		t.Fatalf("a newer Control schema accepted a mutation: %v", err)
	}
	if err := r.Shutdown(); err != nil {
		t.Fatal(err)
	}
	r.Close()
	_ = newer.Wait()
	if after, err := os.ReadFile(database); err != nil || string(after) != string(b) {
		t.Fatalf("the newer Control database was changed: %v", err)
	}
}

// TestRealPipeSlowClientR13 floods the compiled daemon with requests and never reads a reply (R13): the daemon
// must end that connection on its bounded output queue or stall deadline, keep serving the next client at once,
// and answer events.resume with RESYNC_REQUIRED so a client re-bootstraps instead of replaying a lost window.
func TestRealPipeSlowClientR13(t *testing.T) {
	t.Parallel()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	profile, exe, start := realDaemon(t, ctx, "ipc-slow")
	daemon := start()
	ready := dialRaw(t, ctx, profile, exe, true)
	ready.negotiate(t)
	ready.conn.Close()

	slow := dialRaw(t, ctx, profile, exe, false)
	slow.send(t, request("hello", "session.hello", map[string]any{"protocol": map[string]int{"major": 1}}))
	started := time.Now()
	sent, dropped := 0, false
	for time.Since(started) < 40*time.Second {
		batch := make([]byte, 0, 64*100)
		for i := 0; i < 64; i++ {
			batch = append(batch, request(fmt.Sprint(sent+i), "state.bootstrap", map[string]any{})...)
		}
		if _, err := slow.conn.Write(batch); err != nil {
			dropped = true
			break
		}
		sent += 64
	}
	if !dropped {
		t.Fatalf("a client that never reads kept its connection for 40 s (%d requests)", sent)
	}
	t.Logf("non-reading client dropped after %d requests, %.1f s", sent, time.Since(started).Seconds())
	reconnected := time.Now()
	p := dialRaw(t, ctx, profile, exe, true)
	if wait := time.Since(reconnected); wait > time.Second {
		t.Fatalf("next client accepted only after %.1f s", wait.Seconds())
	}
	p.negotiate(t)
	if wait := time.Since(reconnected); wait > 5*time.Second {
		t.Fatalf("next client served only after %.1f s", wait.Seconds())
	}
	p.send(t, request("resume", "events.resume", map[string]any{"bootId": "old", "seq": "1"}))
	if m := p.reply(t, "resume", nil); m.reason() != "RESYNC_REQUIRED" {
		t.Fatalf("an expired event window was not answered with RESYNC_REQUIRED: %+v", m)
	}
	if daemon.ProcessState != nil {
		t.Fatal("daemon exited under a slow client")
	}
}

// TestRealPipeVanishingClientR13 checks that a client ending at any point frees the only pipe instance at
// once (R13): one that closes right after connecting, before the daemon's first read, and one that closes
// right after a request, before its reply is written. The daemon's read or write then fails at the call; it
// used to wait forever for that never-started I/O, so every later client saw "All pipe instances are busy".
// Notes: the daemon polls its pipe every 10 ms and these clients close within microseconds, so they land in
// those windows on a fast or a slow machine. The daemon used to never recover, so the generous 5 s bound
// keeps the verdict independent of machine speed; the slowest accept is logged.
func TestRealPipeVanishingClientR13(t *testing.T) {
	t.Parallel()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	profile, exe, start := realDaemon(t, ctx, "ipc-vanish")
	daemon := start()
	ready := dialRaw(t, ctx, profile, exe, true)
	ready.negotiate(t)
	ready.conn.Close()

	hello := request("hello", "session.hello", map[string]any{"protocol": map[string]int{"major": 1, "minor": protocolMinor}})
	var slowest time.Duration
	for i := 0; i < 40; i++ {
		began := time.Now()
		conn, err := dialPipe(ctx, profile, exe)
		for err != nil && time.Since(began) < 5*time.Second {
			time.Sleep(time.Millisecond)
			conn, err = dialPipe(ctx, profile, exe)
		}
		if err != nil {
			t.Fatalf("client %d not accepted within 5 s after the previous one ended: %v", i, err)
		}
		slowest = max(slowest, time.Since(began))
		if i%2 == 1 {
			_, _ = conn.Write(hello)
		}
		conn.Close()
	}
	t.Logf("slowest accept after a vanished client: %v", slowest)
	dialRaw(t, ctx, profile, exe, true).negotiate(t)
	if daemon.ProcessState != nil {
		t.Fatal("daemon exited after vanishing clients")
	}
}

// TestRealPipeImpostorR09 checks the pipe's security boundary on one machine and session (R09): a process
// that takes the pipe name first is refused by the client and keeps the daemon from serving under that name,
// and a connection through the network redirector (\\127.0.0.1\pipe\...) is rejected as remote.
// Cross-user and cross-session connections need a second Windows account or session and are not run here.
func TestRealPipeImpostorR09(t *testing.T) {
	t.Parallel()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	profile, exe, start := realDaemon(t, ctx, "ipc-impostor")
	name, err := pipeName(profile)
	if err != nil {
		t.Fatal(err)
	}
	path, err := windows.UTF16PtrFromString(name)
	if err != nil {
		t.Fatal(err)
	}
	const firstInstance = 0x00080000 // FILE_FLAG_FIRST_PIPE_INSTANCE
	squatter, err := windows.CreateNamedPipe(path, windows.PIPE_ACCESS_DUPLEX|firstInstance, windows.PIPE_TYPE_BYTE, 1, 4096, 4096, 0, nil)
	if err != nil {
		t.Fatalf("impostor pipe: %v", err)
	}
	defer windows.CloseHandle(squatter)
	if _, err := dialPipe(ctx, profile, exe); !errors.Is(err, ErrRefused) {
		t.Fatalf("client accepted a pipe served by another executable: %v", err)
	}
	daemon := start()
	exited := make(chan error, 1)
	go func() { exited <- daemon.Wait() }()
	select {
	case err := <-exited:
		if err == nil {
			t.Fatal("daemon reported success while another process held its pipe name")
		}
	case <-time.After(15 * time.Second):
		t.Fatal("daemon kept running without its pipe name")
	}
	windows.CloseHandle(squatter)

	start()
	local := dialRaw(t, ctx, profile, exe, true)
	local.negotiate(t)
	local.conn.Close()
	// The same name through the SMB loopback redirector is a remote client. The daemon serves one
	// client at a time, so wait while the instance freed above is still busy.
	remote, _ := windows.UTF16PtrFromString(strings.Replace(name, `\\.\`, `\\127.0.0.1\`, 1))
	for deadline := time.Now().Add(10 * time.Second); ; time.Sleep(100 * time.Millisecond) {
		h, err := windows.CreateFile(remote, windows.GENERIC_READ|windows.GENERIC_WRITE, 0, nil, windows.OPEN_EXISTING, 0, 0)
		if err == nil {
			windows.CloseHandle(h)
			t.Fatal("remote pipe client accepted")
		}
		if !errors.Is(err, windows.ERROR_PIPE_BUSY) {
			t.Logf("remote client refused: %v", err)
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("pipe instance stayed busy after the local client closed")
		}
	}
	// The refused remote attempt left the instance to local clients.
	dialRaw(t, ctx, profile, exe, true).negotiate(t)
}
