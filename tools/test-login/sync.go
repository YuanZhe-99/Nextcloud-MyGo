// sync.go exercises real daemon folders with uniquely owned local and remote fixtures.
package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
	"unicode"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
)

// fixtureDAV sends only approved-origin requests and never includes raw URLs or responses in errors.
func fixtureDAV(method, path string, content []byte, expected ...int) []byte {
	return fixtureDAVLimit(method, path, content, 1048576, expected...)
}

// fixtureDAVLimit bounds owned large-content verification separately from small API response parsing.
func fixtureDAVLimit(method, path string, content []byte, maximum int64, expected ...int) []byte {
	base, err := url.Parse(os.Getenv("NEXTCLOUD_TEST_URL"))
	if err != nil || base.Scheme != "https" || base.User != nil {
		panic("invalid fixture origin")
	}
	target := base.JoinPath(path)
	req, err := http.NewRequest(method, target.String(), bytes.NewReader(content))
	if err != nil {
		panic("fixture request construction failed")
	}
	req.SetBasicAuth(os.Getenv("NEXTCLOUD_TEST_USER"), os.Getenv("NEXTCLOUD_TEST_PASSWORD"))
	req.Header.Set("OCS-APIRequest", "true")
	req.Header.Set("Accept", "application/json")
	client := &http.Client{Timeout: 20 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	readCtx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	req = req.WithContext(readCtx)
	var response *http.Response
	for attempt := 0; ; attempt++ {
		response, err = client.Do(req)
		if err != nil || method != "GET" || response.StatusCode != http.StatusLocked || attempt == 8 {
			break
		}
		// Positive readback can race a real daemon write lock. Never replay a mutating DAV request here.
		response.Body.Close()
		select {
		case <-readCtx.Done():
			panic("fixture locked read deadline")
		case <-time.After(500 * time.Millisecond):
		}
	}
	if err != nil {
		panic("fixture request transport failed")
	}
	defer response.Body.Close()
	accepted := false
	for _, code := range expected {
		accepted = accepted || response.StatusCode == code
	}
	if !accepted {
		panic(fmt.Sprintf("fixture %s returned HTTP %d", method, response.StatusCode))
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, maximum+1))
	if err != nil || int64(len(body)) > maximum {
		panic("fixture response invalid")
	}
	return body
}

// fixtureHome reads the fixture account UID without exposing it in output or saved metadata.
func fixtureHome() string {
	var response struct {
		OCS struct {
			Data struct {
				ID string `json:"id"`
			} `json:"data"`
		} `json:"ocs"`
	}
	body := fixtureDAV("GET", "ocs/v2.php/cloud/user", nil, 200)
	// The endpoint chooses JSON from the explicit Accept header below via the OCS request.
	if json.Unmarshal(body, &response) != nil || response.OCS.Data.ID == "" {
		panic("fixture UID lookup failed")
	}
	return "remote.php/dav/files/" + response.OCS.Data.ID + "/"
}

// retryCommand retries only an optimistic revision conflict, never an uncertain destructive operation.
func retryCommand(ctx context.Context, command func() error) {
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	for {
		err := command()
		if err == nil {
			return
		}
		if !errors.Is(err, syncd.ErrStale) {
			panic(err)
		}
		select {
		case <-ctx.Done():
			fail("fixture command retry (step deadline)")
		case <-time.After(50 * time.Millisecond):
		}
	}
}

// stepBudget bounds every single fixture step, so a hang names its step instead of a whole phase.
const stepBudget = 90 * time.Second

// observed is the daemon connection whose sanitized state a failing step reports.
var observed *syncd.Remote

// daemonState summarizes the daemon's last known state without paths, URLs, names or credentials.
func daemonState() string {
	if observed == nil {
		return "daemon unobserved"
	}
	s := observed.Snapshot()
	out := fmt.Sprintf("daemon connected=%v startup=%s paused=%v", s.Daemon.Connected, s.Daemon.StartupState, s.Daemon.AllPaused)
	for i, f := range s.Folders {
		out += fmt.Sprintf("; folder%d=%s/%s userPaused=%v", i, f.State, f.ReasonCode, f.UserPaused)
	}
	for _, n := range s.Networks {
		out += fmt.Sprintf("; switching=%v", n.Switching)
	}
	return out + fmt.Sprintf("; decisions=%d; lastDisconnect=%s", len(s.Decisions), observed.Disconnect())
}

// fail ends the fixture with the step name and the daemon's sanitized last state.
func fail(step string) {
	panic("step: " + step + "; " + daemonState())
}

// await waits for one observable domain condition within its own step budget.
func await(ctx context.Context, condition func() bool, message string) {
	ctx, cancel := context.WithTimeout(ctx, stepBudget)
	defer cancel()
	for !condition() {
		select {
		case <-ctx.Done():
			fail(message + " (step deadline)")
		case <-time.After(100 * time.Millisecond):
		}
	}
}

// waitOperation waits for one receipt within its own step budget.
func waitOperation(ctx context.Context, r *syncd.Remote, id string) error {
	ctx, cancel := context.WithTimeout(ctx, stepBudget)
	defer cancel()
	return r.WaitOperation(ctx, id)
}

// waitLogin waits for one login session within its own step budget.
// Notes: a renewal holds the old password's revocation for at least the server's 60-second
// floor, longer when the server answers 429, and a slow shared server takes seconds per request.
func waitLogin(ctx context.Context, r *syncd.Remote, id string) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, stepBudget+3*time.Minute)
	defer cancel()
	return r.WaitLogin(ctx, id)
}

// exerciseSync verifies two real daemon mappings, pause isolation and deletion-proposal approval.
func exerciseSync(ctx context.Context, r *syncd.Remote, account string, s state, file string) {
	home := fixtureHome()
	local := filepath.Join(filepath.Dir(file), s.Profile)
	for _, suffix := range []string{"a", "b"} {
		name := "nextcloud-mygo-" + s.Profile + "-" + suffix
		fixtureDAV("MKCOL", home+name, nil, 201)
		dir := filepath.Join(local, suffix)
		if os.MkdirAll(dir, 0700) != nil {
			panic("local fixture creation failed")
		}
		if os.WriteFile(filepath.Join(dir, "upload.txt"), []byte("upload-owned"), 0600) != nil {
			panic("local fixture write failed")
		}
		for i := 0; i < 12; i++ {
			if os.WriteFile(filepath.Join(dir, fmt.Sprintf("delete-%02d.txt", i)), []byte("owned-delete-fixture"), 0600) != nil {
				panic("deletion fixture creation failed")
			}
		}
		fixtureDAV("PUT", home+name+"/download.txt", []byte("download-owned"), 201)
		retryCommand(ctx, func() error { return r.AddFolder(account, "/"+name, dir) })
	}
	await(ctx, func() bool {
		snap := r.Snapshot()
		if len(snap.Folders) != 2 {
			return false
		}
		for _, f := range snap.Folders {
			if f.State != syncd.FolderUpToDate {
				return false
			}
		}
		return true
	}, "two-folder initial sync deadline")
	folders := r.Snapshot().Folders
	if alternate := os.Getenv("NEXTCLOUD_TEST_ALTERNATE_URL"); alternate != "" {
		exerciseEndpoints(ctx, r, account, alternate, s.Profile)
	}
	for _, f := range folders {
		if string(fixtureDAV("GET", home+f.RemotePath[1:]+"/upload.txt", nil, 200)) != "upload-owned" {
			panic("daemon upload mismatch")
		}
		body, err := os.ReadFile(filepath.Join(f.LocalPath, "download.txt"))
		if err != nil || string(body) != "download-owned" {
			panic("daemon download mismatch")
		}
	}
	watchRoot := filepath.Join(folders[0].LocalPath, "watcher", "new-subtree")
	if os.MkdirAll(watchRoot, 0700) != nil || os.WriteFile(filepath.Join(watchRoot, "change.txt"), []byte("recursive-watch-owned"), 0600) != nil {
		panic("recursive watcher fixture creation failed")
	}
	watchCtx, cancelWatch := context.WithTimeout(ctx, 25*time.Second)
	defer cancelWatch()
	await(watchCtx, func() bool {
		return string(fixtureDAV("GET", home+folders[0].RemotePath[1:]+"/watcher/new-subtree/change.txt", nil, 200, 404)) == "recursive-watch-owned"
	}, "recursive local change did not upload without a sync command")
	fmt.Println("PASS new nested local file uploaded without a sync command within the watcher deadline")
	retryCommand(ctx, func() error { return r.PauseFolder(folders[0].ID) })
	retryCommand(ctx, r.PauseAll)
	retryCommand(ctx, r.ResumeAll)
	await(ctx, func() bool {
		f, _ := r.Snapshot().Folder(folders[0].ID)
		return f.UserPaused && f.State == syncd.FolderPaused
	}, "folder pause lost on resume all")
	retryCommand(ctx, func() error { return r.ResumeFolder(folders[0].ID) })
	retryCommand(ctx, r.PauseAll)
	await(ctx, func() bool {
		for _, f := range r.Snapshot().Folders {
			if f.State == syncd.FolderSyncing {
				return false
			}
		}
		return true
	}, "fixture pause did not quiesce")
	target := folders[0]
	for i := 0; i < 12; i++ {
		if os.Remove(filepath.Join(target.LocalPath, fmt.Sprintf("delete-%02d.txt", i))) != nil {
			panic("owned deletion setup failed")
		}
	}
	retryCommand(ctx, r.ResumeAll)
	retryCommand(ctx, func() error { return r.SyncNow(target.ID) })
	await(ctx, func() bool { return len(r.Snapshot().Decisions) > 0 }, "deletion guard did not produce proposal")
	decision := r.Snapshot().Decisions[0]
	if decision.Kind != syncd.DecisionBulkDeletions || decision.RemoteDeletes < 12 {
		panic("deletion proposal counts incorrect")
	}
	fixtureDAV("GET", home+target.RemotePath[1:]+"/delete-00.txt", nil, 200)
	retryCommand(ctx, func() error { return r.ResolveDecision(decision.ID, decision.Digest, syncd.ActionApproveDeletion) })
	await(ctx, func() bool {
		f, _ := r.Snapshot().Folder(target.ID)
		return f.State == syncd.FolderUpToDate && len(r.Snapshot().Decisions) == 0
	}, "approved deletion did not complete")
	fixtureDAV("GET", home+target.RemotePath[1:]+"/delete-00.txt", nil, 404)
	if r.ResolveDecision(decision.ID, decision.Digest, syncd.ActionApproveDeletion) == nil {
		panic("consumed deletion decision accepted twice")
	}
	fmt.Println("PASS real daemon two-folder upload/download, independent pause, pre-propagation deletion block and one-use approval")
}

// exerciseEndpoints checks explicit consent, account-wide switching and pause-gated reconciliation (INV-03/06/08/13).
func exerciseEndpoints(ctx context.Context, r *syncd.Remote, account, alternate, profile string) {
	journals := fixtureJournals(profile)
	var staged syncd.Staging
	retryCommand(ctx, func() error {
		var err error
		staged, err = r.StageEndpoint(account, "Owned alternate fixture", alternate)
		return err
	})
	if !staged.SecureTLS || staged.ID == "" {
		panic("alternate public probe did not validate TLS")
	}
	retryCommand(ctx, func() error {
		err := r.VerifyEndpoint(staged.ID)
		if errors.Is(err, syncd.ErrStale) {
			return err
		}
		if !errors.Is(err, syncd.ErrRefused) {
			panic("unapproved alternate was not refused")
		}
		return nil
	})
	var id string
	retryCommand(ctx, func() error { var err error; id, err = r.ApproveEndpoint(staged.ID); return err })
	await(ctx, func() bool {
		for _, endpoint := range r.Snapshot().Endpoints {
			if endpoint.ID == id {
				return endpoint.Approved && endpoint.Verified == syncd.VerifyVerified && !endpoint.InUse && !endpoint.Blocked
			}
		}
		return false
	}, "approved alternate namespace did not verify")
	primary := ""
	for _, endpoint := range r.Snapshot().Endpoints {
		if endpoint.AccountID == account && endpoint.Main {
			primary = endpoint.ID
		}
	}
	if primary == "" {
		panic("canonical endpoint missing")
	}
	folders := r.Snapshot().Folders
	retryCommand(ctx, func() error { return r.PauseFolder(folders[0].ID) })
	retryCommand(ctx, r.PauseAll)
	await(ctx, func() bool {
		for _, f := range r.Snapshot().Folders {
			if f.State == syncd.FolderSyncing {
				return false
			}
		}
		return true
	}, "pre-switch pause did not drain")
	var operation string
	retryCommand(ctx, func() error { var err error; operation, err = r.RequestSwitch(account, id); return err })
	await(ctx, func() bool {
		for _, endpoint := range r.Snapshot().Endpoints {
			if endpoint.ID == id {
				return endpoint.InUse
			}
		}
		return false
	}, "manual switch did not publish verified alternate")
	for _, f := range r.Snapshot().Folders {
		if f.State != syncd.FolderPaused || (f.ID == folders[0].ID && !f.UserPaused) {
			panic("switch erased a user pause")
		}
	}
	// Stop between binding publication and reconciliation; restart must allocate a fresh operation/epoch.
	retryCommand(ctx, r.Shutdown)
	await(ctx, func() bool { return !r.Snapshot().Daemon.Connected }, "switch restart did not stop owned daemon")
	command := exec.Command(os.Getenv("SYNCD_TEST_EXE"), "--profile", profile)
	if command.Start() != nil {
		panic("owned switch restart failed")
	}
	defer command.Process.Release()
	await(ctx, func() bool {
		if !r.Snapshot().Daemon.Connected {
			return false
		}
		for _, endpoint := range r.Snapshot().Endpoints {
			if endpoint.ID == id {
				return endpoint.InUse
			}
		}
		return false
	}, "interrupted switch did not freshly verify committed alternate after restart")
	if err := waitOperation(ctx, r, operation); !errors.Is(err, syncd.ErrRefused) {
		panic("interrupted receipt was replayed or reported successful")
	}
	retryCommand(ctx, func() error {
		err := r.RemoveEndpoint(id)
		if errors.Is(err, syncd.ErrStale) {
			return err
		}
		if !errors.Is(err, syncd.ErrBusy) {
			panic("active switch target removed")
		}
		return nil
	})
	retryCommand(ctx, r.ResumeAll)
	await(ctx, func() bool {
		for _, f := range r.Snapshot().Folders {
			if f.ID != folders[0].ID {
				return f.State == syncd.FolderUpToDate
			}
		}
		return false
	}, "unpaused folder did not reconcile on alternate")
	f, _ := r.Snapshot().Folder(folders[0].ID)
	if !f.UserPaused {
		panic("account reconciliation resumed a paused folder")
	}
	for _, n := range r.Snapshot().Networks {
		if n.AccountID == account && (!n.Switching || n.ActiveID != id) {
			panic("paused recovery member incorrectly completed switch")
		}
	}
	retryCommand(ctx, func() error { return r.ResumeFolder(folders[0].ID) })
	await(ctx, func() bool {
		for _, n := range r.Snapshot().Networks {
			if n.AccountID == account {
				return !n.Switching && n.ActiveID == id
			}
		}
		return false
	}, "restart recovery did not finish paused-member reconciliation")
	// Transfer changed bytes through the real daemon while the alternate is committed.
	home := fixtureHome()
	for _, f := range folders {
		if os.WriteFile(filepath.Join(f.LocalPath, "alternate.txt"), []byte("alternate-owned"), 0600) != nil {
			panic("alternate fixture write failed")
		}
	}
	await(ctx, func() bool {
		for _, f := range folders {
			if string(fixtureDAV("GET", home+f.RemotePath[1:]+"/alternate.txt", nil, 200, 404)) != "alternate-owned" {
				return false
			}
		}
		return true
	}, "alternate daemon upload failed")
	retryCommand(ctx, r.PauseAll)
	await(ctx, func() bool {
		for _, f := range r.Snapshot().Folders {
			if f.State == syncd.FolderSyncing {
				return false
			}
		}
		return true
	}, "return-switch pause did not drain")
	retryCommand(ctx, func() error { var err error; operation, err = r.RequestSwitch(account, primary); return err })
	await(ctx, func() bool {
		for _, endpoint := range r.Snapshot().Endpoints {
			if endpoint.ID == primary {
				return endpoint.InUse
			}
		}
		return false
	}, "return switch did not publish primary")
	// Kill only the exact fixture child we started; no process-name or user-profile matching.
	if command.Process.Kill() != nil {
		panic("owned daemon crash injection failed")
	}
	_ = command.Wait()
	await(ctx, func() bool { return !r.Snapshot().Daemon.Connected }, "crashed fixture daemon stayed connected")
	recovery := exec.Command(os.Getenv("SYNCD_TEST_EXE"), "--profile", profile)
	if recovery.Start() != nil {
		panic("owned daemon crash recovery launch failed")
	}
	_ = recovery.Process.Release()
	await(ctx, func() bool {
		if !r.Snapshot().Daemon.Connected {
			return false
		}
		for _, endpoint := range r.Snapshot().Endpoints {
			if endpoint.ID == primary {
				return endpoint.InUse
			}
		}
		return false
	}, "crash recovery did not reverify committed primary")
	if err := waitOperation(ctx, r, operation); !errors.Is(err, syncd.ErrRefused) {
		panic("crashed switch receipt replayed or reported success")
	}
	retryCommand(ctx, r.ResumeAll)
	await(ctx, func() bool {
		for _, n := range r.Snapshot().Networks {
			if n.AccountID == account {
				return !n.Switching && n.ActiveID == primary
			}
		}
		return false
	}, "crash recovery did not reconcile committed primary")
	for _, n := range r.Snapshot().Networks {
		if n.AccountID == account && (n.Switching || n.ActiveID != primary) {
			panic("return switch did not reconcile all folders")
		}
	}
	exercisePolicy(ctx, r, account, primary, id, folders[0].ID)
	retryCommand(ctx, func() error { return r.RemoveEndpoint(id) })
	after := fixtureJournals(profile)
	if len(after) != len(journals) {
		panic("switch replaced journal membership")
	}
	for path, before := range journals {
		if current, ok := after[path]; !ok || !os.SameFile(before, current) {
			panic("switch recreated an engine journal")
		}
	}
	fmt.Println("PASS real daemon account switching, preserved pauses, interrupted reconciliation after graceful restart and owned-process kill, fresh-origin recovery, alternate uploads and cleanup")
}

// exercisePolicy verifies fixed policy commits only after every real mapped root reconciles (INV-12/13/14).
func exercisePolicy(ctx context.Context, r *syncd.Remote, account, primary, alternate, paused string) {
	retryCommand(ctx, func() error { return r.SetPolicy(account, syncd.PolicyAuto, "") })
	retryCommand(ctx, func() error { return r.PauseFolder(paused) })
	retryCommand(ctx, r.PauseAll)
	var operation string
	retryCommand(ctx, func() error {
		var err error
		operation, err = r.RequestPolicy(account, syncd.PolicyFixed, alternate)
		return err
	})
	await(ctx, func() bool {
		for _, n := range r.Snapshot().Networks {
			if n.AccountID == account {
				if n.Policy != syncd.PolicyAuto {
					panic("fixed policy committed before paused-root reconciliation")
				}
				return n.ActiveID == alternate && n.RequestedEndpointID == alternate
			}
		}
		return false
	}, "fixed policy did not verify and publish requested alternate separately")
	retryCommand(ctx, r.ResumeAll)
	if f, ok := r.Snapshot().Folder(paused); !ok || !f.UserPaused {
		panic("fixed policy erased an independent pause")
	}
	retryCommand(ctx, func() error { return r.ResumeFolder(paused) })
	if waitOperation(ctx, r, operation) != nil {
		panic("verified fixed policy did not complete")
	}
	await(ctx, func() bool {
		for _, n := range r.Snapshot().Networks {
			if n.AccountID == account {
				return n.Policy == syncd.PolicyFixed && n.FixedEndpointID == alternate && n.ActiveID == alternate && n.RequestedEndpointID == ""
			}
		}
		return false
	}, "fixed policy projection was not committed after reconciliation")
	retryCommand(ctx, func() error {
		var err error
		operation, err = r.RequestPolicy(account, syncd.PolicyFixed, primary)
		return err
	})
	if waitOperation(ctx, r, operation) != nil {
		panic("fixed primary policy did not reconcile")
	}
	fmt.Println("PASS real automatic-policy persistence and fixed-target pending/actual separation, paused-root completion and verified return")
}

// fixtureJournals captures only native file identities to prove the same journals survive switching (INV-08).
func fixtureJournals(profile string) map[string]os.FileInfo {
	hash := sha256.Sum256([]byte(profile))
	dir := filepath.Join(os.Getenv("LOCALAPPDATA"), "Nextcloud-MyGo", "syncd", fmt.Sprintf("%x", hash[:12]), "journals")
	entries, err := os.ReadDir(dir)
	if err != nil {
		panic("owned journal identities unavailable")
	}
	files := map[string]os.FileInfo{}
	for _, entry := range entries {
		if filepath.Ext(entry.Name()) != ".sqlite" {
			continue
		}
		file, err := os.Open(filepath.Join(dir, entry.Name()))
		if err != nil {
			panic("owned journal identity handle unavailable")
		}
		info, err := file.Stat()
		_ = file.Close()
		if err != nil {
			panic("owned journal identity lookup failed")
		}
		files[entry.Name()] = info
	}
	if len(files) != 2 {
		panic("owned two-folder journals missing")
	}
	return files
}

// cleanupSync stops owned mappings before deleting only this fixture's remote and local resources.
func cleanupSync(ctx context.Context, r *syncd.Remote, s state, file string) {
	retryCommand(ctx, r.PauseAll)
	await(ctx, func() bool {
		for _, f := range r.Snapshot().Folders {
			if f.State == syncd.FolderSyncing {
				return false
			}
		}
		return true
	}, "owned sync cleanup did not quiesce")
	for _, f := range r.Snapshot().Folders {
		retryCommand(ctx, func() error { return r.RemoveFolder(f.ID) })
	}
	home := fixtureHome()
	for _, suffix := range []string{"a", "b"} {
		fixtureDAV("DELETE", home+"nextcloud-mygo-"+s.Profile+"-"+suffix, nil, 204, 404)
	}
	if os.RemoveAll(filepath.Join(filepath.Dir(file), s.Profile)) != nil {
		panic("owned local sync cleanup failed")
	}
}

// launchDaemon starts the owned daemon on the fixture profile and reports, on
// stderr, its own fatal messages and its exit code as sanitized
// "SYNCD_DAEMON" lines, so a crash after a restart names itself.
// Notes: only letters, digits and plain punctuation pass; nothing else of the
// daemon's output leaves the fixture.
func launchDaemon(profile string) (*exec.Cmd, <-chan struct{}) {
	exited := make(chan struct{})
	command := exec.Command(os.Getenv("SYNCD_TEST_EXE"), "--profile", profile)
	output, err := command.StderrPipe()
	if err != nil || command.Start() != nil {
		panic("owned daemon launch failed")
	}
	go func() {
		scanner := bufio.NewScanner(output)
		for scanner.Scan() {
			line := []rune(scanner.Text())
			clean := make([]rune, 0, len(line))
			for _, r := range line {
				if r < 128 && (unicode.IsLetter(r) || unicode.IsDigit(r) || strings.ContainsRune(" _.,:()-", r)) {
					clean = append(clean, r)
				}
			}
			if len(clean) > 160 {
				clean = clean[:160]
			}
			fmt.Fprintln(os.Stderr, "SYNCD_DAEMON "+string(clean))
		}
		err := command.Wait()
		code := 0
		if exit, ok := errors.AsType[*exec.ExitError](err); ok {
			code = exit.ExitCode()
		}
		fmt.Fprintf(os.Stderr, "SYNCD_DAEMON exit %d\n", code)
		close(exited)
	}()
	return command, exited
}

// reasonWord names a client error by its stable reason code or sentinel,
// never by server text.
func reasonWord(err error) string {
	if err == nil {
		return "no error"
	}
	if code := syncd.ReasonOf(err); code != "" {
		return code
	}
	for _, known := range []error{syncd.ErrCredentialCleanup, syncd.ErrBusy, syncd.ErrStale, syncd.ErrRefused, syncd.ErrNotFound, context.DeadlineExceeded, context.Canceled} {
		if errors.Is(err, known) {
			return strings.TrimPrefix(known.Error(), "syncd: ")
		}
	}
	return "other"
}

// ownDaemon starts the fixture's daemon itself once the previous one has
// released the profile: a start that exits at once found it still held.
func ownDaemon(profile string) {
	for attempt := 0; attempt < 15; attempt++ {
		_, exited := launchDaemon(profile)
		select {
		case <-exited:
			time.Sleep(time.Second)
		case <-time.After(3 * time.Second):
			return
		}
	}
	panic("owned daemon could not take its profile")
}
