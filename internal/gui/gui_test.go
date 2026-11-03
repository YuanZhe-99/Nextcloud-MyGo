package gui

import (
	"image/png"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/egoist/mygo/i18n"
	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// testApp is the interface over a Mock, with work run inline so a test
// sees each command's result at once.
func testApp(t *testing.T, sample bool) (*App, *syncd.Mock) {
	t.Helper()
	m := syncd.NewMock(sample, nil)
	a := New(m, "")
	a.later = func(fn func()) { fn() }
	a.async = func(fn func()) { fn() }
	a.Start()
	return a, m
}

// shoot writes the window to SCREENSHOT_DIR when it is set, to compare
// with design/.
func shoot(t *testing.T, tt *ui.Tester, name string) {
	t.Helper()
	dir := os.Getenv("SCREENSHOT_DIR")
	if dir == "" {
		return
	}
	f, err := os.Create(filepath.Join(dir, name+".png"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if err := png.Encode(f, tt.Image()); err != nil {
		t.Fatal(err)
	}
}

// TestPagesRender builds every page and Settings page in both appearances
// and both languages: each shows its title and none panics.
func TestPagesRender(t *testing.T) {
	pages := []struct {
		name string
		pg   page
		set  settingsPage
		open bool
		want func(L.Messages) string
	}{
		{"overview", pageOverview, 0, false, L.Messages.NeedsAttention},
		{"folders", pageFolders, 0, false, L.Messages.ColPaths},
		{"network", pageNetwork, 0, false, L.Messages.AddressesOf},
		{"activity", pageActivity, 0, false, L.Messages.ReviewWouldDelete},
		{"settings-general", pageOverview, spGeneral, true, L.Messages.SectionStartClose},
		{"settings-notifications", pageOverview, spNotifications, true, L.Messages.SectionNotify},
		{"settings-sync", pageOverview, spSync, true, L.Messages.DeletionProtection},
		{"settings-network", pageOverview, spNetwork, true, L.Messages.NetworkSettingsRow},
		{"settings-about", pageOverview, spAbout, true, L.Messages.SectionUpdates},
		{"settings-diagnostics", pageOverview, spDiagnostics, true, L.Messages.SupportBundle},
	}
	for _, lang := range []string{"en", "zh-Hans"} {
		for _, dark := range []bool{false, true} {
			for _, pc := range pages {
				name := pc.name + "-" + lang
				if dark {
					name += "-dark"
				}
				t.Run(name, func(t *testing.T) {
					a, _ := testApp(t, true)
					a.page = pc.pg
					if pc.open {
						a.openSettings(pc.set)
					}
					tt := ui.NewTester(a.view, 1200, 780)
					i18n.SetLocaleFunc(func() string { return lang })
					defer i18n.SetLocaleFunc(nil)
					tt.SetLocale(lang)
					tt.SetDark(dark)
					tt.Frame()
					want := pc.want(L.In(lang))
					if !tt.HasText(want) {
						t.Fatalf("%s does not show %q", pc.name, want)
					}
					shoot(t, tt, name)
				})
			}
		}
	}
}

// TestFirstRunShowsSetup: without an account the window is setup, whatever
// page was chosen.
func TestFirstRunShowsSetup(t *testing.T) {
	a, _ := testApp(t, false)
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	if !tt.HasText(L.SetupWelcomeTitle()) {
		t.Fatal("no setup on a first run")
	}
	shoot(t, tt, "setup-welcome")
	a.setup.step = stepFolders
	a.setup.remote, _ = a.client.RemoteFolders("")
	a.setup.chosen["/Documents"] = true
	tt.Frame()
	if !tt.HasText(L.EncryptedUnsupported()) {
		t.Fatal("an encrypted folder is not marked as unsupported")
	}
	shoot(t, tt, "setup-folders")
	a.setup.step = stepStart
	tt.Frame()
	shoot(t, tt, "setup-start")
}

// TestRestoreResolvesTheReview: the safe answer resolves the proposal
// shown, and the review leaves the page.
func TestRestoreResolvesTheReview(t *testing.T) {
	a, m := testApp(t, true)
	a.page = pageActivity
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	if err := tt.Click(L.ActionRestorePC()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	for _, d := range m.Snapshot().Decisions {
		if d.ID == "d-photos" {
			t.Fatal("the deletion review is still pending")
		}
	}
	if tt.HasText(L.ReviewWouldDelete()) {
		t.Fatal("the review still shows")
	}
}

// TestDeleteOnServerAsksFirst: deleting on the server needs a second,
// explicit answer; cancelling deletes nothing (PLAN.md §11.3).
func TestDeleteOnServerAsksFirst(t *testing.T) {
	a, m := testApp(t, true)
	a.page = pageActivity
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	if err := tt.Click(L.ActionDeleteServer(142)); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if !a.confirm.open {
		t.Fatal("no question before deleting on the server")
	}
	if len(m.Snapshot().Decisions) != 2 {
		t.Fatal("deleted before the answer")
	}
	shoot(t, tt, "activity-confirm-delete")
	if err := tt.Click(L.ActionCancel()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if a.confirm.open || len(m.Snapshot().Decisions) != 2 {
		t.Fatal("cancel did not leave the proposal as it was")
	}
}

// TestPauseAllKeepsFolderPause: Pause sync and Resume sync act on every
// folder but leave one paused on its own paused.
func TestPauseAllKeepsFolderPause(t *testing.T) {
	a, m := testApp(t, true)
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	for _, label := range []string{L.ActionPauseSync(), L.ActionResumeSync()} {
		if err := tt.Click(label); err != nil {
			t.Fatal(err)
		}
		tt.Frame()
	}
	if f, _ := m.Snapshot().Folder("f-reports"); !f.UserPaused {
		t.Fatal("resuming everything undid a folder's own pause")
	}
}

func TestOverall(t *testing.T) {
	now := time.Now()
	cases := []struct {
		name string
		s    syncd.Snapshot
		want overall
	}{
		{"nothing yet", syncd.Snapshot{}, overallUpToDate},
		{"paused wins", syncd.Snapshot{Daemon: syncd.Daemon{AllPaused: true}, Folders: []syncd.Folder{{State: syncd.FolderSyncing}}}, overallPaused},
		{"review before syncing", syncd.Snapshot{Folders: []syncd.Folder{{State: syncd.FolderSyncing}, {State: syncd.FolderNeedsReview}}}, overallNeedsYou},
		{"syncing", syncd.Snapshot{Folders: []syncd.Folder{{State: syncd.FolderSyncing}, {State: syncd.FolderUpToDate}}}, overallSyncing},
		{"all offline", syncd.Snapshot{Folders: []syncd.Folder{{State: syncd.FolderOffline}}}, overallOffline},
		{"sample", syncd.NewMock(true, func() time.Time { return now }).Snapshot(), overallNeedsYou},
	}
	for _, c := range cases {
		if got := overallOf(c.s); got != c.want {
			t.Errorf("%s: %v, want %v", c.name, got, c.want)
		}
	}
}

func TestAutoDue(t *testing.T) {
	three := time.Date(2026, 11, 4, 3, 20, 0, 0, time.Local)
	noon := time.Date(2026, 11, 4, 12, 0, 0, 0, time.Local)
	cases := []struct {
		mode string
		idle time.Duration
		now  time.Time
		want bool
	}{
		{"never", time.Hour, three, false},
		{"idle", 14 * time.Minute, noon, false},
		{"idle", 15 * time.Minute, noon, true},
		{"daily", 0, three, true},
		{"daily", time.Hour, noon, false},
	}
	for _, c := range cases {
		if got := autoDue(c.mode, c.idle, c.now); got != c.want {
			t.Errorf("autoDue(%s, %v, %v) = %v", c.mode, c.idle, c.now.Hour(), got)
		}
	}
}

func TestSize(t *testing.T) {
	cases := map[int64]string{999: "999 B", 8_000_000: "8.0 MB", 120_000_000: "120 MB", 1_800_000_000: "1.8 GB"}
	for b, want := range cases {
		if got := size(b); got != want {
			t.Errorf("size(%d) = %q, want %q", b, got, want)
		}
	}
}
