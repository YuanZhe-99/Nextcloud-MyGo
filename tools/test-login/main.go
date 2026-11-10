// test-login is the real IPC half of the isolated Login Flow v2 acceptance fixture.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
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

// connect waits for the paired daemon's negotiated snapshot within the fixture deadline.
func connect(ctx context.Context, s state, executable string) *syncd.Remote {
	r := syncd.NewRemote(ctx, s.Profile, executable)
	for !r.Snapshot().Daemon.Connected {
		select {
		case <-ctx.Done():
			panic("daemon connection deadline")
		case <-time.After(20 * time.Millisecond):
		}
	}
	return r
}

// main starts, completes or cleans one owned consent fixture without touching user sync roots.
func main() {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Minute)
	defer cancel()
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
	r := connect(ctx, s, executable)
	if os.Args[1] == "abort" {
		if s.Renewal != "" {
			_ = r.CancelLogin(s.Renewal)
		}
		if s.Folders {
			cleanupSync(ctx, r, s, file)
		}
		for _, account := range r.Snapshot().Accounts {
			if err := r.RemoveAccount(account.ID); err != nil {
				panic("owned account cleanup needs attention")
			}
		}
		_ = r.CancelLogin(s.Session)
		if err := r.Shutdown(); err != nil {
			panic("owned daemon shutdown failed")
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
		fmt.Println("Cancelled owned incomplete login and cleaned local profile")
		return
	}
	id, err := r.WaitLogin(ctx, s.Session)
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
	time.Sleep(300 * time.Millisecond)
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
			panic("account removal deadline")
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
