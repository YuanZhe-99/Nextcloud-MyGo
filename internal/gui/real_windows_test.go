package gui

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// realApp is the interface over the compiled SyncD on a unique profile:
// results from SyncD's goroutines queue up and run on the test's
// goroutine, as they do on the main thread in the app.
type realApp struct {
	*App
	t       *testing.T
	remote  *syncd.Remote
	queue   chan func()
	opened  []string
	profile string
	exe     string
}

// startReal starts the daemon and the interface over it.
// Side effects: one uniquely named local profile, removed afterwards; the
// daemon is stopped explicitly at the end (daemon.shutdown).
func startReal(t *testing.T, ctx context.Context) *realApp {
	t.Helper()
	exe := os.Getenv("SYNCD_TEST_EXE")
	if exe == "" {
		t.Skip("set SYNCD_TEST_EXE to the compiled native daemon")
	}
	exe, err := filepath.Abs(exe)
	if err != nil {
		t.Fatal(err)
	}
	b := make([]byte, 8)
	_, _ = rand.Read(b)
	profile := fmt.Sprintf("gui-test-%x", b)
	hash := sha256.Sum256([]byte(profile))
	dir := filepath.Join(os.Getenv("LOCALAPPDATA"), "Nextcloud-MyGo", "syncd", fmt.Sprintf("%x", hash[:12]))
	t.Cleanup(func() {
		// Retried: a scanner may still hold a just-closed file (seen on hosted runners).
		var err error
		for deadline := time.Now().Add(10 * time.Second); ; time.Sleep(200 * time.Millisecond) {
			if err = os.RemoveAll(dir); err == nil || time.Now().After(deadline) {
				break
			}
		}
		if err != nil {
			t.Error(err)
		}
	})
	cmd := exec.CommandContext(ctx, exe, "--profile", profile)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = cmd.Process.Kill(); _ = cmd.Wait() })
	r := &realApp{t: t, queue: make(chan func(), 256), profile: profile, exe: exe}
	// The test starts SyncD itself; NewRemote could race it with a second daemon.
	r.remote = syncd.ConnectRemote(ctx, profile, exe)
	t.Cleanup(r.remote.Close)
	r.App = New(r.remote, t.TempDir())
	r.later = func(fn func()) { r.queue <- fn }
	r.openURL = func(url string) { r.queue <- func() { r.opened = append(r.opened, url) } }
	r.Start()
	t.Cleanup(r.Stop)
	return r
}

// await runs queued results until cond holds, or fails the test.
func (r *realApp) await(what string, cond func() bool) {
	r.t.Helper()
	deadline := time.After(30 * time.Second)
	for !cond() {
		select {
		case fn := <-r.queue:
			fn()
		case <-time.After(20 * time.Millisecond):
		case <-deadline:
			r.t.Fatalf("timed out waiting for %s", what)
		}
	}
}

// TestRealDaemonInterface drives the interface against the compiled SyncD
// over the real Named Pipe: a fresh profile is a known empty one (setup,
// not unknown), Pause every folder persists through SyncD, Diagnostics
// reads the daemon and exports its bundle, closing the interface leaves
// SyncD running (INV-11, INV-13, PLAN.md §17 item 10), and with
// NEXTCLOUD_TEST_URL set a browser sign-in starts and is cancelled
// without any credential reaching the interface.
func TestRealDaemonInterface(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	r := startReal(t, ctx)
	r.await("a connected snapshot", func() bool { return r.snap.Daemon.Connected })
	s := r.snap
	if s.Daemon.Demo || s.Daemon.ProtocolMinor < 7 || !s.Daemon.Capabilities["fullSync"] || !s.Daemon.Capabilities["endpointAutoSwitch"] {
		t.Fatalf("unexpected daemon: demo %v, protocol 1.%d, capabilities %v", s.Daemon.Demo, s.Daemon.ProtocolMinor, s.Daemon.Capabilities)
	}
	tt := ui.NewTester(r.view, 1200, 780)
	tt.Frame()
	if !stateKnown(s) || !r.setupShown() || !tt.HasText(L.SetupWelcomeTitle()) {
		t.Fatal("a fresh profile is not shown as a first run")
	}
	if tt.HasText(L.LoginUnavailable()) || tt.HasText(L.DaemonDisconnected()) {
		t.Fatal("sign-in shown as unavailable with a live login capability")
	}
	shoot(t, tt, "realpipe-setup")
	if o := overallOf(s); o == overallUnknown {
		t.Fatal("a connected daemon with readable state reads as unknown")
	}

	// Pause every folder from Settings, through settings.update.
	r.openSettings(spSync)
	tt.Frame()
	clickRole(t, tt, ui.RoleSwitch, L.PauseEverything())
	tt.Frame()
	r.await("SyncD to report everything paused", func() bool { return (r.snap.Daemon.AllPaused || r.toast != "") && !r.busy["all"] })
	if !r.snap.Daemon.AllPaused {
		t.Fatalf("pause failed: %s", r.toast)
	}
	tt.Frame()
	shoot(t, tt, "realpipe-settings-sync")

	// Diagnostics: SyncD's own reading, then a support bundle it makes.
	r.openSettings(spDiagnostics)
	r.await("diagnostics", func() bool { return r.diag.reading != nil || r.diag.failed != "" })
	if r.diag.failed != "" || r.diag.reading.StartupState != syncd.StartupNormal || !r.diag.reading.AllPaused {
		t.Fatalf("diagnostics: %q %+v", r.diag.failed, r.diag.reading)
	}
	tt.Frame()
	if err := tt.Click(L.ActionExport()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	r.await("the support bundle", func() bool { return r.diag.bundle != "" || (!r.busy["export"] && r.toast != "") })
	if r.diag.bundle == "" {
		t.Fatalf("export failed: %s", r.toast)
	}
	if _, err := os.Stat(r.diag.bundle); err != nil {
		t.Fatal("the bundle SyncD reported does not exist")
	}
	tt.Frame()
	if !tt.HasText(L.BundleSaved()) {
		t.Fatal("the bundle's path is not offered")
	}
	shoot(t, tt, "realpipe-diagnostics")
	r.settings.open = false

	if server := os.Getenv("NEXTCLOUD_TEST_URL"); server != "" {
		// Login Flow v2 starts in SyncD; the interface only opens the link.
		r.setup.server = server
		r.beginLogin()
		r.await("the browser step", func() bool { return (r.setup.step == stepSignIn && len(r.opened) > 0) || r.setup.failed != "" })
		if r.setup.failed != "" || len(r.opened) != 1 || !strings.HasPrefix(r.opened[0], "https://") {
			t.Fatalf("sign-in did not reach the browser step: %q, %d links opened", r.setup.failed, len(r.opened))
		}
		tt.Frame()
		shoot(t, tt, "realpipe-signin")
		id := r.setup.login.ID
		if err := tt.Click(L.ActionBack()); err != nil {
			t.Fatal(err)
		}
		tt.Frame()
		r.await("the cancellation", func() bool { return !r.busy["login-cancel"] && !r.busy["login"] })
		if _, err := r.remote.WaitLogin(ctx, id); err == nil {
			t.Fatal("a cancelled sign-in produced an account")
		}
		if r.setup.step != stepServer {
			t.Fatal("Back did not return to the server step")
		}
	}

	// Closing the interface leaves SyncD and its state as they were.
	r.Stop()
	r.remote.Close()
	again := syncd.NewRemote(ctx, r.profile, r.exe)
	t.Cleanup(again.Close)
	deadline := time.Now().Add(15 * time.Second)
	for s := again.Snapshot(); !s.Daemon.Connected || !s.Daemon.AllPaused; s = again.Snapshot() {
		if time.Now().After(deadline) {
			t.Fatal("SyncD did not keep running with its pause after the interface closed")
		}
		time.Sleep(20 * time.Millisecond)
	}
	if err := again.ResumeAll(); err != nil {
		t.Fatal(err)
	}
	if err := again.Shutdown(); err != nil {
		t.Fatal(err)
	}
}
