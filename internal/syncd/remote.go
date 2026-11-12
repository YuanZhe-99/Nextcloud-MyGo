package syncd

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

// protocolMinor is the protocol 1.x minor version this client speaks
// (protocol/contract.json).
const protocolMinor = 9

// Remote projects a real SyncD connection into the existing UI Client contract.
// Side effects: reconnects and bootstraps; Close never shuts down the daemon.
type Remote struct {
	mu        sync.Mutex
	writeMu   sync.Mutex
	state     Snapshot
	revision  string
	boot      string
	seq       uint64
	conn      io.ReadWriteCloser
	pending   map[string]chan rpcReply
	listeners map[uint64]func()
	next      atomic.Uint64
	ctx       context.Context
	cancel    context.CancelFunc
	dial      func(context.Context) (io.ReadWriteCloser, error)
	// ready admits RPCs after bootstrap, including diagnostic-only sessions.
	ready bool
	// legacyState is enabled only by an explicit pre-1.7 hello response.
	legacyState bool
	bootstrapID string
	// minor is the protocol minor version the current connection negotiated.
	minor int
	// cause is why the last connection ended or could not start (Disconnect).
	cause string
}

// rpcReply holds a response or a transport failure, neither of which implies rollback.
type rpcReply struct {
	ID        string          `json:"id"`
	Result    json.RawMessage `json:"result"`
	Error     *rpcError       `json:"error"`
	transport error
}

// rpcError carries stable reason codes, with no server message shown directly to the user.
type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
	Data    struct {
		Reason string `json:"reasonCode"`
	} `json:"data"`
}

// Error returns the diagnostic reason rather than potentially sensitive remote text.
func (e *rpcError) Error() string {
	if e.Data.Reason == "STATE_UNAVAILABLE" {
		return ErrStateUnavailable.Error()
	}
	return "syncd: " + e.Data.Reason
}

// Is preserves the UI's existing localized error mapping.
func (e *rpcError) Is(target error) bool {
	switch e.Data.Reason {
	case "STATE_UNAVAILABLE":
		return target == ErrStateUnavailable
	case "APP_PASSWORD_REVOCATION_UNCONFIRMED":
		return target == ErrCredentialCleanup
	case "REVISION_CONFLICT", "OPERATION_KEY_CONFLICT":
		return target == ErrStale
	case "QUIESCE_STALLED", "PAUSED", "REMOTE_LOCKED", "SERVER_THROTTLED", "DECISION_PENDING":
		return target == ErrBusy
	case "ACCOUNT_EXISTS":
		return target == ErrAccountExists
	case "NOT_FOUND":
		return target == ErrNotFound
	default:
		return target == ErrRefused
	}
}

// projection binds a complete snapshot to a boot, cursor and configuration revision.
type projection struct {
	Boot     string   `json:"bootId"`
	Seq      string   `json:"seq"`
	Revision string   `json:"revision"`
	Snapshot Snapshot `json:"snapshot"`
}

// NewRemote starts the reconnect loop using the platform's authenticated local pipe.
// Side effects: starts SyncD once when its pipe does not exist.
func NewRemote(ctx context.Context, profile, executable string) *Remote {
	return newRemote(ctx, profile, executable, true)
}

// ConnectRemote is NewRemote that never starts SyncD: it only connects to a
// running one, for the installer (packaging.md).
// Notes: executable may be relative here; it is only compared with the
// pipe server's own path.
func ConnectRemote(ctx context.Context, profile, executable string) *Remote {
	return newRemote(ctx, profile, executable, false)
}

// newRemote starts the reconnect loop; launch says whether a missing pipe
// starts SyncD.
func newRemote(ctx context.Context, profile, executable string, launch bool) *Remote {
	ctx, cancel := context.WithCancel(ctx)
	r := &Remote{ctx: ctx, cancel: cancel, pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	var once sync.Once
	r.dial = func(ctx context.Context) (io.ReadWriteCloser, error) {
		conn, err := dialPipe(ctx, profile, executable)
		if launch && errors.Is(err, os.ErrNotExist) {
			once.Do(func() { _ = startDaemon(profile, executable) })
		}
		return conn, err
	}
	go r.run()
	return r
}

// Close ends UI transport activity while leaving background sync running.
func (r *Remote) Close() {
	r.cancel()
	r.mu.Lock()
	if r.conn != nil {
		r.conn.Close()
	}
	r.mu.Unlock()
}

// Snapshot returns an independent copy so views cannot mutate shared transport state.
func (r *Remote) Snapshot() Snapshot {
	r.mu.Lock()
	defer r.mu.Unlock()
	b, _ := json.Marshal(r.state)
	var s Snapshot
	_ = json.Unmarshal(b, &s)
	s.Daemon.Connected = r.state.Daemon.Connected
	s.Daemon.ProtocolMinor = r.state.Daemon.ProtocolMinor
	return s
}

// Subscribe registers a notification callback; callbacks run outside the state lock.
func (r *Remote) Subscribe(fn func()) func() {
	id := r.next.Add(1)
	r.mu.Lock()
	r.listeners[id] = fn
	r.mu.Unlock()
	return func() { r.mu.Lock(); delete(r.listeners, id); r.mu.Unlock() }
}

// notify publishes state changes without holding a lock across application callbacks.
func (r *Remote) notify() {
	r.mu.Lock()
	fns := make([]func(), 0, len(r.listeners))
	for _, fn := range r.listeners {
		fns = append(fns, fn)
	}
	r.mu.Unlock()
	for _, fn := range fns {
		fn()
	}
}

// run reconnects with bounded delay and never reuses a snapshot across daemon boots.
func (r *Remote) run() {
	for r.ctx.Err() == nil {
		conn, err := r.dial(r.ctx)
		if err != nil {
			r.mu.Lock()
			r.cause = "dial: " + dialCause(err)
			r.mu.Unlock()
		}
		if err == nil {
			r.mu.Lock()
			r.conn = conn
			r.legacyState = false
			r.mu.Unlock()
			done := make(chan struct{})
			go func() { r.read(conn); close(done) }()
			var hello struct {
				Protocol struct {
					Major int  `json:"major"`
					Minor *int `json:"minor"`
				} `json:"protocol"`
			}
			err = r.call(r.ctx, "session.hello", map[string]any{"protocol": map[string]int{"major": 1, "minor": protocolMinor}}, &hello)
			if err == nil && hello.Protocol.Major == 1 {
				r.mu.Lock()
				r.legacyState = hello.Protocol.Minor != nil && *hello.Protocol.Minor >= 0 && *hello.Protocol.Minor < 7
				r.minor = 0
				if hello.Protocol.Minor != nil {
					r.minor = *hello.Protocol.Minor
				}
				r.mu.Unlock()
				err = r.call(r.ctx, "state.bootstrap", map[string]any{}, nil)
			} else if err == nil {
				err = ErrRefused
			}
			if err != nil {
				r.mu.Lock()
				if r.cause == "" {
					r.cause = "handshake: " + dialCause(err)
				}
				r.mu.Unlock()
				conn.Close()
			}
			select {
			case <-done:
			case <-r.ctx.Done():
				conn.Close()
				<-done
			}
		}
		select {
		case <-r.ctx.Done():
			return
		case <-time.After(time.Second):
		}
	}
}

// apply accepts monotonic projections; bootstrap cannot overwrite a newer queued event.
// Notes: diagnostic bootstrap enables RPCs, not healthy UI status; unavailable collections remain unknown.
func (r *Remote) apply(p projection, bootstrap bool) error {
	seq, err := strconv.ParseUint(p.Seq, 10, 64)
	if err != nil || strconv.FormatUint(seq, 10) != p.Seq || p.Boot == "" {
		return ErrRefused
	}
	rev, err := strconv.ParseUint(p.Revision, 10, 64)
	if err != nil || strconv.FormatUint(rev, 10) != p.Revision {
		return ErrRefused
	}
	r.mu.Lock()
	if !bootstrap && !r.ready {
		r.mu.Unlock()
		return nil
	}
	if !bootstrap && r.boot != p.Boot {
		r.mu.Unlock()
		return ErrRefused
	}
	if r.boot == p.Boot && seq < r.seq {
		r.mu.Unlock()
		return nil
	}
	if r.legacyState && p.Snapshot.Daemon.StartupState == "" {
		p.Snapshot.Daemon.StateAvailable = true
		p.Snapshot.Daemon.StartupState = StartupNormal
	}
	if !p.Snapshot.Daemon.StateAvailable || p.Snapshot.Daemon.StartupState != StartupNormal {
		// Never retain cached or unexpected lists as proof of known application state.
		p.Snapshot.Daemon.StateAvailable = false
		p.Snapshot = Snapshot{Daemon: p.Snapshot.Daemon}
	}
	r.boot = p.Boot
	r.seq = seq
	r.revision = p.Revision
	r.state = p.Snapshot
	r.state.Daemon.Connected = r.state.Daemon.StateAvailable
	r.state.Daemon.ProtocolMinor = r.minor
	r.ready = true
	if bootstrap {
		r.cause = ""
	}
	r.mu.Unlock()
	r.notify()
	return nil
}

// read dispatches replies and ordered projections, failing pending calls on disconnect.
func (r *Remote) read(conn io.ReadWriteCloser) {
	cause := "eof"
	defer func() {
		conn.Close()
		r.mu.Lock()
		if r.cause == "" {
			r.cause = cause
		}
		r.conn = nil
		r.ready = false
		r.state.Daemon.Connected = false
		for i := range r.state.Folders {
			r.state.Folders[i].State = FolderOffline
		}
		for i := range r.state.Endpoints {
			r.state.Endpoints[i].Reachable = false
			r.state.Endpoints[i].InUse = false
		}
		for id, ch := range r.pending {
			ch <- rpcReply{transport: io.ErrUnexpectedEOF}
			delete(r.pending, id)
		}
		r.mu.Unlock()
		r.notify()
	}()
	for {
		b, err := readFrame(conn)
		if err != nil {
			return
		}
		cause = "invalid frame"
		var fields map[string]json.RawMessage
		if json.Unmarshal(b, &fields) != nil || string(fields["jsonrpc"]) != `"2.0"` {
			return
		}
		var header struct {
			Method string          `json:"method"`
			Params json.RawMessage `json:"params"`
		}
		if json.Unmarshal(b, &header) != nil {
			return
		}
		if header.Method == "event" {
			if fields["id"] != nil || fields["result"] != nil || fields["error"] != nil {
				return
			}
			var p projection
			if err := json.Unmarshal(header.Params, &p); err != nil {
				cause = "event: " + decodeCause(err)
				return
			}
			if r.apply(p, false) != nil {
				cause = "event refused"
				return
			}
			cause = "eof"
			continue
		}
		if header.Method != "" || fields["id"] == nil || (fields["result"] == nil) == (fields["error"] == nil) {
			return
		}
		var reply rpcReply
		if json.Unmarshal(b, &reply) != nil || reply.ID == "" || (fields["error"] != nil && reply.Error == nil) {
			return
		}
		r.mu.Lock()
		ch := r.pending[reply.ID]
		bootstrap := reply.ID == r.bootstrapID
		r.mu.Unlock()
		if ch == nil {
			cause = "unknown reply"
			return
		}
		if bootstrap && reply.Error == nil {
			var p projection
			if err := json.Unmarshal(reply.Result, &p); err != nil {
				cause = "bootstrap: " + decodeCause(err)
				return
			}
			if r.apply(p, true) != nil {
				cause = "bootstrap refused"
				return
			}
		}
		cause = "eof"
		r.mu.Lock()
		delete(r.pending, reply.ID)
		r.mu.Unlock()
		ch <- reply
	}
}

// call sends a correlated RPC with a deadline and bounded outstanding-request count.
// Notes: cancellation takes precedence over the transport failure caused by closing its pipe.
func (r *Remote) call(ctx context.Context, method string, params any, out any) error {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	id := fmt.Sprint(r.next.Add(1))
	ch := make(chan rpcReply, 1)
	r.mu.Lock()
	conn := r.conn
	if conn == nil || len(r.pending) >= 32 || (!r.ready && method != "session.hello" && method != "state.bootstrap") {
		r.mu.Unlock()
		return ErrBusy
	}
	r.pending[id] = ch
	if method == "state.bootstrap" {
		r.bootstrapID = id
	}
	r.mu.Unlock()
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	defer stop()
	defer func() { r.mu.Lock(); delete(r.pending, id); r.mu.Unlock() }()
	r.writeMu.Lock()
	err := writeFrame(conn, map[string]any{"jsonrpc": "2.0", "id": id, "method": method, "params": params})
	r.writeMu.Unlock()
	if err != nil {
		conn.Close()
		return err
	}
	select {
	case <-ctx.Done():
		conn.Close()
		return ctx.Err()
	case reply := <-ch:
		if err := ctx.Err(); err != nil {
			return err
		}
		if reply.transport != nil {
			return reply.transport
		}
		if reply.Error != nil {
			return reply.Error
		}
		if out != nil {
			return json.Unmarshal(reply.Result, out)
		}
		return nil
	}
}

// operationKey creates a UUID for one logical mutation, retained across reconnect retries.
func operationKey() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic(err)
	}
	b[6] = (b[6] & 15) | 64
	b[8] = (b[8] & 63) | 128
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[:4], b[4:6], b[6:8], b[8:10], b[10:])
}

// operation is a durable receipt; running never means propagation has completed.
type operation struct {
	ID     string          `json:"operationId"`
	State  string          `json:"state"`
	Result json.RawMessage `json:"result"`
	Error  *rpcError       `json:"error"`
}

// submit retains one operation key across uncertain transport retries without waiting for accepted work.
func (r *Remote) submit(ctx context.Context, method string, p map[string]any) (operation, error) {
	var op operation
	p["operationKey"] = operationKey()
	r.mu.Lock()
	if !r.ready {
		r.mu.Unlock()
		return op, ErrBusy
	}
	p["expectedRevision"] = r.revision
	r.mu.Unlock()
	for {
		err := r.call(ctx, method, p, &op)
		if err == nil {
			return op, nil
		}
		var remote *rpcError
		if errors.As(err, &remote) {
			return op, err
		}
		select {
		case <-ctx.Done():
			return op, ctx.Err()
		case <-time.After(250 * time.Millisecond):
		}
	}
}

// mutate resolves accepted work through operations.get and retries uncertain transport outcomes with the same key.
func (r *Remote) mutate(method string, p map[string]any, out any) error {
	ctx, cancel := context.WithTimeout(r.ctx, 45*time.Second)
	defer cancel()
	op, err := r.submit(ctx, method, p)
	if err != nil {
		return err
	}
	for op.State == "accepted" || op.State == "running" {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(200 * time.Millisecond):
		}
		if err := r.call(ctx, "operations.get", map[string]any{"operationId": op.ID}, &op); err != nil {
			return err
		}
	}
	if op.Error != nil {
		return op.Error
	}
	if op.State != "succeeded" {
		return ErrRefused
	}
	if out != nil {
		return json.Unmarshal(op.Result, out)
	}
	return nil
}

// RequestSwitch submits a manual account-wide switch and returns its durable operation ID.
// Notes: use WaitOperation to observe reconciliation; user-paused folders may keep it pending indefinitely.
// A transport/deadline error does not prove rejection or cancel any already accepted work.
func (r *Remote) RequestSwitch(account, endpoint string) (string, error) {
	ctx, cancel := context.WithTimeout(r.ctx, 45*time.Second)
	defer cancel()
	op, err := r.submit(ctx, "endpoints.requestSwitch", map[string]any{"accountId": account, "endpointId": endpoint})
	if err != nil {
		return "", err
	}
	if op.Error != nil {
		return "", op.Error
	}
	if op.ID == "" || (op.State != "running" && op.State != "accepted" && op.State != "succeeded") {
		return "", ErrRefused
	}
	return op.ID, nil
}

// WaitOperation observes an existing receipt without ever resubmitting or cancelling its work.
func (r *Remote) WaitOperation(ctx context.Context, id string) error {
	for {
		var op operation
		if err := r.call(ctx, "operations.get", map[string]any{"operationId": id}, &op); err != nil {
			return err
		}
		if op.Error != nil {
			return op.Error
		}
		switch op.State {
		case "succeeded":
			return nil
		case "accepted", "running":
		default:
			return ErrRefused
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(200 * time.Millisecond):
		}
	}
}

// PauseAll persists the global user pause independently of folder pause flags (INV-13).
func (r *Remote) PauseAll() error {
	return r.mutate("settings.update", map[string]any{"allPaused": true}, nil)
}

// ResumeAll removes only the global pause (INV-13).
func (r *Remote) ResumeAll() error {
	return r.mutate("settings.update", map[string]any{"allPaused": false}, nil)
}

// PauseFolder requests a folder-specific pause.
func (r *Remote) PauseFolder(id string) error {
	return r.mutate("folders.pause", map[string]any{"folderId": id}, nil)
}

// ResumeFolder removes a folder-specific user pause.
func (r *Remote) ResumeFolder(id string) error {
	return r.mutate("folders.resume", map[string]any{"folderId": id}, nil)
}

// SyncNow schedules a cycle through the daemon.
func (r *Remote) SyncNow(id string) error {
	return r.mutate("folders.syncNow", map[string]any{"folderId": id}, nil)
}

// RemoveFolder removes only the mapping, never its files.
func (r *Remote) RemoveFolder(id string) error {
	return r.mutate("folders.remove", map[string]any{"folderId": id}, nil)
}

// ResolveDecision binds the answer to the reviewed proposal.
func (r *Remote) ResolveDecision(id, digest string, a Action) error {
	return r.mutate("decisions.resolve", map[string]any{"decisionId": id, "proposalDigest": digest, "action": a}, nil)
}

// StartLogin requests a browser login without exposing an app password.
func (r *Remote) StartLogin(url string) (Login, error) {
	var login Login
	err := r.mutate("accounts.login.start", map[string]any{"canonicalBaseUrl": url}, &login)
	return login, err
}

// WaitLogin observes a login session independently of its start operation's identity.
func (r *Remote) WaitLogin(ctx context.Context, id string) (string, error) {
	for {
		var result struct {
			AccountID string `json:"accountId"`
			State     string `json:"state"`
			Reason    string `json:"reasonCode"`
		}
		if err := r.call(ctx, "accounts.login.status", map[string]any{"loginSessionId": id}, &result); err != nil {
			return "", err
		}
		if result.State == "succeeded" && result.AccountID != "" {
			if result.Reason == "APP_PASSWORD_REVOCATION_UNCONFIRMED" {
				return result.AccountID, ErrCredentialCleanup
			}
			return result.AccountID, nil
		}
		if result.State == "failed" || result.State == "cancelled" {
			if result.Reason == "APP_PASSWORD_REVOCATION_UNCONFIRMED" {
				return "", ErrCredentialCleanup
			}
			// Keeps the reason (expired, cancelled) for the interface's words; still ErrRefused.
			failure := &rpcError{Code: -32000}
			failure.Data.Reason = result.Reason
			return "", failure
		}
		select {
		case <-ctx.Done():
			return "", ctx.Err()
		case <-time.After(time.Second):
		}
	}
}

// CancelLogin cancels the local login transaction.
func (r *Remote) CancelLogin(id string) error {
	return r.mutate("accounts.login.cancel", map[string]any{"loginSessionId": id}, nil)
}

// Reauthenticate renews credentials without changing account identity.
func (r *Remote) Reauthenticate(id string) (Login, error) {
	var login Login
	err := r.mutate("accounts.reauthenticate", map[string]any{"accountId": id}, &login)
	return login, err
}

// RemoveAccount removes account management after safe shutdown.
func (r *Remote) RemoveAccount(id string) error {
	var result struct {
		Revoked bool `json:"serverRevocationConfirmed"`
	}
	if err := r.mutate("accounts.remove", map[string]any{"accountId": id}, &result); err != nil {
		return err
	}
	if !result.Revoked {
		return ErrRevocationUnconfirmed
	}
	return nil
}

// RemoteFolders reads the account's selectable remote folders.
func (r *Remote) RemoteFolders(id string) ([]RemoteFolder, error) {
	var folders []RemoteFolder
	err := r.call(r.ctx, "folders.list", map[string]any{"accountId": id, "remote": true}, &folders)
	return folders, err
}

// AddFolder binds an inspection token to a separately revalidated add command.
// Notes: without a mode SyncD adds a full-file folder; FolderOnDemand needs
// protocol 1.8 and answers UNSUPPORTED_CAPABILITY where Cloud Files is missing.
func (r *Remote) AddFolder(account, remote, local string, mode ...FolderMode) error {
	var inspection struct {
		Token string `json:"inspectToken"`
	}
	if err := r.call(r.ctx, "folders.inspect", map[string]any{"accountId": account, "remotePath": remote, "localPath": local}, &inspection); err != nil {
		return err
	}
	params := map[string]any{"inspectToken": inspection.Token}
	if len(mode) > 0 && mode[0] != "" {
		params["mode"] = mode[0]
	}
	return r.mutate("folders.add", params, nil)
}

// RequestPolicy returns durable policy acceptance separately from potentially user-paused reconciliation.
func (r *Remote) RequestPolicy(account string, p Policy, fixed string) (string, error) {
	ctx, cancel := context.WithTimeout(r.ctx, 45*time.Second)
	defer cancel()
	op, err := r.submit(ctx, "endpoints.setPolicy", map[string]any{"accountId": account, "policy": p, "fixedEndpointId": fixed})
	if err != nil {
		return "", err
	}
	if op.Error != nil {
		return "", op.Error
	}
	if op.ID == "" || (op.State != "running" && op.State != "accepted" && op.State != "succeeded") {
		return "", ErrRefused
	}
	return op.ID, nil
}

// SetPolicy waits for verified daemon-owned selection; a timeout never cancels accepted work.
func (r *Remote) SetPolicy(account string, p Policy, fixed string) error {
	return r.mutate("endpoints.setPolicy", map[string]any{"accountId": account, "policy": p, "fixedEndpointId": fixed}, nil)
}

// StageEndpoint inspects an address without account credentials.
func (r *Remote) StageEndpoint(account, name, url string, kind ...EndpointKind) (Staging, error) {
	var staging Staging
	params := map[string]any{"accountId": account, "name": name, "baseUrl": url}
	if len(kind) > 0 {
		params["kind"] = string(kind[0])
	}
	err := r.mutate("endpoints.stage", params, &staging)
	return staging, err
}

// ApproveEndpoint records explicit origin approval before verification.
func (r *Remote) ApproveEndpoint(id string) (string, error) {
	var result struct {
		ID string `json:"endpointId"`
	}
	err := r.mutate("endpoints.approveOrigin", map[string]any{"stagingId": id}, &result)
	return result.ID, err
}

// VerifyEndpoint requests authenticated namespace verification.
func (r *Remote) VerifyEndpoint(id string) error {
	return r.mutate("endpoints.verify", map[string]any{"endpointId": id}, nil)
}

// RemoveEndpoint removes only the nominated endpoint configuration.
func (r *Remote) RemoveEndpoint(id string) error {
	return r.mutate("endpoints.remove", map[string]any{"endpointId": id}, nil)
}

// RuntimeDiagnostics contains nonsecret aggregate counters intended only for diagnostics and acceptance.
type RuntimeDiagnostics struct {
	Version, Protocol, NetworkGeneration string
	AllPaused                            bool
	StateAvailable                       bool         `json:"stateAvailable"`
	StartupState                         StartupState `json:"startupState"`
	ReasonCode                           string       `json:"reasonCode"`
	Folders                              []FolderDiagnostics
}

// FolderDiagnostics identifies one immutable binding without exposing its local or remote path.
type FolderDiagnostics struct {
	FolderID, AccountID, Epoch string
	State, ReasonCode          string
	UserPaused                 bool
	Blockers                   []string
	ActiveUploads              uint64 `json:",string"`
	ActiveDownloads            uint64 `json:",string"`
	ActiveRequests             uint64 `json:",string"`
	RetainedReplies            uint64 `json:",string"`
	UncertainWrites            uint64 `json:",string"`
	Requests                   uint64 `json:",string"`
	TransportFailures          uint64 `json:",string"`
	UploadBytes                uint64 `json:",string"`
	DownloadBytes              uint64 `json:",string"`
}

// Diagnostics reads sanitized runtime evidence through IPC, never by inspecting the journal or Control DB.
func (r *Remote) Diagnostics(ctx context.Context) (RuntimeDiagnostics, error) {
	var result RuntimeDiagnostics
	err := r.call(ctx, "diagnostics.snapshot", map[string]any{}, &result)
	r.mu.Lock()
	legacy := r.legacyState
	r.mu.Unlock()
	if err == nil && legacy && result.StartupState == "" {
		result.StateAvailable, result.StartupState = true, StartupNormal
	}
	return result, err
}

// ExportDiagnostics requests only the daemon's sanitized local JSON bundle; nothing is uploaded.
func (r *Remote) ExportDiagnostics() (string, error) {
	var result struct {
		Path string `json:"path"`
	}
	err := r.mutate("diagnostics.export", map[string]any{"options": map[string]any{}}, &result)
	if err == nil && result.Path == "" {
		return "", ErrRefused
	}
	return result.Path, err
}

// PrepareUpdate requests a proven safe point before installation.
func (r *Remote) PrepareUpdate(v string) error {
	return r.mutate("daemon.prepareUpdate", map[string]any{"targetVersion": v}, nil)
}

// Shutdown explicitly ends background sync.
func (r *Remote) Shutdown() error {
	return r.mutate("daemon.shutdown", map[string]any{"reason": "user"}, nil)
}

var _ Client = (*Remote)(nil)

// Disconnect says, for Diagnostics, why the last connection to SyncD ended
// or could not start: a stable phrase that names at most a field, never data.
// Returns: "" while connected and before any connection ended.
func (r *Remote) Disconnect() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.cause
}

// decodeCause names a projection decoding failure by field and kind only.
func decodeCause(err error) string {
	if e, ok := errors.AsType[*json.UnmarshalTypeError](err); ok {
		return "field " + e.Field + " is " + e.Value
	}
	return "invalid JSON"
}

// dialCause names a transport or handshake failure without paths or names.
func dialCause(err error) string {
	switch {
	case errors.Is(err, os.ErrNotExist):
		return "no pipe"
	case errors.Is(err, ErrRefused):
		return "refused"
	case errors.Is(err, ErrStale), errors.Is(err, ErrBusy):
		return "busy"
	case errors.Is(err, context.DeadlineExceeded), errors.Is(err, context.Canceled):
		return "deadline"
	case errors.Is(err, io.ErrUnexpectedEOF), errors.Is(err, io.EOF):
		return "eof"
	}
	if _, ok := errors.AsType[*rpcError](err); ok {
		return "rejected"
	}
	return "other"
}
