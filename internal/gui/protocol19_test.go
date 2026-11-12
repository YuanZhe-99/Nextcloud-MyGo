package gui

import (
	"context"
	"errors"
	"slices"
	"testing"

	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// recClient is a Mock that records what the interface asked of it, and
// whose sign-in ends with err when set: what SyncD answers that the Mock's
// sample does not.
type recClient struct {
	*syncd.Mock
	loginErr       error
	removedFolders int
	disconnect     string
}

func (c *recClient) WaitLogin(ctx context.Context, id string) (string, error) {
	if c.loginErr != nil {
		return "", c.loginErr
	}
	return c.Mock.WaitLogin(ctx, id)
}

func (c *recClient) RemoveFolder(id string) error {
	c.removedFolders++
	return c.Mock.RemoveFolder(id)
}

func (c *recClient) Disconnect() string { return c.disconnect }

// recApp is the interface over a recClient with the sample state.
func recApp(t *testing.T) (*App, *recClient) {
	t.Helper()
	rc := &recClient{Mock: syncd.NewMock(true, nil)}
	a := New(rc, "")
	a.later = func(fn func()) { fn() }
	a.async = func(fn func()) { fn() }
	a.openURL = func(string) {}
	a.Start()
	return a, rc
}

// TestAccountAuth: whether an account needs signing in again follows its
// Auth (protocol 1.9), with Sign in in browser where the user looks; only
// a daemon before 1.9 (no Auth) is inferred from its folders.
func TestAccountAuth(t *testing.T) {
	cases := []struct {
		name   string
		auth   syncd.AuthState
		reason string // the folder's reason code
		want   bool
		words  func() string
	}{
		{"signed in", syncd.AuthSignedIn, "", false, nil},
		{"signed in, folder refused, trusts Auth", syncd.AuthSignedIn, "AUTH_REQUIRED", false, nil},
		{"needs sign-in on this PC", syncd.AuthNeedsSignIn, "", true, L.AccountNeedsSignIn},
		{"revoked by the server", syncd.AuthRevoked, "AUTH_REQUIRED", true, L.AccountSignedOut},
		{"before 1.9, inferred from the folder", "", "AUTH_REQUIRED", true, L.AccountSignedOut},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			s := realSnapshot()
			s.Accounts[0].Auth, s.Accounts[0].SignedIn = c.auth, c.auth != syncd.AuthRevoked && c.auth != syncd.AuthNeedsSignIn
			s.Folders[0].ReasonCode = c.reason
			if got := needsSignIn(s, s.Accounts[0]); got != c.want {
				t.Fatalf("needsSignIn = %v, want %v", got, c.want)
			}
			a := snapApp(t, s)
			a.page = pageFolders
			tt := ui.NewTester(a.view, 1200, 780)
			tt.Frame()
			if tt.HasText(L.ActionSignInBrowser()) != c.want {
				t.Fatalf("Sign in in browser shown = %v, want %v", !c.want, c.want)
			}
			if c.words != nil && !tt.HasText(c.words()) {
				t.Fatalf("the account does not say why it waits")
			}
		})
	}
}

// TestSidebarListsAccounts: every account is in the sidebar up to four,
// then the rest are counted; each opens Accounts & folders.
func TestSidebarListsAccounts(t *testing.T) {
	servers := []string{"one.example", "two.example", "three.example", "four.example", "five.example"}
	for _, n := range []int{1, 2, 4, 5} {
		s := realSnapshot()
		s.Accounts = nil
		for _, srv := range servers[:n] {
			s.Accounts = append(s.Accounts, syncd.Account{ID: srv, User: "u", DisplayName: "U", Server: srv, Auth: syncd.AuthSignedIn, SignedIn: true})
		}
		a := snapApp(t, s)
		tt := ui.NewTester(a.view, 1200, 780)
		tt.Frame()
		shown := min(n, sidebarShown)
		if n > sidebarShown {
			shown = sidebarShown - 1
			if !tt.HasText(L.SidebarMoreAccounts(n - shown)) {
				t.Errorf("%d accounts: the rest are not counted", n)
			}
		}
		for i, srv := range servers[:n] {
			if tt.HasText(srv) != (i < shown) {
				t.Errorf("%d accounts: %s shown = %v", n, srv, i < shown)
			}
		}
		if err := tt.Click(accountName(s.Accounts[0])); err != nil {
			t.Fatal(err)
		}
		tt.Frame()
		if a.page != pageFolders {
			t.Errorf("%d accounts: the account does not open Accounts & folders", n)
		}
	}
}

// TestAddAccountNeedsMultiAccount: a further account is offered only where
// SyncD supports several; the first always is.
func TestAddAccountNeedsMultiAccount(t *testing.T) {
	for _, multi := range []bool{true, false} {
		s := realSnapshot()
		s.Daemon.Capabilities["multiAccount"] = multi
		a := snapApp(t, s)
		a.page = pageFolders
		tt := ui.NewTester(a.view, 1200, 780)
		tt.Frame()
		b, ok := tt.AccessByRole(ui.RoleButton, L.ActionAddAccount())
		if !ok || b.Disabled == multi {
			t.Fatalf("multiAccount %v: Add an account enabled = %v", multi, !b.Disabled)
		}
	}
}

// TestDuplicateAccountSaysSo: a sign-in to an account already added ends
// back at the server step with a plain message, not a failure.
func TestDuplicateAccountSaysSo(t *testing.T) {
	a, rc := recApp(t)
	rc.loginErr = syncd.ErrAccountExists
	a.startSetup()
	a.setup.server = "cloud.example.com"
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	a.beginLogin()
	tt.Frame()
	if a.setup.step != stepServer || a.setup.failed != L.ReasonAccountExists() {
		t.Fatalf("step %d, failed %q", a.setup.step, a.setup.failed)
	}
	if in, ok := tt.AccessByRole(ui.RoleTextField, L.ServerAddressLabel()); !ok || in.Description != L.ReasonAccountExists() {
		t.Fatalf("the duplicate is not said: %+v", in)
	}
	shoot(t, tt, "setup-account-exists")
}

// TestRemoveAccountKeepsItsFolders: removing an account is one command;
// SyncD stops its folders itself (protocol 1.9), and the folders are not
// removed one by one first.
func TestRemoveAccountKeepsItsFolders(t *testing.T) {
	a, rc := recApp(t)
	a.removeAccount("a-home")
	if rc.removedFolders != 0 {
		t.Fatalf("%d folders removed before the account", rc.removedFolders)
	}
	if _, ok := rc.Snapshot().Account("a-home"); ok {
		t.Fatal("the account stays")
	}
}

// TestFolderStates: a folder's phase and files so far, and a full server,
// as the Overview and Accounts & folders show them; a full server needs
// the user though its cycle finished.
func TestFolderStates(t *testing.T) {
	cases := []struct {
		name    string
		f       func(*syncd.Folder)
		overall overall
		texts   []string
	}{
		{"checking", func(f *syncd.Folder) { f.State, f.Phase = syncd.FolderSyncing, syncd.PhaseChecking }, overallSyncing,
			[]string{L.DetailChecking("Docs"), L.FolderChecking()}},
		{"uploading", func(f *syncd.Folder) { f.State, f.Phase, f.FilesDone = syncd.FolderSyncing, syncd.PhaseUploading, 3 }, overallSyncing,
			[]string{L.DetailUploading("Docs", 3), L.FolderUploading(3)}},
		{"downloading", func(f *syncd.Folder) { f.State, f.Phase, f.FilesDone = syncd.FolderSyncing, syncd.PhaseDownloading, 1 }, overallSyncing,
			[]string{L.DetailDownloading("Docs", 1), L.FolderDownloading(1)}},
		{"server storage full", func(f *syncd.Folder) {
			f.State, f.Phase, f.ReasonCode = syncd.FolderUpToDate, "", "REMOTE_QUOTA_EXCEEDED"
		}, overallNeedsYou,
			[]string{L.AttnQuotaTitle("user@cloud.example.invalid"), L.FolderStorageFull(), L.ReasonQuota()}},
		{"finished", func(f *syncd.Folder) { f.State, f.Phase = syncd.FolderUpToDate, "" }, overallFinished,
			[]string{L.StateFinished()}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			s := realSnapshot()
			c.f(&s.Folders[0])
			if got := overallOf(s); got != c.overall {
				t.Fatalf("overall = %v, want %v", got, c.overall)
			}
			a := snapApp(t, s)
			tt := ui.NewTester(a.view, 1200, 780)
			for _, pg := range []page{pageOverview, pageFolders} {
				a.page = pg
				tt.Frame()
				if !slices.ContainsFunc(c.texts, tt.HasText) {
					t.Fatalf("page %d shows none of %q", pg, c.texts)
				}
			}
			if quota := c.overall == overallNeedsYou; quota != tt.HasText(L.FolderStorageFull()) {
				t.Fatalf("server storage full shown = %v", !quota)
			}
		})
	}
}

// TestFolderModeOffered: Full files or On demand is offered only with the
// virtualFiles capability; the choice reaches AddFolder, and without it no
// mode is sent and SyncD keeps full files.
func TestFolderModeOffered(t *testing.T) {
	cases := []struct {
		name    string
		virtual bool
		choose  int
		want    syncd.FolderMode
	}{
		{"without virtualFiles", false, 1, syncd.FolderFull},
		{"full files", true, 0, syncd.FolderFull},
		{"on demand", true, 1, syncd.FolderOnDemand},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			a, m := testApp(t, true)
			m.SetCapability("virtualFiles", c.virtual)
			a.startAddFolders("a-home")
			tt := ui.NewTester(a.view, 1200, 780)
			tt.Frame()
			if tt.HasText(L.ModeLabel()) != c.virtual {
				t.Fatalf("mode offered = %v, want %v", !c.virtual, c.virtual)
			}
			if c.virtual {
				shoot(t, tt, "setup-folders-mode")
			}
			a.setup.chosen = map[string]bool{"/Work": true}
			a.setup.mode = c.choose
			tt.Frame()
			if err := tt.Click(L.ActionStartSyncing()); err != nil {
				t.Fatal(err)
			}
			tt.Frame()
			for _, f := range m.Snapshot().Folders {
				if f.RemotePath == "/Work" {
					if f.Mode != c.want {
						t.Fatalf("mode = %q, want %q", f.Mode, c.want)
					}
					a.page = pageFolders
					tt.Frame()
					if c.virtual && !tt.HasText(modeWord(c.want)) {
						t.Fatal("the folder's mode is not shown")
					}
					return
				}
			}
			t.Fatal("the folder was not added")
		})
	}
}

// TestEndpointStates: an address says why it is blocked, in words; a
// security block offers neither Verify now nor Use this one (INV-04).
func TestEndpointStates(t *testing.T) {
	cases := []struct {
		name   string
		e      func(*syncd.Endpoint)
		words  string
		action string // a button the card must not offer, or ""
	}{
		{"certificate", func(e *syncd.Endpoint) { e.Blocked, e.BlockedReason = true, "TLS_ERROR" }, L.BlockedTLS(), L.ActionVerifyNow()},
		{"different server", func(e *syncd.Endpoint) {
			e.Blocked, e.BlockedReason, e.Verified = true, "ENDPOINT_IDENTITY_MISMATCH", syncd.VerifyFailed
		}, L.TrustBlocked(), L.ActionVerifyNow()},
		{"never allowed", func(e *syncd.Endpoint) { e.Blocked, e.BlockedReason = true, "ENDPOINT_UNTRUSTED" }, L.BlockedUntrusted(), L.ActionVerifyNow()},
		{"waits for sign-in", func(e *syncd.Endpoint) {
			e.Verified, e.Blocked, e.BlockedReason = syncd.VerifyVerified, true, "AUTH_REQUIRED"
		}, L.BlockedSignIn(), L.ActionUseThis()},
		{"other network", func(e *syncd.Endpoint) {
			e.Verified, e.Blocked, e.BlockedReason = syncd.VerifyVerified, true, "NETWORK_UNAVAILABLE"
		}, L.BlockedNetwork(), L.ActionUseThis()},
		{"verification under way", func(e *syncd.Endpoint) { e.Blocked, e.BlockedReason = true, "VERIFICATION_PENDING" }, L.TrustPending(), ""},
		{"fast and usable", func(e *syncd.Endpoint) {
			e.Verified, e.Blocked, e.BlockedReason, e.Reachable, e.LatencyMs = syncd.VerifyVerified, false, "", true, 8
		}, L.LatencyMs("8"), ""},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			s := realSnapshot()
			c.e(&s.Endpoints[1])
			a := snapApp(t, s)
			a.page = pageNetwork
			tt := ui.NewTester(a.view, 1200, 780)
			tt.Frame()
			if !tt.HasText(c.words) {
				t.Fatalf("the address does not say %q", c.words)
			}
			if c.action != "" && tt.HasText(c.action) {
				t.Fatalf("a blocked address offers %q", c.action)
			}
		})
	}
}

// TestWhyWords: each reason SyncD gives for the address in use has words;
// faster says the answer times when both are known (no speedup is sent),
// and a pending switch is not said twice.
func TestWhyWords(t *testing.T) {
	s := syncd.NewMock(true, nil).Snapshot()
	lan, _ := s.Endpoint("e-lan")
	n, _ := s.Network("a-home")
	switching := n
	switching.Switching = true
	cases := []struct {
		r    syncd.Reason
		n    syncd.Network
		want string
	}{
		{syncd.ReasonVerified, n, L.WhyVerified()},
		{syncd.ReasonFaster, n, L.WhyFasterMs("8", "64")},
		{syncd.ReasonStable, n, L.WhyStable()},
		{syncd.ReasonUserChoice, n, L.WhyUser()},
		{syncd.ReasonJoinedNetwork, n, L.WhyJoined()},
		{syncd.ReasonUnreachable, n, L.WhyUnreachable()},
		{syncd.ReasonPostponed, n, L.WhyPostponed()},
		{syncd.ReasonPostponed, switching, ""},
	}
	for _, c := range cases {
		if got := whyWords(s, lan, c.n, c.r); got != c.want {
			t.Errorf("%s (switching %v) = %q, want %q", c.r, c.n.Switching, got, c.want)
		}
	}
}

// TestDecisions19: every deletion review shows with its groups counted
// (no size is sent) and only Decide later and Allow; a conflict is closed
// with resolved, once.
func TestDecisions19(t *testing.T) {
	a, m := testApp(t, true)
	a.page = pageActivity
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	if !tt.HasText(L.ReviewFiles(96)) {
		t.Fatal("a group is not counted")
	}
	if err := tt.Click(L.ActionMarkResolved()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	for _, d := range m.Snapshot().Decisions {
		if d.Kind == syncd.DecisionConflict {
			t.Fatal("Mark resolved did not close the conflict")
		}
	}

	// Two folders of two accounts each wait for a review.
	s := m.Snapshot()
	s.Decisions = append(s.Decisions, syncd.Decision{ID: "d-team", Kind: syncd.DecisionBulkDeletions, AccountID: "a-work", FolderID: "f-team", Count: 7, LocalDeletes: 7, Groups: []syncd.FileGroup{{Name: "Plans", Files: 7}}})
	b := snapApp(t, s)
	b.page = pageActivity
	tt = ui.NewTester(b.view, 1200, 900)
	tt.Frame()
	for _, n := range []int{142, 7} {
		if !tt.HasText(L.ReviewTitle(n)) || !tt.HasText(L.ActionAllowDeletions(n)) {
			t.Errorf("the review of %d files is not shown with its answer", n)
		}
	}
	shoot(t, tt, "activity-two-reviews")
}

// TestUpdatePostponed: SyncD's reason for not being at a safe point tells
// a deletion the user can review from a sync that settles on its own.
func TestUpdatePostponed(t *testing.T) {
	m := syncd.NewMock(true, nil)
	cases := []struct {
		name               string
		err                error
		postponed, waitDel bool
	}{
		{"deletion waits", m.PrepareUpdate("0.1.1"), true, true},
		{"sync settles", nil, false, false},
		{"no connection", syncd.ErrBusy, true, false},
		{"failed", errors.New("x"), false, false},
	}
	for _, c := range cases {
		if p, d := postponement(c.err); p != c.postponed || d != c.waitDel {
			t.Errorf("%s: postponed %v deletion %v", c.name, p, d)
		}
	}
	if err := m.ResolveDecision("d-photos", "sha256:photos-142", syncd.ActionApproveDeletion); err != nil {
		t.Fatal(err)
	}
	if p, d := postponement(m.PrepareUpdate("0.1.1")); !p || d {
		t.Errorf("files moving: postponed %v deletion %v", p, d)
	}
}

// TestDiagnosticsStartupAndDisconnect: SyncD's start report and why the
// pipe last ended are in Diagnostics, and nowhere in the pages.
func TestDiagnosticsStartupAndDisconnect(t *testing.T) {
	a, rc := recApp(t)
	rc.disconnect = "eof"
	tt := ui.NewTester(a.view, 1200, 780)
	for _, pg := range []page{pageOverview, pageFolders, pageNetwork, pageActivity} {
		a.page = pg
		tt.Frame()
		if tt.HasText(L.DiagSelfCheck()) || tt.HasText(L.DiagDisconnect()) {
			t.Fatalf("page %d shows Diagnostics' rows", pg)
		}
	}
	a.openSettings(spDiagnostics)
	tt.Frame()
	for _, want := range []string{L.DiagSelfCheck(), L.DiagUpgradeValue("—", "10", "11"), L.DiagDisconnect(), "eof"} {
		if !tt.HasText(want) {
			t.Errorf("Diagnostics does not show %q", want)
		}
	}
	shoot(t, tt, "settings-diagnostics-startup")
	if rows := connectionRows(syncd.StartupReport{}, syncd.NewMock(false, nil)); len(rows) != 0 {
		t.Errorf("rows for nothing known: %v", rows)
	}
}
