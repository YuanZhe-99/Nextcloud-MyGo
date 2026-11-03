package syncd

import (
	"errors"
	"testing"
	"time"
)

func fixed() time.Time { return time.Date(2026, 11, 3, 14, 6, 0, 0, time.UTC) }

func TestResolveDecisionOnce(t *testing.T) {
	cases := []struct {
		name   string
		digest string
		action Action
		want   error
		left   int // decisions left
	}{
		{"stale digest is refused", "sha256:other", ActionRestore, ErrStale, 2},
		{"later keeps it", "sha256:photos-142", ActionLater, nil, 2},
		{"restore answers it", "sha256:photos-142", ActionRestore, nil, 1},
		{"delete answers it", "sha256:photos-142", ActionDeleteRemote, nil, 1},
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

func TestRestoreDownloadsThenUpToDate(t *testing.T) {
	m := NewMock(true, fixed)
	if err := m.ResolveDecision("d-photos", "sha256:photos-142", ActionRestore); err != nil {
		t.Fatal(err)
	}
	f, _ := m.Snapshot().Folder("f-photos")
	if f.State != FolderSyncing || f.Phase != PhaseDownloading {
		t.Fatalf("Photos = %s/%s, want downloading", f.State, f.Phase)
	}
	for range 142 {
		m.Step()
	}
	if f, _ := m.Snapshot().Folder("f-photos"); f.State != FolderUpToDate {
		t.Fatalf("Photos = %s, want up to date", f.State)
	}
}

func TestPrepareUpdateWaitsForSafePoint(t *testing.T) {
	m := NewMock(true, fixed)
	if err := m.PrepareUpdate("0.1.1"); !errors.Is(err, ErrBusy) {
		t.Fatalf("with a sync running and a review waiting: %v, want busy", err)
	}
	empty := NewMock(false, fixed)
	if err := empty.PrepareUpdate("0.1.1"); err != nil {
		t.Fatalf("idle: %v", err)
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
