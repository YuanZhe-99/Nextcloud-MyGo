package syncd

import (
	"errors"
	"testing"
	"time"
)

func fixed() time.Time { return time.Date(2026, 11, 3, 14, 6, 0, 0, time.UTC) }

// TestResolveDecisionOnce: a deletion proposal is answered once, bound to
// its digest, with only the answers SyncD accepts (PLAN.md §11.3, INV-11).
func TestResolveDecisionOnce(t *testing.T) {
	cases := []struct {
		name   string
		digest string
		action Action
		want   error
		left   int // decisions left
	}{
		{"stale digest is refused", "sha256:other", ActionApproveDeletion, ErrStale, 2},
		{"later keeps it", "sha256:photos-142", ActionLater, nil, 2},
		{"a conflict answer is refused", "sha256:photos-142", ActionResolved, ErrRefused, 2},
		{"approval consumes it", "sha256:photos-142", ActionApproveDeletion, nil, 1},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			m := NewMock(true, fixed)
			if err := m.ResolveDecision("d-photos", c.digest, c.action); !errors.Is(err, c.want) {
				t.Fatalf("err = %v, want %v", err, c.want)
			}
			if n := len(m.Snapshot().Decisions); n != c.left {
				t.Fatalf("%d decisions left, want %d", n, c.left)
			}
			if c.left == 1 {
				// A second answer to the same proposal changes nothing (INV-11).
				if err := m.ResolveDecision("d-photos", c.digest, c.action); !errors.Is(err, ErrNotFound) {
					t.Fatalf("second answer: %v", err)
				}
			}
		})
	}
}

// TestExpiredProposalIsStale: an approval after the proposal expired is
// refused; the folder is checked again instead (PLAN.md §11.3 item 7).
func TestExpiredProposalIsStale(t *testing.T) {
	now := fixed()
	m := NewMock(true, func() time.Time { return now })
	now = now.Add(11 * time.Minute)
	if err := m.ResolveDecision("d-photos", "sha256:photos-142", ActionApproveDeletion); !errors.Is(err, ErrStale) {
		t.Fatalf("expired approval: %v, want stale", err)
	}
}

// TestResumeAllKeepsUserPause: resuming everything never undoes a folder
// the user paused on its own (INV-13, INV-14).
func TestResumeAllKeepsUserPause(t *testing.T) {
	m := NewMock(true, fixed)
	if err := m.PauseAll(); err != nil {
		t.Fatal(err)
	}
	if err := m.ResumeAll(); err != nil {
		t.Fatal(err)
	}
	s := m.Snapshot()
	reports, _ := s.Folder("f-reports")
	if reports.State != FolderPaused || !reports.UserPaused {
		t.Fatalf("Reports = %+v, want still paused by the user", reports)
	}
	photos, _ := s.Folder("f-photos")
	if photos.State != FolderNeedsReview {
		t.Fatalf("Photos = %s, want still waiting for review", photos.State)
	}
}

func TestSyncNowRefusedWhileBlocked(t *testing.T) {
	m := NewMock(true, fixed)
	for _, id := range []string{"f-photos", "f-team", "f-reports"} {
		if err := m.SyncNow(id); !errors.Is(err, ErrRefused) {
			t.Errorf("SyncNow(%s) = %v, want refused", id, err)
		}
	}
	if err := m.SyncNow("f-docs"); err != nil {
		t.Fatal(err)
	}
	if f, _ := m.Snapshot().Folder("f-docs"); f.State != FolderSyncing {
		t.Fatalf("Documents = %s, want syncing", f.State)
	}
}

// TestApprovalSyncsTheFolder: after an approval the folder syncs again and
// the deletion is in the history.
func TestApprovalSyncsTheFolder(t *testing.T) {
	m := NewMock(true, fixed)
	if err := m.ResolveDecision("d-photos", "sha256:photos-142", ActionApproveDeletion); err != nil {
		t.Fatal(err)
	}
	s := m.Snapshot()
	if f, _ := s.Folder("f-photos"); f.State != FolderSyncing {
		t.Fatalf("Photos = %s, want syncing", f.State)
	}
	if s.Activity[0].Kind != ActDeleted || s.Activity[0].Count != 142 {
		t.Fatalf("history = %+v, want the deletion first", s.Activity[0])
	}
}

// TestPrepareUpdateWaitsForSafePoint: an update waits while a deletion
// waits for the user or files move, each busy with SyncD's own reason
// (protocol 1.9), and goes ahead when idle (PLAN.md §16.2).
func TestPrepareUpdateWaitsForSafePoint(t *testing.T) {
	cases := []struct {
		name   string
		mock   func() *Mock
		reason string
	}{
		{"a deletion waits", func() *Mock { return NewMock(true, fixed) }, "DECISION_PENDING"},
		{"files move", func() *Mock {
			m := NewMock(true, fixed)
			if err := m.ResolveDecision("d-photos", "sha256:photos-142", ActionApproveDeletion); err != nil {
				t.Fatal(err)
			}
			return m
		}, "QUIESCE_STALLED"},
		{"idle", func() *Mock { return NewMock(false, fixed) }, ""},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			err := c.mock().PrepareUpdate("0.1.1")
			if got := ReasonOf(err); got != c.reason || (c.reason != "") != errors.Is(err, ErrBusy) {
				t.Fatalf("err = %v (%q), want %q", err, got, c.reason)
			}
		})
	}
}

// TestDuplicateSignInRefused: signing in again to a server already added
// as a new account is ACCOUNT_EXISTS, as SyncD refuses it; nothing is added.
func TestDuplicateSignInRefused(t *testing.T) {
	m := NewMock(true, fixed)
	l, err := m.StartLogin("cloud.example.com")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := m.WaitLogin(t.Context(), l.ID); !errors.Is(err, ErrAccountExists) {
		t.Fatalf("WaitLogin = %v, want ErrAccountExists", err)
	}
	if n := len(m.Snapshot().Accounts); n != 2 {
		t.Fatalf("%d accounts, want 2", n)
	}
}

// TestRemoveAccountWithFolders: an account is removed with its folders,
// busy or not; nothing of it stays in the state (protocol 1.9).
func TestRemoveAccountWithFolders(t *testing.T) {
	m := NewMock(true, fixed)
	if err := m.RemoveAccount("a-home"); err != nil {
		t.Fatal(err)
	}
	s := m.Snapshot()
	for _, f := range s.Folders {
		if f.AccountID == "a-home" {
			t.Fatalf("folder %s of the removed account stays", f.ID)
		}
	}
	if _, ok := s.Account("a-home"); ok || len(s.Accounts) != 1 {
		t.Fatalf("accounts = %+v", s.Accounts)
	}
}

func TestRemoveEndpointKeepsTheOneInUse(t *testing.T) {
	m := NewMock(true, fixed)
	for _, id := range []string{"e-lan", "e-wan"} {
		if err := m.RemoveEndpoint(id); !errors.Is(err, ErrRefused) {
			t.Errorf("RemoveEndpoint(%s) = %v, want refused", id, err)
		}
	}
	if err := m.RemoveEndpoint("e-nas"); err != nil {
		t.Fatal(err)
	}
}

func TestHostOf(t *testing.T) {
	cases := map[string]string{
		"cloud.example.com":                 "cloud.example.com",
		" https://cloud.example.com/index ": "cloud.example.com",
		"http://10.0.0.2:8080/":             "10.0.0.2:8080",
		"not a host":                        "",
	}
	for in, want := range cases {
		if got := hostOf(in); got != want {
			t.Errorf("hostOf(%q) = %q, want %q", in, got, want)
		}
	}
}
