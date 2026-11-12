// test-login is the real IPC half of the isolated Login Flow v2 acceptance fixture.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
)

// state persists only owned fixture identities, never a URL, token or password.
type state struct {
	Profile, Session string
	Renewal          string
	Folders          bool
}

// connect waits for the paired daemon's negotiated normal snapshot within one step budget.
// Notes: a daemon that answers but stays in backoff or diagnostic state fails here with that state.
func connect(ctx context.Context, s state, executable string) *syncd.Remote {
	r := syncd.NewRemote(ctx, s.Profile, executable)
	observed = r
	await(ctx, func() bool { return r.Snapshot().Daemon.Connected }, "daemon connection")
	return r
}

// main starts, completes or cleans one owned consent fixture without touching user sync roots.
func main() {
	// The whole flow takes a few minutes; each step also has its own shorter budget.
	budget := 12 * time.Minute
	if len(os.Args) == 2 && os.Args[1] == "abort" {
		budget = 2 * time.Minute
	}
	ctx, cancel := context.WithTimeout(context.Background(), budget)
	defer cancel()
	// A step that ignores its context still cannot hang the fixture: past the phase budget the
	// watchdog prints where every goroutine stands (code positions only) and exits.
	time.AfterFunc(budget+15*time.Second, func() {
		stacks := make([]byte, 1<<20)
		stacks = stacks[:runtime.Stack(stacks, true)]
		fmt.Fprintf(os.Stderr, "panic: watchdog: phase exceeded its budget\n%s", stacks)
		go func() { fmt.Fprintln(os.Stderr, "watchdog last state:", daemonState()) }()
		time.Sleep(time.Second)
		os.Exit(3)
	})
	executable, err := filepath.Abs(os.Getenv("SYNCD_TEST_EXE"))
	if err != nil {
		panic(err)
	}
	file := os.Getenv("SYNCD_LOGIN_FIXTURE_STATE")
	if file == "" || os.Getenv("SYNCD_TEST_EXE") == "" {
		panic("fixture environment missing")
	}
	if len(os.Args) != 2 {
		panic("start or finish required")
	}
	if os.Args[1] != "start" && os.Args[1] != "finish" && os.Args[1] != "abort" {
		panic("unknown fixture phase")
	}
	if os.Getenv("LOCALAPPDATA") == "" {
		panic("Windows profile location missing")
	}
	// The start phase's daemon must outlive it until finish; the wrapper's job owns that one.
	if os.Args[1] != "start" {
		ownChildren()
	}
	var s state
	if os.Args[1] == "start" {
		s.Profile = fmt.Sprintf("login-probe-%d", time.Now().UnixNano())
		s.Folders = os.Getenv("SYNCD_TEST_FOLDERS") == "1"
		initial, _ := json.Marshal(s)
		if err := os.WriteFile(file, initial, 0600); err != nil {
			panic(err)
		}
		r := connect(ctx, s, executable)
		defer r.Close()
		login, err := r.StartLogin(os.Getenv("NEXTCLOUD_TEST_URL"))
		if err != nil {
			panic(err)
		}
		s.Session = login.ID
		b, _ := json.Marshal(s)
		if err := os.WriteFile(file, b, 0600); err != nil {
			panic(err)
		}
		// This is an explicit one-time browser handoff, never an application diagnostic log or saved file.
		fmt.Println(login.LoginURL)
		return
	}
	b, err := os.ReadFile(file)
	if err != nil {
		panic(err)
	}
	if err := json.Unmarshal(b, &s); err != nil {
		panic(err)
	}
	stamp, err := strconv.ParseInt(strings.TrimPrefix(s.Profile, "login-probe-"), 10, 64)
	if !strings.HasPrefix(s.Profile, "login-probe-") || err != nil || stamp <= 0 {
		panic("invalid owned fixture profile")
	}
	if os.Args[1] == "abort" {
		abort(ctx, s, executable, file)
		return
	}
	r := connect(ctx, s, executable)
	id, err := waitLogin(ctx, r, s.Session)
	if err != nil {
		panic(err)
	}
	if id == "" {
		panic("missing ready account")
	}
	if err := r.Shutdown(); err != nil {
		panic(err)
	}
	r.Close()
	time.Sleep(1500 * time.Millisecond)
	// Started here, not by the client, so its own fatal messages are reported.
	ownDaemon(s.Profile)
	r = connect(ctx, s, executable)
	defer r.Close()
	defer func() {
		if failure := recover(); failure != nil {
			deadline, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			if diagnostic, err := r.Diagnostics(deadline); err == nil {
				if encoded, err := json.Marshal(diagnostic); err == nil {
					fmt.Fprintln(os.Stderr, "SYNCD_DIAGNOSTICS "+string(encoded))
				}
			} else {
				fmt.Fprintf(os.Stderr, "SYNCD_DIAGNOSTICS_ERROR %v\n", err)
			}
			cancel()
			panic(failure)
		}
	}()
	account, ok := r.Snapshot().Account(id)
	if !ok || !account.SignedIn {
		panic("account did not survive daemon restart")
	}
	if s.Folders {
		exerciseSync(ctx, r, id, s, file)
		await(ctx, func() bool {
			uploaded, downloaded, switched := false, false, false
			for _, e := range r.Snapshot().Activity {
				if e.Kind == syncd.ActUploaded && e.Count > 0 {
					uploaded = true
				}
				if e.Kind == syncd.ActDownloaded && e.Count > 0 {
					downloaded = true
				}
				if e.Kind == syncd.ActSwitched {
					switched = true
				}
			}
			return uploaded && downloaded && switched
		}, "confirmed upstream transfer/switch activity was not published")
		exerciseActiveCrash(ctx, r, s)
		exerciseActiveSwitch(ctx, r, s, id)
		if os.Getenv("SYNCD_TEST_REAUTH") == "1" {
			exerciseReauthentication(ctx, r, id, s, file)
		}
		cleanupSync(ctx, r, s, file)
	}
	if err := r.RemoveAccount(id); err != nil {
		panic(err)
	}
	for len(r.Snapshot().Accounts) != 0 {
		select {
		case <-ctx.Done():
			fail("account removal (phase deadline)")
		case <-time.After(20 * time.Millisecond):
		}
	}
	if err := r.Shutdown(); err != nil {
		panic(err)
	}
	r.Close()
	hash := sha256.Sum256([]byte(s.Profile))
	dir := filepath.Join(os.Getenv("LOCALAPPDATA"), "Nextcloud-MyGo", "syncd", fmt.Sprintf("%x", hash[:12]))
	for i := 0; i < 100; i++ {
		err = os.RemoveAll(dir)
		if err == nil {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if err != nil {
		panic("owned profile cleanup failed")
	}
	if err := os.Remove(file); err != nil {
		panic(err)
	}
	fmt.Println("PASS real Login Flow v2 consent, native credential storage, account persistence/restart, confirmed remote app-password revocation, local account/vault/profile cleanup")
}

// abort removes only this fixture's owned objects, directly and within the abort budget; it never
// replays the flow that failed. Remote collections and local roots go even when the daemon cannot be
// reached; the account (and with it the issued app password) is removed only through the daemon, so a
// profile whose account could not be removed is kept and reported rather than deleted.
func abort(ctx context.Context, s state, executable, file string) {
	removed := func() (ok bool) {
		defer func() {
			if failure := recover(); failure != nil {
				fmt.Fprintln(os.Stderr, failure)
				ok = false
			}
		}()
		// A daemon restarted after the fixture's own kills waits out its crash backoff (at most 60 s).
		// Started here so its own fatal messages are reported; a daemon that is
		// already running keeps the profile and this one exits.
		launchDaemon(s.Profile) // exits at once if a daemon still holds the profile
		connectCtx, cancel := context.WithTimeout(ctx, 75*time.Second)
		defer cancel()
		r := connect(connectCtx, s, executable)
		defer r.Close()
		if s.Renewal != "" {
			_ = r.CancelLogin(s.Renewal)
		}
		_ = r.CancelLogin(s.Session)
		// Stop the daemon before local roots are removed, whatever happens below.
		defer func() {
			if r.Shutdown() == nil {
				await(ctx, func() bool { return !r.Snapshot().Daemon.Connected }, "owned daemon stop")
			}
		}()
		// Folder removal is best effort: an interrupted switch can refuse it, and account removal
		// below removes the account's folders anyway.
		_ = r.PauseAll()
		for _, f := range r.Snapshot().Folders {
			if err := r.RemoveFolder(f.ID); err != nil {
				fmt.Fprintln(os.Stderr, "owned folder removal deferred to account removal:", err)
			}
		}
		for _, account := range r.Snapshot().Accounts {
			waited := false
			for {
				err := r.RemoveAccount(account.ID)
				if err == nil {
					break
				}
				if errors.Is(err, syncd.ErrStale) && ctx.Err() == nil {
					time.Sleep(100 * time.Millisecond)
					continue
				}
				if !errors.Is(err, syncd.ErrBusy) || waited {
					fmt.Fprintln(os.Stderr, "owned account removal refused:", err)
					return false
				}
				// An interrupted switch must finish before its account can go: let the daemon
				// reconcile once (owned folders only), then remove the account.
				fmt.Fprintln(os.Stderr, "owned account removal waits for the interrupted switch:", err)
				waited = true
				retryCommand(ctx, r.ResumeAll)
				await(ctx, func() bool {
					for _, n := range r.Snapshot().Networks {
						if n.Switching {
							return false
						}
					}
					return true
				}, "interrupted switch completion before account removal")
			}
		}
		await(ctx, func() bool { return len(r.Snapshot().Accounts) == 0 }, "owned account removal")
		return true
	}()
	if !removed {
		// Keep the roots: an interrupted switch can only finish (and the account go) while they exist.
		panic("owned account could not be removed through the daemon; profile, roots and state kept")
	}
	if s.Folders {
		home := fixtureHome()
		for _, suffix := range []string{"a", "b"} {
			fixtureDAV("DELETE", home+"nextcloud-mygo-"+s.Profile+"-"+suffix, nil, 204, 404)
		}
		if os.RemoveAll(filepath.Join(filepath.Dir(file), s.Profile)) != nil {
			panic("owned local sync cleanup failed")
		}
	}
	hash := sha256.Sum256([]byte(s.Profile))
	dir := filepath.Join(os.Getenv("LOCALAPPDATA"), "Nextcloud-MyGo", "syncd", fmt.Sprintf("%x", hash[:12]))
	var err error
	for i := 0; i < 100; i++ {
		if err = os.RemoveAll(dir); err == nil {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if err != nil {
		panic("owned profile cleanup failed")
	}
	if err := os.Remove(file); err != nil {
		panic(err)
	}
	fmt.Println("Cancelled owned incomplete login and cleaned local profile")
}
