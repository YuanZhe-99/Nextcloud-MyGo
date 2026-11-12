package syncd

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"os"
	"reflect"
	"sync"
	"testing"
	"time"
)

// TestDeletionApprovalEnvelope shares the C++ boundary example to prevent digest-field drift (INV-11).
func TestDeletionApprovalEnvelope(t *testing.T) {
	b, err := os.ReadFile("../../protocol/folders.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples map[string]struct {
		Method string
		Params map[string]any
	}
	if err := json.Unmarshal(b, &examples); err != nil {
		t.Fatal(err)
	}
	example := examples["approveDeletion"]
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	client, server := net.Pipe()
	defer server.Close()
	r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, revision: "42", pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	done := make(chan struct{})
	go func() { r.read(client); close(done) }()
	t.Cleanup(func() { client.Close(); <-done })
	request := make(chan map[string]any, 1)
	go func() {
		b, err := readFrame(server)
		if err != nil {
			return
		}
		var req map[string]any
		if json.Unmarshal(b, &req) != nil {
			return
		}
		request <- req
		_ = writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": map[string]any{"operationId": "test", "state": "succeeded", "result": map[string]any{}}})
	}()
	if err := r.ResolveDecision(example.Params["decisionId"].(string), example.Params["proposalDigest"].(string), ActionApproveDeletion); err != nil {
		t.Fatal(err)
	}
	select {
	case req := <-request:
		if req["method"] != example.Method {
			t.Fatal("wrong approval method")
		}
		params := req["params"].(map[string]any)
		if len(params) != len(example.Params) {
			t.Fatal("approval parameter set drifted")
		}
		for key, expected := range example.Params {
			if key != "operationKey" && params[key] != expected {
				t.Fatalf("approval field %s differs from protocol example", key)
			}
		}
	case <-ctx.Done():
		t.Fatal("approval request missing")
	}
}

// TestFolderAddModeEnvelope keeps folders.add aligned with protocol 1.8's
// addOnDemand example and sends no mode for a full-file folder (PLAN §2.3).
func TestFolderAddModeEnvelope(t *testing.T) {
	b, err := os.ReadFile("../../protocol/folders.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples map[string]struct {
		Method string
		Params map[string]any
	}
	if err := json.Unmarshal(b, &examples); err != nil {
		t.Fatal(err)
	}
	example := examples["addOnDemand"]
	for _, mode := range []FolderMode{FolderOnDemand, ""} {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		client, server := net.Pipe()
		r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, revision: example.Params["expectedRevision"].(string), pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
		done := make(chan struct{})
		go func() { r.read(client); close(done) }()
		added := make(chan map[string]any, 1)
		go func() {
			for {
				b, err := readFrame(server)
				if err != nil {
					return
				}
				var req map[string]any
				if json.Unmarshal(b, &req) != nil {
					return
				}
				result := map[string]any{"operationId": "test", "state": "succeeded", "result": map[string]any{}}
				if req["method"] == "folders.inspect" {
					result = map[string]any{"inspectToken": example.Params["inspectToken"], "localExists": true, "mergeExisting": false, "expiresInSeconds": 300}
				} else {
					added <- req
				}
				_ = writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": result})
			}
		}()
		var modes []FolderMode
		if mode != "" {
			modes = append(modes, mode)
		}
		if err := r.AddFolder("account", "/Example", "C:/Sync/Example", modes...); err != nil {
			t.Fatal(err)
		}
		select {
		case req := <-added:
			params := req["params"].(map[string]any)
			if req["method"] != example.Method {
				t.Fatal("wrong add method")
			}
			for key, expected := range example.Params {
				if key == "operationKey" || (key == "mode" && mode == "") {
					continue
				}
				if params[key] != expected {
					t.Fatalf("add field %s differs from protocol example", key)
				}
			}
			if _, sent := params["mode"]; mode == "" && sent {
				t.Fatal("full-file add sent a mode")
			}
		case <-ctx.Done():
			t.Fatal("add request missing")
		}
		client.Close()
		server.Close()
		<-done
		cancel()
	}
}

// TestEndpointCommandEnvelopes keeps consent and manual-switch fields aligned with protocol 1.4 (INV-03/06).
func TestEndpointCommandEnvelopes(t *testing.T) {
	b, err := os.ReadFile("../../protocol/endpoints.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples map[string]struct {
		Method string
		Params map[string]any
	}
	if err := json.Unmarshal(b, &examples); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	client, server := net.Pipe()
	defer server.Close()
	r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	done := make(chan struct{})
	go func() { r.read(client); close(done) }()
	t.Cleanup(func() { client.Close(); <-done })
	requests := make(chan map[string]any, 4)
	go func() {
		for range 4 {
			b, err := readFrame(server)
			if err != nil {
				return
			}
			var req map[string]any
			if json.Unmarshal(b, &req) != nil {
				return
			}
			requests <- req
			if writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": map[string]any{"operationId": "test", "state": "succeeded", "result": map[string]any{}}}) != nil {
				return
			}
		}
	}()
	for _, name := range []string{"stage", "approve", "verify", "switch"} {
		example := examples[name]
		r.mu.Lock()
		r.revision = example.Params["expectedRevision"].(string)
		r.mu.Unlock()
		switch name {
		case "stage":
			_, err = r.StageEndpoint(example.Params["accountId"].(string), example.Params["name"].(string), example.Params["baseUrl"].(string))
		case "approve":
			_, err = r.ApproveEndpoint(example.Params["stagingId"].(string))
		case "verify":
			err = r.VerifyEndpoint(example.Params["endpointId"].(string))
		case "switch":
			_, err = r.RequestSwitch(example.Params["accountId"].(string), example.Params["endpointId"].(string))
		}
		if err != nil {
			t.Fatal(err)
		}
		select {
		case req := <-requests:
			if req["method"] != example.Method {
				t.Fatal("endpoint method drifted")
			}
			params := req["params"].(map[string]any)
			if len(params) != len(example.Params) {
				t.Fatal("endpoint parameter set drifted")
			}
			for key, expected := range example.Params {
				if key != "operationKey" && params[key] != expected {
					t.Fatalf("endpoint field %s differs from protocol", key)
				}
			}
		case <-ctx.Done():
			t.Fatal("endpoint request missing")
		}
	}
}

// TestSwitchReceiptWait observes completion without resubmitting accepted work (INV-06/12).
func TestSwitchReceiptWait(t *testing.T) {
	for _, terminal := range []string{"succeeded", "failed"} {
		t.Run(terminal, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			client, server := net.Pipe()
			defer server.Close()
			r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, revision: "7", pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
			done := make(chan struct{})
			go func() { r.read(client); close(done) }()
			t.Cleanup(func() { client.Close(); <-done })
			requests := make(chan string, 3)
			go func() {
				for i := range 3 {
					b, err := readFrame(server)
					if err != nil {
						return
					}
					var req struct {
						ID     string `json:"id"`
						Method string `json:"method"`
					}
					if json.Unmarshal(b, &req) != nil {
						return
					}
					requests <- req.Method
					result := map[string]any{"operationId": "switch-test", "state": "running", "result": map[string]any{}}
					if i == 2 {
						result["state"] = terminal
						if terminal == "failed" {
							result["error"] = map[string]any{"code": -32000, "data": map[string]any{"reasonCode": "TLS_ERROR"}}
						}
					}
					if writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req.ID, "result": result}) != nil {
						return
					}
				}
			}()
			id, err := r.RequestSwitch("account", "endpoint")
			if err != nil || id != "switch-test" {
				t.Fatalf("acceptance: %q %v", id, err)
			}
			err = r.WaitOperation(ctx, id)
			if (terminal == "succeeded" && err != nil) || (terminal == "failed" && !errors.Is(err, ErrRefused)) {
				t.Fatalf("completion: %v", err)
			}
			for _, method := range []string{"endpoints.requestSwitch", "operations.get", "operations.get"} {
				select {
				case got := <-requests:
					if got != method {
						t.Fatalf("resubmitted switch: %s", got)
					}
				case <-ctx.Done():
					t.Fatal("missing receipt query")
				}
			}
		})
	}
}

// TestLoginSessionCleanupWarning protects session identity and actionable cleanup failures (INV-03/12).
func TestLoginSessionCleanupWarning(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	client, server := net.Pipe()
	defer server.Close()
	r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	done := make(chan struct{})
	go func() { r.read(client); close(done) }()
	t.Cleanup(func() { client.Close(); <-done })
	request := make(chan map[string]any, 1)
	go func() {
		b, err := readFrame(server)
		if err != nil {
			return
		}
		var req map[string]any
		if json.Unmarshal(b, &req) != nil {
			return
		}
		request <- req
		_ = writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": map[string]any{"state": "failed", "reasonCode": "APP_PASSWORD_REVOCATION_UNCONFIRMED"}})
	}()
	if _, err := r.WaitLogin(ctx, "session-test"); !errors.Is(err, ErrCredentialCleanup) {
		t.Fatalf("cleanup warning lost: %v", err)
	}
	select {
	case req := <-request:
		if req["method"] != "accounts.login.status" || req["params"].(map[string]any)["loginSessionId"] != "session-test" {
			t.Fatal("session polled as operation")
		}
	case <-ctx.Done():
		t.Fatal("login status request missing")
	}
}

// TestLoginFailureKeepsReason: a failed or cancelled sign-in stays
// ErrRefused and keeps SyncD's reason, for the interface to say why.
func TestLoginFailureKeepsReason(t *testing.T) {
	for _, c := range []struct{ state, reason string }{{"failed", "LOGIN_SESSION_EXPIRED"}, {"cancelled", "LOGIN_CANCELLED"}, {"failed", ""}} {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		client, server := net.Pipe()
		r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
		done := make(chan struct{})
		go func() { r.read(client); close(done) }()
		go func() {
			b, err := readFrame(server)
			var req map[string]any
			if err != nil || json.Unmarshal(b, &req) != nil {
				return
			}
			_ = writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": map[string]any{"state": c.state, "reasonCode": c.reason}})
		}()
		_, err := r.WaitLogin(ctx, "session-test")
		if !errors.Is(err, ErrRefused) || ReasonOf(err) != c.reason {
			t.Errorf("%s %s: %v, reason %q", c.state, c.reason, err, ReasonOf(err))
		}
		client.Close()
		server.Close()
		<-done
		cancel()
	}
}

// TestReauthenticationBoundary preserves account IDs and explicit old-password cleanup warnings (INV-02/03/12).
func TestReauthenticationBoundary(t *testing.T) {
	b, err := os.ReadFile("../../protocol/reauthentication.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples map[string]struct {
		Method string
		Params map[string]any
		Result json.RawMessage
	}
	if err = json.Unmarshal(b, &examples); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	client, server := net.Pipe()
	defer server.Close()
	r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, revision: "42", pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	done := make(chan struct{})
	go func() { r.read(client); close(done) }()
	t.Cleanup(func() { client.Close(); <-done })
	requests := make(chan map[string]any, 2)
	go func() {
		for i := range 2 {
			body, err := readFrame(server)
			if err != nil {
				return
			}
			var req map[string]any
			if json.Unmarshal(body, &req) != nil {
				return
			}
			requests <- req
			var result any = map[string]any{"operationId": "renew-op", "state": "succeeded", "result": map[string]any{"id": "renew-session", "loginURL": "https://primary.example.invalid/login", "expires": "2026-11-08T00:00:00Z"}}
			if i == 1 {
				result = examples["warning"].Result
			}
			if writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": result}) != nil {
				return
			}
		}
	}()
	account := examples["renew"].Params["accountId"].(string)
	login, err := r.Reauthenticate(account)
	if err != nil || login.ID != "renew-session" {
		t.Fatalf("renewal: %+v %v", login, err)
	}
	id, err := r.WaitLogin(ctx, login.ID)
	if id != account || !errors.Is(err, ErrCredentialCleanup) {
		t.Fatalf("installed account/cleanup warning lost: %q %v", id, err)
	}
	for i := range 2 {
		select {
		case req := <-requests:
			params := req["params"].(map[string]any)
			if i == 0 {
				if req["method"] != examples["renew"].Method || len(params) != len(examples["renew"].Params) {
					t.Fatal("renewal envelope drifted")
				}
				for key, value := range examples["renew"].Params {
					if key != "operationKey" && params[key] != value {
						t.Fatalf("renewal field %s drifted", key)
					}
				}
			} else if req["method"] != "accounts.login.status" || params["loginSessionId"] != login.ID {
				t.Fatal("renewal polled as operation")
			}
		case <-ctx.Done():
			t.Fatal("missing renewal request")
		}
	}
}

// TestPolicyBoundary keeps durable acceptance and requested/actual targets distinct (INV-12/14).
func TestPolicyBoundary(t *testing.T) {
	b, err := os.ReadFile("../../protocol/policy.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples map[string]json.RawMessage
	if json.Unmarshal(b, &examples) != nil {
		t.Fatal("policy examples invalid")
	}
	var example struct {
		Method string
		Params map[string]any
	}
	if json.Unmarshal(examples["fixed"], &example) != nil {
		t.Fatal("policy request invalid")
	}
	var n Network
	if err = json.Unmarshal(examples["pendingNetwork"], &n); err != nil {
		t.Fatal(err)
	}
	if n.Policy != PolicyAuto || n.RequestedEndpointID == "" || n.ActiveID == n.RequestedEndpointID || n.FixedEndpointID != "" {
		t.Fatal("pending target became committed configuration")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	client, server := net.Pipe()
	defer server.Close()
	r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, revision: "42", pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	done := make(chan struct{})
	go func() { r.read(client); close(done) }()
	t.Cleanup(func() { client.Close(); <-done })
	request := make(chan map[string]any, 1)
	go func() {
		body, err := readFrame(server)
		if err != nil {
			return
		}
		var req map[string]any
		if json.Unmarshal(body, &req) != nil {
			return
		}
		request <- req
		_ = writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": map[string]any{"operationId": "policy-operation", "state": "running", "result": map[string]any{}}})
	}()
	id, err := r.RequestPolicy(example.Params["accountId"].(string), PolicyFixed, example.Params["fixedEndpointId"].(string))
	if err != nil || id != "policy-operation" {
		t.Fatalf("policy acceptance: %q %v", id, err)
	}
	select {
	case req := <-request:
		params := req["params"].(map[string]any)
		if req["method"] != example.Method || len(params) != len(example.Params) {
			t.Fatal("policy envelope drifted")
		}
		for key, value := range example.Params {
			if key != "operationKey" && params[key] != value {
				t.Fatalf("policy field %s drifted", key)
			}
		}
	case <-ctx.Done():
		t.Fatal("policy acceptance polled instead of returning receipt")
	}
}

// TestDiagnosticsBoundary exercises sanitized string counters and explicit local bundle export through IPC (INV-03/12).
func TestDiagnosticsBoundary(t *testing.T) {
	b, err := os.ReadFile("../../protocol/diagnostics.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var example struct{ Result json.RawMessage }
	if json.Unmarshal(b, &example) != nil {
		t.Fatal("diagnostic example invalid")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	client, server := net.Pipe()
	defer server.Close()
	r := &Remote{ctx: ctx, cancel: cancel, conn: client, ready: true, revision: "42", pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	done := make(chan struct{})
	go func() { r.read(client); close(done) }()
	t.Cleanup(func() { client.Close(); <-done })
	requests := make(chan map[string]any, 2)
	go func() {
		for i := range 2 {
			body, err := readFrame(server)
			if err != nil {
				return
			}
			var req map[string]any
			if json.Unmarshal(body, &req) != nil {
				return
			}
			requests <- req
			var result any = example.Result
			if i == 1 {
				result = map[string]any{"operationId": "export-operation", "state": "succeeded", "result": map[string]any{"path": `C:\owned-diagnostics\bundle.json`, "format": "application/json"}}
			}
			if writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req["id"], "result": result}) != nil {
				return
			}
		}
	}()
	d, err := r.Diagnostics(ctx)
	if err != nil || d.Protocol != "1.8" || !d.StateAvailable || d.StartupState != StartupNormal || d.ReasonCode != "" || len(d.Folders) != 1 || d.Folders[0].ActiveUploads != 1 || d.Folders[0].UploadBytes != 65536 || d.Folders[0].UncertainWrites != 2 {
		t.Fatalf("diagnostics: %+v %v", d, err)
	}
	path, err := r.ExportDiagnostics()
	if err != nil || path != `C:\owned-diagnostics\bundle.json` {
		t.Fatalf("support export: %q %v", path, err)
	}
	for i := range 2 {
		select {
		case req := <-requests:
			if i == 0 && req["method"] != "diagnostics.snapshot" {
				t.Fatal("diagnostics did not use read-only boundary")
			}
			if i == 1 {
				params := req["params"].(map[string]any)
				if req["method"] != "diagnostics.export" || len(params["options"].(map[string]any)) != 0 {
					t.Fatal("export requested private data or drifted")
				}
			}
		case <-ctx.Done():
			t.Fatal("missing diagnostic request")
		}
	}
}

// TestBootstrapEventOrdering protects the atomic subscription boundary when replies and events arrive together (§13.6).
func TestBootstrapEventOrdering(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()
	r := &Remote{ctx: ctx, cancel: cancel, conn: client, pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	changed := make(chan struct{}, 4)
	r.Subscribe(func() { changed <- struct{}{} })
	done := make(chan struct{})
	go func() { r.read(client); close(done) }()
	t.Cleanup(func() { client.Close(); <-done })
	if err := r.call(ctx, "settings.update", map[string]any{}, nil); err != ErrDisconnected || !errors.Is(err, ErrBusy) {
		t.Fatalf("unnegotiated mutation: %v", err)
	}
	serverDone := make(chan error, 1)
	go func() {
		b, err := readFrame(server)
		if err != nil {
			serverDone <- err
			return
		}
		var req struct {
			ID string `json:"id"`
		}
		if err := json.Unmarshal(b, &req); err != nil {
			serverDone <- err
			return
		}
		p := projection{Boot: "boot-test", Seq: "0", Revision: "0", Snapshot: Snapshot{Daemon: Daemon{StateAvailable: true, StartupState: StartupNormal}}}
		if err := writeFrame(server, map[string]any{"jsonrpc": "2.0", "id": req.ID, "result": p}); err != nil {
			serverDone <- err
			return
		}
		p.Seq, p.Revision, p.Snapshot.Daemon.AllPaused = "1", "1", true
		serverDone <- writeFrame(server, map[string]any{"jsonrpc": "2.0", "method": "event", "params": p})
	}()
	if err := r.call(ctx, "state.bootstrap", map[string]any{}, nil); err != nil {
		t.Fatal(err)
	}
	for !r.Snapshot().Daemon.AllPaused {
		select {
		case <-changed:
		case <-ctx.Done():
			t.Fatal("event following bootstrap was lost")
		}
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
	if !r.Snapshot().Daemon.Connected {
		t.Fatal("live bootstrap did not mark connection ready")
	}
}

// testRPCRequest records observable requests from the real Remote transport.
type testRPCRequest struct {
	ID     string         `json:"id"`
	Method string         `json:"method"`
	Params map[string]any `json:"params"`
}

// readTestRequest checks the next framed request without terminating a peer goroutine.
func readTestRequest(t *testing.T, conn net.Conn, method string) (testRPCRequest, bool) {
	t.Helper()
	var req testRPCRequest
	body, err := readFrame(conn)
	if err != nil {
		t.Errorf("read %s: %v", method, err)
		return req, false
	}
	if err := json.Unmarshal(body, &req); err != nil {
		t.Errorf("decode %s: %v", method, err)
		return req, false
	}
	if req.Method != method || req.ID == "" {
		t.Errorf("expected %s request, got %+v", method, req)
		return req, false
	}
	return req, true
}

// replyTestRequest sends a correlated response through the actual framing boundary.
func replyTestRequest(t *testing.T, conn net.Conn, req testRPCRequest, result any) bool {
	t.Helper()
	if err := writeFrame(conn, map[string]any{"jsonrpc": "2.0", "id": req.ID, "result": result}); err != nil {
		t.Errorf("reply to %s: %v", req.Method, err)
		return false
	}
	return true
}

// bootstrapTestPeer negotiates and subscribes one injected daemon connection.
func bootstrapTestPeer(t *testing.T, conn net.Conn, p projection) bool {
	t.Helper()
	req, ok := readTestRequest(t, conn, "session.hello")
	if !ok || !replyTestRequest(t, conn, req, map[string]any{"protocol": map[string]int{"major": 1, "minor": 8}}) {
		return false
	}
	req, ok = readTestRequest(t, conn, "state.bootstrap")
	return ok && replyTestRequest(t, conn, req, p)
}

// startTestRemote exercises the production reconnect loop against bounded pipe peers.
// Side effects: cleanup cancels the Remote and joins every owned peer goroutine.
func startTestRemote(t *testing.T, serve func(net.Conn, int)) *Remote {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	r := &Remote{ctx: ctx, cancel: cancel, pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	var peers sync.WaitGroup
	attempt := 0
	r.dial = func(ctx context.Context) (io.ReadWriteCloser, error) {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		client, server := net.Pipe()
		_ = server.SetDeadline(time.Now().Add(10 * time.Second))
		n := attempt
		attempt++
		peers.Go(func() {
			defer server.Close()
			serve(server, n)
		})
		return client, nil
	}
	done := make(chan struct{})
	go func() { r.run(); close(done) }()
	t.Cleanup(func() { r.Close(); <-done; peers.Wait() })
	return r
}

// awaitTestSnapshot waits on subscription notifications rather than polling transport state.
func awaitTestSnapshot(t *testing.T, r *Remote, condition func(Snapshot) bool) {
	t.Helper()
	changed := make(chan struct{}, 1)
	unsubscribe := r.Subscribe(func() {
		select {
		case changed <- struct{}{}:
		default:
		}
	})
	defer unsubscribe()
	for !condition(r.Snapshot()) {
		select {
		case <-changed:
		case <-r.ctx.Done():
			t.Fatal("transport did not publish expected snapshot")
		}
	}
}

// TestMutationLostReplyRetry preserves one logical command across a new bootstrap revision (INV-12).
func TestMutationLostReplyRetry(t *testing.T) {
	requests := make(chan testRPCRequest, 2)
	r := startTestRemote(t, func(conn net.Conn, attempt int) {
		if attempt > 1 {
			t.Error("unexpected third connection")
			return
		}
		p := projection{Boot: "same-daemon", Seq: "7", Revision: "7", Snapshot: Snapshot{Daemon: Daemon{StateAvailable: true, StartupState: StartupNormal}}}
		if attempt == 1 {
			p.Seq, p.Revision = "8", "8"
		}
		if !bootstrapTestPeer(t, conn, p) {
			return
		}
		req, ok := readTestRequest(t, conn, "endpoints.requestSwitch")
		if !ok {
			return
		}
		requests <- req
		if attempt == 0 {
			// Model a committed command whose reply is lost, not a rejected command.
			return
		}
		if !replyTestRequest(t, conn, req, map[string]any{"operationId": "durable-switch", "state": "running"}) {
			return
		}
		if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
			t.Errorf("unexpected work after retry acceptance: %v", err)
		}
	})
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.Connected })
	id, err := r.RequestSwitch("account-original", "endpoint-original")
	if err != nil || id != "durable-switch" {
		t.Fatalf("retried acceptance: %q %v", id, err)
	}
	first, retry := <-requests, <-requests
	key, ok := first.Params["operationKey"].(string)
	if !ok || key == "" {
		t.Fatal("mutation has no operation key")
	}
	expected := map[string]any{"accountId": "account-original", "endpointId": "endpoint-original", "expectedRevision": "7", "operationKey": key}
	if !reflect.DeepEqual(first.Params, expected) || !reflect.DeepEqual(retry.Params, expected) {
		t.Fatalf("retry changed logical command after revision 8 bootstrap: first=%+v retry=%+v", first.Params, retry.Params)
	}
}

// TestWaitOperationCancellation keeps accepted work observable after an in-flight poll is abandoned (INV-12).
func TestWaitOperationCancellation(t *testing.T) {
	pollStarted := make(chan struct{})
	continued := make(chan struct{})
	requests := make(chan testRPCRequest, 3)
	r := startTestRemote(t, func(conn net.Conn, attempt int) {
		if attempt > 1 {
			t.Error("unexpected connection after resumed observation")
			return
		}
		p := projection{Boot: "same-daemon", Seq: "1", Revision: "1", Snapshot: Snapshot{Daemon: Daemon{StateAvailable: true, StartupState: StartupNormal}}}
		if attempt == 1 {
			p.Snapshot.Daemon.Version = "after-cancel"
		}
		if !bootstrapTestPeer(t, conn, p) {
			return
		}
		if attempt == 0 {
			req, ok := readTestRequest(t, conn, "endpoints.requestSwitch")
			if !ok {
				return
			}
			requests <- req
			if !replyTestRequest(t, conn, req, map[string]any{"operationId": "still-owned-by-daemon", "state": "running"}) {
				return
			}
		}
		req, ok := readTestRequest(t, conn, "operations.get")
		if !ok {
			return
		}
		requests <- req
		if attempt == 0 {
			close(pollStarted)
			if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
				t.Errorf("cancelled wait sent a command instead of closing its poll: %v", err)
				return
			}
			// The daemon finishes independently; no cancellation or second mutation is needed.
			close(continued)
			return
		}
		if !replyTestRequest(t, conn, req, map[string]any{"operationId": "still-owned-by-daemon", "state": "succeeded"}) {
			return
		}
		if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
			t.Errorf("unexpected work after completion: %v", err)
		}
	})
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.Connected })
	id, err := r.RequestSwitch("account", "endpoint")
	if err != nil || id != "still-owned-by-daemon" {
		t.Fatalf("acceptance: %q %v", id, err)
	}
	waitCtx, cancelWait := context.WithCancel(r.ctx)
	defer cancelWait()
	waitDone := make(chan error, 1)
	go func() { waitDone <- r.WaitOperation(waitCtx, id) }()
	select {
	case <-pollStarted:
	case <-r.ctx.Done():
		t.Fatal("operation poll missing")
	}
	cancelWait()
	select {
	case err := <-waitDone:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("cancelled wait returned %v", err)
		}
	case <-r.ctx.Done():
		t.Fatal("wait did not stop after cancellation")
	}
	select {
	case <-continued:
	case <-r.ctx.Done():
		t.Fatal("daemon did not retain accepted work")
	}
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.Connected && s.Daemon.Version == "after-cancel" })
	if err := r.WaitOperation(r.ctx, id); err != nil {
		t.Fatalf("resumed observation: %v", err)
	}
	for i := range 3 {
		req := <-requests
		if i == 0 {
			if req.Method != "endpoints.requestSwitch" {
				t.Fatal("missing original mutation")
			}
		} else if req.Method != "operations.get" || !reflect.DeepEqual(req.Params, map[string]any{"operationId": id}) {
			t.Fatalf("wait resubmitted or cancelled work: %+v", req)
		}
	}
}

// TestHelloManifestBoundary binds emitted negotiation to the manifest and refuses incompatible majors (§13.3).
func TestHelloManifestBoundary(t *testing.T) {
	body, err := os.ReadFile("../../protocol/contract.json")
	if err != nil {
		t.Fatal(err)
	}
	var contract struct{ Protocol map[string]any }
	if err := json.Unmarshal(body, &contract); err != nil {
		t.Fatal(err)
	}
	for _, compatible := range []bool{true, false} {
		name := "compatible"
		if !compatible {
			name = "incompatible-major"
		}
		t.Run(name, func(t *testing.T) {
			rejected := make(chan struct{})
			r := startTestRemote(t, func(conn net.Conn, attempt int) {
				if attempt != 0 {
					t.Error("unexpected reconnect during handshake assertion")
					return
				}
				req, ok := readTestRequest(t, conn, "session.hello")
				if !ok {
					return
				}
				if !reflect.DeepEqual(req.Params, map[string]any{"protocol": contract.Protocol}) {
					t.Errorf("hello differs from manifest: %+v", req.Params)
				}
				protocol := map[string]any{"major": contract.Protocol["major"], "minor": contract.Protocol["minor"]}
				if !compatible {
					protocol["major"] = contract.Protocol["major"].(float64) + 1
				}
				if !replyTestRequest(t, conn, req, map[string]any{"protocol": protocol}) {
					return
				}
				if !compatible {
					if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
						t.Errorf("incompatible major allowed further RPC: %v", err)
						return
					}
					close(rejected)
					return
				}
				req, ok = readTestRequest(t, conn, "state.bootstrap")
				if !ok || !replyTestRequest(t, conn, req, projection{Boot: "compatible-boot", Seq: "0", Revision: "0", Snapshot: Snapshot{Daemon: Daemon{StateAvailable: true, StartupState: StartupNormal}}}) {
					return
				}
				if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
					t.Errorf("unexpected request after bootstrap: %v", err)
				}
			})
			if compatible {
				awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.Connected })
				return
			}
			select {
			case <-rejected:
			case <-r.ctx.Done():
				t.Fatal("incompatible major was not refused")
			}
			if r.Snapshot().Daemon.Connected || !errors.Is(r.PauseAll(), ErrBusy) {
				t.Fatal("incompatible session admitted a mutation")
			}
		})
	}
}

// TestProjectionIsolationAndDisconnect rejects stale/foreign state and removes disconnected green status (§13.6).
func TestProjectionIsolationAndDisconnect(t *testing.T) {
	stale, foreign := make(chan struct{}), make(chan struct{})
	r := startTestRemote(t, func(conn net.Conn, attempt int) {
		if attempt != 0 {
			t.Error("unexpected reconnect during projection assertion")
			return
		}
		p := projection{Boot: "current-boot", Seq: "10", Revision: "3", Snapshot: Snapshot{
			Daemon:    Daemon{StateAvailable: true, StartupState: StartupNormal},
			Folders:   []Folder{{ID: "folder", State: FolderUpToDate}},
			Endpoints: []Endpoint{{ID: "endpoint", Reachable: true, InUse: true}},
		}}
		if !bootstrapTestPeer(t, conn, p) {
			return
		}
		<-stale
		p.Seq, p.Revision, p.Snapshot.Daemon.AllPaused = "9", "2", true
		p.Snapshot.Folders[0].State = FolderError
		if err := writeFrame(conn, map[string]any{"jsonrpc": "2.0", "method": "event", "params": p}); err != nil {
			t.Error(err)
			return
		}
		// A following correlated reply proves the stale event has passed through read().
		req, ok := readTestRequest(t, conn, "diagnostics.snapshot")
		if !ok || !replyTestRequest(t, conn, req, map[string]any{}) {
			return
		}
		<-foreign
		p.Boot, p.Seq = "foreign-boot", "11"
		if err := writeFrame(conn, map[string]any{"jsonrpc": "2.0", "method": "event", "params": p}); err != nil {
			t.Error(err)
			return
		}
		if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
			t.Errorf("foreign-boot event did not close transport: %v", err)
		}
	})
	// Ensure failure cleanup can also release peers waiting for the next scenario step.
	defer func() {
		select {
		case <-stale:
		default:
			close(stale)
		}
		select {
		case <-foreign:
		default:
			close(foreign)
		}
	}()
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.Connected })
	close(stale)
	if _, err := r.Diagnostics(r.ctx); err != nil {
		t.Fatal(err)
	}
	s := r.Snapshot()
	if !s.Daemon.Connected || s.Daemon.AllPaused || s.Folders[0].State != FolderUpToDate {
		t.Fatalf("stale projection overwrote live state: %+v", s)
	}
	close(foreign)
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return !s.Daemon.Connected })
	s = r.Snapshot()
	if s.Daemon.AllPaused || s.Folders[0].State != FolderOffline || s.Endpoints[0].Reachable || s.Endpoints[0].InUse {
		t.Fatalf("foreign/disconnected projection retained green status: %+v", s)
	}
}

// TestDisconnectedIsNotBusy keeps a missing daemon from reading as a daemon at an unsafe point:
// the paired-daemon fixture once reported every call "busy" while its daemon had exited.
func TestDisconnectedIsNotBusy(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	r := &Remote{ctx: ctx, cancel: cancel, pending: map[string]chan rpcReply{}, listeners: map[uint64]func(){}}
	r.dial = func(context.Context) (io.ReadWriteCloser, error) { return nil, os.ErrNotExist }
	go r.run()
	defer r.Close()
	_, diagnostics := r.Diagnostics(ctx)
	for name, err := range map[string]error{"mutation": r.PauseAll(), "call": diagnostics} {
		if !errors.Is(err, ErrDisconnected) || !errors.Is(err, ErrBusy) || err.Error() == ErrBusy.Error() {
			t.Errorf("%s without a connection: %v", name, err)
		}
	}
}

// assertUnknownSnapshot protects unavailable state from becoming stale green or empty-success state (INV-07).
func assertUnknownSnapshot(t *testing.T, s Snapshot, state StartupState) {
	t.Helper()
	if s.Daemon.Connected || s.Daemon.StateAvailable || s.Daemon.StartupState != state {
		t.Fatalf("unavailable daemon became healthy: %+v", s.Daemon)
	}
	if s.Accounts != nil || s.Folders != nil || s.Endpoints != nil || s.Networks != nil || s.Switches != nil || s.Decisions != nil || s.Activity != nil {
		t.Fatalf("unavailable state retained known collections: %+v", s)
	}
	body, err := json.Marshal(s)
	if err != nil {
		t.Fatal(err)
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(body, &fields); err != nil {
		t.Fatal(err)
	}
	if len(fields) != 1 || fields["daemon"] == nil {
		t.Fatalf("unknown collections were serialized as known state: %s", body)
	}
}

// TestUnavailableBootstrapRPCs keeps diagnostic/stop access without authorizing state-sensitive work (INV-07/12).
func TestUnavailableBootstrapRPCs(t *testing.T) {
	body, err := os.ReadFile("../../protocol/startup.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples struct {
		DatabaseUnavailable projection
		StateRefusal        struct{ Error json.RawMessage }
	}
	if err := json.Unmarshal(body, &examples); err != nil {
		t.Fatal(err)
	}
	for _, state := range []StartupState{StartupBackoff, StartupDiagnostic, StartupStopped, StartupDatabaseUnavailable} {
		t.Run(string(state), func(t *testing.T) {
			r := startTestRemote(t, func(conn net.Conn, attempt int) {
				if attempt != 0 {
					t.Error("unexpected reconnect during diagnostic RPCs")
					return
				}
				p := examples.DatabaseUnavailable
				p.Snapshot.Daemon.StartupState = state
				if !bootstrapTestPeer(t, conn, p) {
					return
				}
				for _, method := range []string{"diagnostics.snapshot", "folders.list", "accounts.login.start", "operations.get", "daemon.shutdown"} {
					req, ok := readTestRequest(t, conn, method)
					if !ok {
						return
					}
					switch method {
					case "diagnostics.snapshot":
						if !replyTestRequest(t, conn, req, map[string]any{"protocol": "1.8", "stateAvailable": false, "startupState": state, "reasonCode": "STATE_UNAVAILABLE"}) {
							return
						}
					case "daemon.shutdown":
						if req.Params["reason"] != "user" || req.Params["operationKey"] == "" || req.Params["operationKey"] == nil || req.Params["expectedRevision"] != "0" {
							t.Errorf("diagnostic shutdown lost explicit durable command fields: %+v", req.Params)
						}
						if !replyTestRequest(t, conn, req, map[string]any{"operationId": "durable-stop", "state": "succeeded", "result": map[string]any{}}) {
							return
						}
					default:
						if err := writeFrame(conn, map[string]any{"jsonrpc": "2.0", "id": req.ID, "error": examples.StateRefusal.Error}); err != nil {
							t.Error(err)
							return
						}
					}
				}
				if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
					t.Errorf("unexpected work after explicit shutdown: %v", err)
				}
			})
			awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.StartupState == state })
			assertUnknownSnapshot(t, r.Snapshot(), state)
			d, err := r.Diagnostics(r.ctx)
			if err != nil || d.StateAvailable || d.StartupState != state || d.ReasonCode != "STATE_UNAVAILABLE" || d.Protocol != "1.8" || d.Folders != nil {
				t.Fatalf("diagnostic access: %+v %v", d, err)
			}
			folders, readErr := r.RemoteFolders("unknown-account")
			_, loginErr := r.StartLogin("https://primary.example.invalid/")
			waitErr := r.WaitOperation(r.ctx, "previous-operation")
			if folders != nil {
				t.Fatal("refused folder read became a successful empty list")
			}
			for _, err := range []error{readErr, loginErr, waitErr} {
				if !errors.Is(err, ErrStateUnavailable) {
					t.Fatalf("unavailable RPC returned %v", err)
				}
				if err.Error() != ErrStateUnavailable.Error() {
					t.Fatalf("unavailable error exposed backend text: %v", err)
				}
			}
			if err := r.Shutdown(); err != nil {
				t.Fatalf("diagnostic explicit shutdown: %v", err)
			}
			assertUnknownSnapshot(t, r.Snapshot(), state)
		})
	}
}

// TestUnavailableTransitionFreshState discards cached roots and preserves known-empty lists after recovery (INV-07/13).
func TestUnavailableTransitionFreshState(t *testing.T) {
	body, err := os.ReadFile("../../protocol/startup.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples struct{ Folder Folder }
	if err := json.Unmarshal(body, &examples); err != nil {
		t.Fatal(err)
	}
	events := make(chan projection)
	defer close(events)
	r := startTestRemote(t, func(conn net.Conn, attempt int) {
		if attempt != 0 {
			t.Error("unexpected reconnect during startup transitions")
			return
		}
		p := projection{Boot: "same-boot", Seq: "1", Revision: "1", Snapshot: Snapshot{
			Daemon:   Daemon{StateAvailable: true, StartupState: StartupNormal},
			Accounts: []Account{{ID: "old-account", SignedIn: true}}, Folders: []Folder{{ID: "old-folder", State: FolderUpToDate}},
			Endpoints: []Endpoint{{ID: "old-endpoint", Reachable: true, InUse: true}}, Networks: []Network{{AccountID: "old-account"}},
			Switches: []Switch{{AccountID: "old-account"}}, Decisions: []Decision{{ID: "old-decision"}}, Activity: []Activity{{Cursor: "1"}},
		}}
		if !bootstrapTestPeer(t, conn, p) {
			return
		}
		for event := range events {
			if err := writeFrame(conn, map[string]any{"jsonrpc": "2.0", "method": "event", "params": event}); err != nil {
				t.Error(err)
				return
			}
			req, ok := readTestRequest(t, conn, "diagnostics.snapshot")
			if !ok || !replyTestRequest(t, conn, req, map[string]any{}) {
				return
			}
		}
	})
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.Connected })
	events <- projection{Boot: "same-boot", Seq: "2", Revision: "1", Snapshot: Snapshot{Daemon: Daemon{StartupState: StartupDiagnostic}}}
	if _, err := r.Diagnostics(r.ctx); err != nil {
		t.Fatal(err)
	}
	assertUnknownSnapshot(t, r.Snapshot(), StartupDiagnostic)
	fresh := Snapshot{
		Daemon: Daemon{StateAvailable: true, StartupState: StartupNormal}, Accounts: []Account{},
		Folders:   []Folder{examples.Folder},
		Endpoints: []Endpoint{}, Networks: []Network{}, Switches: []Switch{}, Decisions: []Decision{}, Activity: []Activity{},
	}
	events <- projection{Boot: "same-boot", Seq: "3", Revision: "2", Snapshot: fresh}
	if _, err := r.Diagnostics(r.ctx); err != nil {
		t.Fatal(err)
	}
	s := r.Snapshot()
	if !s.Daemon.Connected || !s.Daemon.StateAvailable || s.Daemon.StartupState != StartupNormal || !reflect.DeepEqual(s.Folders, fresh.Folders) || s.Folders[0].ReasonCode != "JOURNAL_ERROR" || !s.Folders[0].UserPaused || s.Folders[0].State != FolderPaused {
		t.Fatalf("recovery did not use fresh paused/blocker projection: %+v", s)
	}
	if s.Accounts == nil || len(s.Accounts) != 0 || s.Endpoints == nil || len(s.Endpoints) != 0 || s.Networks == nil || len(s.Networks) != 0 || s.Switches == nil || len(s.Switches) != 0 || s.Decisions == nil || len(s.Decisions) != 0 || s.Activity == nil || len(s.Activity) != 0 {
		t.Fatalf("recovery confused proven-empty lists with old or unknown state: %+v", s)
	}
}

// TestDiagnosticCancellationRecovery keeps cancellation local and bootstraps fresh known state after reconnect (INV-07/12).
func TestDiagnosticCancellationRecovery(t *testing.T) {
	started := make(chan struct{})
	r := startTestRemote(t, func(conn net.Conn, attempt int) {
		if attempt > 1 {
			t.Error("unexpected reconnect after diagnostic cancellation")
			return
		}
		p := projection{Boot: "diagnostic-boot", Seq: "9", Revision: "4", Snapshot: Snapshot{Daemon: Daemon{StartupState: StartupDatabaseUnavailable}}}
		if attempt == 1 {
			p = projection{Boot: "recovered-boot", Seq: "0", Revision: "0", Snapshot: Snapshot{
				Daemon: Daemon{StateAvailable: true, StartupState: StartupNormal}, Accounts: []Account{}, Folders: []Folder{},
				Endpoints: []Endpoint{}, Networks: []Network{}, Switches: []Switch{}, Decisions: []Decision{}, Activity: []Activity{},
			}}
		}
		if !bootstrapTestPeer(t, conn, p) {
			return
		}
		req, ok := readTestRequest(t, conn, "diagnostics.snapshot")
		if !ok {
			return
		}
		if attempt == 0 {
			close(started)
		} else if !replyTestRequest(t, conn, req, map[string]any{"stateAvailable": true, "startupState": StartupNormal}) {
			return
		}
		if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
			t.Errorf("diagnostic cancellation sent unexpected work: %v", err)
		}
	})
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.StartupState == StartupDatabaseUnavailable })
	assertUnknownSnapshot(t, r.Snapshot(), StartupDatabaseUnavailable)
	ctx, cancel := context.WithCancel(r.ctx)
	defer cancel()
	done := make(chan error, 1)
	go func() { _, err := r.Diagnostics(ctx); done <- err }()
	select {
	case <-started:
	case <-r.ctx.Done():
		t.Fatal("diagnostic request missing")
	}
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("diagnostic cancellation: %v", err)
		}
	case <-r.ctx.Done():
		t.Fatal("diagnostic cancellation did not return")
	}
	awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.Connected })
	s := r.Snapshot()
	if !s.Daemon.StateAvailable || s.Daemon.StartupState != StartupNormal || s.Accounts == nil || len(s.Accounts) != 0 || s.Folders == nil || len(s.Folders) != 0 {
		t.Fatalf("new boot did not restore fresh known state: %+v", s)
	}
	d, err := r.Diagnostics(r.ctx)
	if err != nil || !d.StateAvailable || d.StartupState != StartupNormal {
		t.Fatalf("diagnostics after fresh bootstrap: %+v %v", d, err)
	}
}

// TestAvailabilityHelloCompatibility infers legacy availability only from an explicit pre-1.7 hello (§13.3).
func TestAvailabilityHelloCompatibility(t *testing.T) {
	for _, tc := range []struct {
		name      string
		minor     int
		available bool
	}{
		{"legacy-1.6", 6, false},
		{"current-missing-state", 7, false},
		{"current-missing-startup", 7, true},
		{"missing-minor", -1, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := startTestRemote(t, func(conn net.Conn, attempt int) {
				if attempt != 0 {
					t.Error("unexpected reconnect during availability compatibility")
					return
				}
				req, ok := readTestRequest(t, conn, "session.hello")
				protocol := map[string]int{"major": 1}
				if tc.minor >= 0 {
					protocol["minor"] = tc.minor
				}
				if !ok || !replyTestRequest(t, conn, req, map[string]any{"protocol": protocol}) {
					return
				}
				req, ok = readTestRequest(t, conn, "state.bootstrap")
				// Missing mandatory fields are legacy-compatible only after explicit old-minor negotiation.
				daemon := map[string]any{"version": "availability-probe"}
				if tc.available {
					daemon["stateAvailable"] = true
				}
				p := map[string]any{"bootId": "compatibility-boot", "seq": "0", "revision": "0", "snapshot": map[string]any{
					"daemon": daemon, "accounts": []any{}, "folders": []any{},
				}}
				if !ok || !replyTestRequest(t, conn, req, p) {
					return
				}
				req, ok = readTestRequest(t, conn, "diagnostics.snapshot")
				if !ok || !replyTestRequest(t, conn, req, map[string]any{}) {
					return
				}
				if _, err := readFrame(conn); !errors.Is(err, io.EOF) {
					t.Errorf("unexpected compatibility request: %v", err)
				}
			})
			awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.Version == "availability-probe" })
			s := r.Snapshot()
			if tc.minor == 6 {
				if !s.Daemon.Connected || !s.Daemon.StateAvailable || s.Daemon.StartupState != StartupNormal || s.Accounts == nil || s.Folders == nil {
					t.Fatalf("explicit legacy hello was not projected compatibly: %+v", s)
				}
			} else {
				assertUnknownSnapshot(t, s, "")
			}
			d, err := r.Diagnostics(r.ctx)
			if err != nil || d.StateAvailable != (tc.minor == 6) {
				t.Fatalf("diagnostic compatibility: %+v %v", d, err)
			}
		})
	}
}

// TestMockStartupState keeps explicit demo profiles distinct from unavailable connected state.
func TestMockStartupState(t *testing.T) {
	for _, sample := range []bool{false, true} {
		s := NewMock(sample, nil).Snapshot()
		if !s.Daemon.Demo || !s.Daemon.StateAvailable || s.Daemon.StartupState != StartupNormal {
			t.Fatalf("demo startup state: %+v", s.Daemon)
		}
		if s.Accounts == nil || s.Folders == nil || s.Endpoints == nil || s.Networks == nil || s.Switches == nil || s.Decisions == nil || s.Activity == nil {
			t.Fatal("normal demo state contains unknown collections")
		}
	}
}

// TestServerConstraintErrors preserves permission refusal versus retryable server constraints (PLAN §12.1/N18).
func TestServerConstraintErrors(t *testing.T) {
	data, err := os.ReadFile("../../protocol/constraints.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples map[string]rpcReply
	if err := json.Unmarshal(data, &examples); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name string
		want error
	}{
		{"permission", ErrRefused},
		{"lock", ErrBusy},
		{"throttle", ErrBusy},
	} {
		t.Run(tc.name, func(t *testing.T) {
			reply, ok := examples[tc.name]
			if !ok || reply.Error == nil || !errors.Is(reply.Error, tc.want) {
				t.Fatalf("server constraint lost its refusal/retry semantics: %v", reply.Error)
			}
		})
	}
}

// TestNonNormalStartupCannotBeHealthy refuses contradictory availability at the pipe boundary (INV-07/13).
func TestNonNormalStartupCannotBeHealthy(t *testing.T) {
	for _, state := range []StartupState{StartupBackoff, StartupDiagnostic, StartupStopped, StartupDatabaseUnavailable, "unexpected"} {
		t.Run(string(state), func(t *testing.T) {
			r := &Remote{}
			p := projection{Boot: "contradictory-startup", Seq: "0", Revision: "0", Snapshot: Snapshot{
				Daemon:   Daemon{StateAvailable: true, StartupState: state},
				Accounts: []Account{}, Folders: []Folder{},
			}}
			if err := r.apply(p, true); err != nil {
				t.Fatal(err)
			}
			assertUnknownSnapshot(t, r.Snapshot(), state)
		})
	}
}

// TestProtocol19StateExample decodes the 1.9 snapshot example: per-account
// sign-in state, folder phase and progress, address latency, kind and block
// reason, why an address was chosen, deletion groups, conflicts and the
// start report all reach the interface's types.
func TestProtocol19StateExample(t *testing.T) {
	b, err := os.ReadFile("../../protocol/state.examples.json")
	if err != nil {
		t.Fatal(err)
	}
	var examples struct {
		Event struct {
			Params projection `json:"params"`
		} `json:"event"`
		PrepareUpdatePostponed struct {
			Error *rpcError `json:"error"`
		} `json:"prepareUpdatePostponed"`
		AccountExists struct {
			Result struct {
				Reason string `json:"reasonCode"`
			} `json:"result"`
		} `json:"accountExists"`
		VersionReport struct {
			Protocol struct{ Minor int } `json:"protocol"`
		} `json:"versionReport"`
	}
	if err := json.Unmarshal(b, &examples); err != nil {
		t.Fatal(err)
	}
	s := examples.Event.Params.Snapshot
	if len(s.Accounts) != 2 || s.Accounts[0].Auth != AuthSignedIn || s.Accounts[1].Auth != AuthRevoked || s.Accounts[1].SignedIn {
		t.Fatalf("account sign-in state: %+v", s.Accounts)
	}
	if f := s.Folders[0]; f.Phase != PhaseUploading || f.FilesDone != 12 || s.Folders[1].Phase != "" {
		t.Fatalf("folder phase and progress: %+v", s.Folders)
	}
	if e := s.Endpoints[1]; e.Kind != KindHome || e.LatencyMs != 6 || e.Blocked || e.BlockedReason != "" {
		t.Fatalf("address latency and kind: %+v", e)
	}
	if e := s.Endpoints[2]; !e.Blocked || e.BlockedReason != "ENDPOINT_IDENTITY_MISMATCH" {
		t.Fatalf("address block reason: %+v", e)
	}
	if n := s.Networks[0]; len(n.Why) != 1 || n.Why[0] != ReasonFaster || n.SwitchReason != ReasonFaster || n.Switched.IsZero() {
		t.Fatalf("why an address was chosen: %+v", n)
	}
	d := s.Decisions[0]
	if d.Kind != DecisionBulkDeletions || len(d.Groups) != 2 || d.Groups[0].Name != "2024" || d.Groups[0].Files != 12 || !d.Groups[1].Loose {
		t.Fatalf("deletion groups: %+v", d)
	}
	if c := s.Decisions[1]; c.Kind != DecisionConflict || c.File != "Budget.xlsx" || c.Copy == "" {
		t.Fatalf("conflict decision: %+v", c)
	}
	if r := s.Daemon.StartupReport; r.PreviousSchema != 10 || r.Schema != 11 || r.SelfCheck != "failed" || len(r.ReasonCodes) != 1 || !s.Daemon.Capabilities["engine"] {
		t.Fatalf("start report and engine capability: %+v", s.Daemon)
	}
	if !errors.Is(examples.PrepareUpdatePostponed.Error, ErrBusy) {
		t.Fatal("a deletion waiting for the user does not postpone an update as busy")
	}
	if !errors.Is(&rpcError{Data: struct {
		Reason string `json:"reasonCode"`
	}{examples.AccountExists.Result.Reason}}, ErrAccountExists) {
		t.Fatal("ACCOUNT_EXISTS is not ErrAccountExists")
	}
	if examples.VersionReport.Protocol.Minor != protocolMinor {
		t.Fatal("version report example does not name this client's protocol")
	}
}
