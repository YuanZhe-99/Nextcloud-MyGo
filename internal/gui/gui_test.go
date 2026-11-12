package gui

import (
	"encoding/json"
	"image/png"
	"os"
	"path/filepath"
	"strings"
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
	a.openURL = func(string) {}
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
		{"activity", pageActivity, 0, false, L.Messages.ReviewKeep},
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

// TestDecideLaterKeepsTheReview: the safe answer keeps the proposal and
// deletes nothing (PLAN.md §11.3).
func TestDecideLaterKeepsTheReview(t *testing.T) {
	a, m := testApp(t, true)
	a.page = pageActivity
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	shoot(t, tt, "activity-review")
	if err := tt.Click(L.ActionDecideLater()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if _, ok := pendingReview(m.Snapshot(), "f-photos"); !ok {
		t.Fatal("Decide later answered the proposal")
	}
	if a.page != pageOverview {
		t.Fatal("Decide later stayed on the review")
	}
}

// TestAllowDeletionsAsksFirst: allowing deletions needs a second,
// explicit answer; cancelling deletes nothing, confirming answers the
// proposal shown, once (PLAN.md §11.3, §17 item 6).
func TestAllowDeletionsAsksFirst(t *testing.T) {
	a, m := testApp(t, true)
	a.page = pageActivity
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	if !tt.HasText(L.ReviewWouldDeleteServer(142)) {
		t.Fatal("the review does not say which way the deletion goes")
	}
	if err := tt.Click(L.ActionAllowDeletions(142)); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if !a.confirm.open {
		t.Fatal("no question before allowing the deletions")
	}
	shoot(t, tt, "activity-confirm-delete")
	if err := tt.Click(L.ActionCancel()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if _, ok := pendingReview(m.Snapshot(), "f-photos"); a.confirm.open || !ok {
		t.Fatal("cancel did not leave the proposal as it was")
	}
	if err := tt.Click(L.ActionAllowDeletions(142)); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if err := tt.Click(L.ConfirmDelete()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if _, ok := pendingReview(m.Snapshot(), "f-photos"); ok {
		t.Fatal("the approved proposal is still pending")
	}
	if tt.HasText(L.ReviewWouldDeleteServer(142)) {
		t.Fatal("the review still shows")
	}
}

// clickRole clicks the control with a role and name, not a label beside
// it with the same words (a Settings row's title and its switch).
func clickRole(t *testing.T, tt *ui.Tester, r ui.Role, name string) {
	t.Helper()
	e, ok := tt.AccessByRole(r, name)
	if !ok {
		t.Fatalf("no %q to click", name)
	}
	tt.ClickAt(e.Bounds.X+e.Bounds.W/2, e.Bounds.Y+e.Bounds.H/2)
}

// TestPauseEverySwitch: Settings' Pause every folder pauses through SyncD
// and leaves a folder's own pause when switched back (INV-13).
func TestPauseEverySwitch(t *testing.T) {
	a, m := testApp(t, true)
	a.openSettings(spSync)
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	clickRole(t, tt, ui.RoleSwitch, L.PauseEverything())
	tt.Frame()
	if !m.Snapshot().Daemon.AllPaused {
		t.Fatal("the switch did not pause every folder")
	}
	clickRole(t, tt, ui.RoleSwitch, L.PauseEverything())
	tt.Frame()
	if s := m.Snapshot(); s.Daemon.AllPaused {
		t.Fatal("the switch did not resume")
	} else if f, _ := s.Folder("f-reports"); !f.UserPaused {
		t.Fatal("resuming everything undid a folder's own pause")
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

// TestOverall: the overall state never reads as up to date from stale,
// unknown or unsupported backend state (PLAN.md §13.6–§13.7, INV-15).
func TestOverall(t *testing.T) {
	now := time.Now()
	live := syncd.Daemon{Connected: true, StateAvailable: true, StartupState: syncd.StartupNormal, Capabilities: map[string]bool{"fullSync": true}}
	preview := live
	preview.Capabilities = map[string]bool{"fullSync": false}
	diagnostic := syncd.Daemon{StartupState: syncd.StartupDiagnostic, Capabilities: map[string]bool{"fullSync": true}}
	stale := live
	stale.Connected = false
	cases := []struct {
		name string
		s    syncd.Snapshot
		want overall
	}{
		{"not connected", syncd.Snapshot{}, overallUnknown},
		{"stale green IPC 13.6", syncd.Snapshot{Daemon: stale, Folders: []syncd.Folder{{State: syncd.FolderUpToDate}}}, overallUnknown},
		{"diagnostic startup", syncd.Snapshot{Daemon: diagnostic}, overallUnknown},
		{"full sync not supported", syncd.Snapshot{Daemon: preview, Folders: []syncd.Folder{{State: syncd.FolderUpToDate}}}, overallFinished},
		{"paused wins", syncd.Snapshot{Daemon: syncd.Daemon{Connected: true, StateAvailable: true, AllPaused: true}, Folders: []syncd.Folder{{State: syncd.FolderSyncing}}}, overallPaused},
		{"review before syncing", syncd.Snapshot{Daemon: live, Folders: []syncd.Folder{{State: syncd.FolderSyncing}, {State: syncd.FolderNeedsReview}}}, overallNeedsYou},
		{"refused password", syncd.Snapshot{Daemon: live, Accounts: []syncd.Account{{ID: "a", SignedIn: true}}, Folders: []syncd.Folder{{AccountID: "a", State: syncd.FolderError, ReasonCode: "AUTH_REQUIRED"}}}, overallNeedsYou},
		{"syncing", syncd.Snapshot{Daemon: live, Folders: []syncd.Folder{{State: syncd.FolderSyncing}, {State: syncd.FolderUpToDate}}}, overallSyncing},
		{"all offline", syncd.Snapshot{Daemon: live, Folders: []syncd.Folder{{State: syncd.FolderOffline}}}, overallOffline},
		{"up to date", syncd.Snapshot{Daemon: live, Folders: []syncd.Folder{{State: syncd.FolderUpToDate}}}, overallUpToDate},
		{"sample", syncd.NewMock(true, func() time.Time { return now }).Snapshot(), overallNeedsYou},
	}
	for _, c := range cases {
		if got := overallOf(c.s); got != c.want {
			t.Errorf("%s: %v, want %v", c.name, got, c.want)
		}
	}
}

// snapClient is a Mock whose snapshot a test sets: what a real SyncD may
// report that the Mock does not.
type snapClient struct {
	*syncd.Mock
	s syncd.Snapshot
}

func (c *snapClient) Snapshot() syncd.Snapshot { return c.s }

// snapApp is the interface over a fixed snapshot.
func snapApp(t *testing.T, s syncd.Snapshot) *App {
	t.Helper()
	a := New(&snapClient{Mock: syncd.NewMock(false, nil), s: s}, "")
	a.later = func(fn func()) { fn() }
	a.async = func(fn func()) { fn() }
	a.openURL = func(string) {}
	a.Start()
	return a
}

// realSnapshot is what SyncD 1.9 reports for one signed-in account with
// one folder: fixed policy, no automatic switching, full sync and
// on-demand files not yet supported capabilities.
func realSnapshot() syncd.Snapshot {
	return syncd.Snapshot{
		Daemon:    syncd.Daemon{Connected: true, StateAvailable: true, StartupState: syncd.StartupNormal, Version: "0.1.0", Protocol: 1, ProtocolMinor: 9, Capabilities: map[string]bool{"login": true, "multiAccount": true, "fullSync": false, "endpointAutoSwitch": false, "virtualFiles": false}},
		Accounts:  []syncd.Account{{ID: "acc", User: "user", DisplayName: "User", Server: "cloud.example.invalid", Auth: syncd.AuthSignedIn, SignedIn: true}},
		Folders:   []syncd.Folder{{ID: "fold", AccountID: "acc", Name: "Docs", LocalPath: `C:\Sync\Docs`, RemotePath: "/Docs", State: syncd.FolderUpToDate, Mode: syncd.FolderFull}},
		Endpoints: []syncd.Endpoint{{ID: "ep-main", AccountID: "acc", Name: "cloud.example.invalid", Kind: syncd.KindInternet, URL: "https://cloud.example.invalid", Main: true, Approved: true, Verified: syncd.VerifyVerified, Reachable: true, InUse: true}, {ID: "ep-alt", AccountID: "acc", Name: "Alternate", Kind: syncd.KindInternet, URL: "https://alt.example.invalid", Approved: true, Verified: syncd.VerifyPending, Blocked: true, BlockedReason: "VERIFICATION_PENDING"}},
		Networks:  []syncd.Network{{AccountID: "acc", Policy: syncd.PolicyFixed, FixedEndpointID: "ep-main", ActiveID: "ep-main"}},
		Switches:  []syncd.Switch{}, Decisions: []syncd.Decision{}, Activity: []syncd.Activity{},
	}
}

// TestUnknownIsNotEmpty: while SyncD's lists are unknown (not connected,
// or a diagnostic start), the window shows neither first-run setup nor an
// empty account, and nothing healthy (PLAN.md §13.6, INV-15).
func TestUnknownIsNotEmpty(t *testing.T) {
	cases := []struct {
		name string
		d    syncd.Daemon
		want func() string
	}{
		{"connecting", syncd.Daemon{}, L.DaemonDisconnected},
		{"database unavailable", syncd.Daemon{StartupState: syncd.StartupDatabaseUnavailable, Version: "0.1.0"}, L.StateUnavailableTitle},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			a := snapApp(t, syncd.Snapshot{Daemon: c.d})
			tt := ui.NewTester(a.view, 1200, 780)
			tt.Frame()
			if a.setupShown() || tt.HasText(L.SetupWelcomeTitle()) {
				t.Fatal("unknown state shown as a first run")
			}
			if !tt.HasText(c.want()) {
				t.Fatalf("does not say %q", c.want())
			}
			if tt.HasText(L.NoFolders()) || tt.HasText(L.StateUpToDate()) {
				t.Fatal("unknown state shown as empty or healthy")
			}
			shoot(t, tt, "status-"+c.name)
		})
	}
}

// TestRealDaemonCapabilities: with SyncD's real capabilities the window
// never offers automatic address choice, never shows green "up to date",
// and adding folders says file sync is not yet supported.
func TestRealDaemonCapabilities(t *testing.T) {
	a := snapApp(t, realSnapshot())
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	if tt.HasText(L.OverviewUpToDate()) || !tt.HasText(L.StateFinished()) {
		t.Fatal("an unsupported full sync reads as up to date")
	}
	shoot(t, tt, "real-overview")
	a.page = pageNetwork
	tt.Frame()
	if tt.HasText(L.NetworkModeFixed()) || !tt.HasText(L.AutoUnavailable()) {
		t.Fatal("automatic switching is offered without the capability")
	}
	if !tt.HasText(L.TrustPending()) || tt.HasText(L.TrustBlocked()) {
		t.Fatal("an address waiting for verification reads as a different server")
	}
	shoot(t, tt, "real-network")
	a.startAddFolders("acc")
	tt.Frame()
	if !tt.HasText(L.SyncPreview()) {
		t.Fatal("adding folders does not say file sync is not yet supported")
	}
	shoot(t, tt, "real-setup-folders")
}

// TestRefusedPasswordAsksToSignIn: an account whose server refused the app
// password (Auth revoked) puts its Sign in in browser where the user looks,
// and its folder says why it waits.
func TestRefusedPasswordAsksToSignIn(t *testing.T) {
	s := realSnapshot()
	s.Accounts[0].Auth, s.Accounts[0].SignedIn = syncd.AuthRevoked, false
	s.Folders[0].State, s.Folders[0].ReasonCode = syncd.FolderError, "AUTH_REQUIRED"
	a := snapApp(t, s)
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	if !tt.HasText(L.AttnSignInTitle(accountName(s.Accounts[0]))) || !tt.HasText(L.ActionSignInBrowser()) {
		t.Fatal("no sign-in notice for a refused password")
	}
	a.page = pageFolders
	tt.Frame()
	if !tt.HasText(L.ReasonAuthRequired()) {
		t.Fatal("the folder does not say why it waits")
	}
	shoot(t, tt, "real-folders-signin")
}

// TestReasonCodesHaveWords: every reason code of the protocol is put into
// words without showing the code (AGENTS.md: no codes in the interface).
func TestReasonCodesHaveWords(t *testing.T) {
	b, err := os.ReadFile(filepath.Join("..", "..", "protocol", "contract.json"))
	if err != nil {
		t.Fatal(err)
	}
	var contract struct {
		ReasonCodes []string `json:"reasonCodes"`
	}
	if err := json.Unmarshal(b, &contract); err != nil || len(contract.ReasonCodes) == 0 {
		t.Fatalf("contract: %v", err)
	}
	// Codes only a programming error produces: the generic words suffice.
	generic := map[string]bool{"INVALID_PARAMS": true, "PROTOCOL_MISMATCH": true}
	for _, code := range contract.ReasonCodes {
		words := reasonText(code)
		if strings.Contains(words, code) {
			t.Errorf("%s shows its code", code)
		}
		if words == "" && !generic[code] {
			t.Errorf("%s has no words", code)
		}
	}
	if w, col, _ := folderWord(syncd.Folder{State: "somethingNew"}, syncd.Snapshot{}, &lightPal); w != L.FolderUnknown() || col == lightPal.success {
		t.Error("an unknown folder state is not shown as unknown")
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
