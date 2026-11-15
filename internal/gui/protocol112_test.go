package gui

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// TestMergeWarnedOnce: a chosen folder that already holds files on this PC
// stops the folders step once with a warning that names it; the second
// click merges and adds it. Before 0.1.3 nothing said so.
func TestMergeWarnedOnce(t *testing.T) {
	a, m := testApp(t, true)
	a.startAddFolders("a-home")
	a.setup.base = t.TempDir()
	work := filepath.Join(a.setup.base, "Work")
	if err := os.MkdirAll(work, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(work, "notes.txt"), []byte("here"), 0o644); err != nil {
		t.Fatal(err)
	}
	a.setup.chosen = map[string]bool{"/Work": true}
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	before := len(m.Snapshot().Folders)
	if err := tt.Click(L.ActionStartSyncing()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if !tt.HasText(L.MergeExistingTitle("Work")) || len(m.Snapshot().Folders) != before {
		t.Fatal("a folder holding files was added without a warning")
	}
	shoot(t, tt, "setup-merge-warning")
	if err := tt.Click(L.ActionMergeContinue()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	if len(m.Snapshot().Folders) != before+1 || a.setup.active {
		t.Fatal("merging after the warning did not add the folder")
	}
}

// TestAvailabilityShown: a folder row says how an on-demand folder keeps
// its files (protocol 1.12).
func TestAvailabilityShown(t *testing.T) {
	a, m := testApp(t, true)
	if err := m.SetAvailability("f-projects", syncd.AvailabilityAlwaysLocal); err != nil {
		t.Fatal(err)
	}
	a.page = pageFolders
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	if !tt.HasText(L.AvailWordAlwaysLocal()) {
		t.Fatal("the folder row does not say its files are always on this PC")
	}
}

// TestRenameMainAddress renames the main address through its dialog: the
// name and kind reach SyncD, its trust does not change (protocol 1.12).
func TestRenameMainAddress(t *testing.T) {
	a, m := testApp(t, true)
	var main syncd.Endpoint
	for _, e := range m.Snapshot().Endpoints {
		if e.Main {
			main = e
			break
		}
	}
	a.page = pageNetwork
	a.rename = renameForm{open: true, id: main.ID, name: "Office", kind: 2}
	tt := ui.NewTester(a.view, 1200, 780)
	tt.Frame()
	shoot(t, tt, "network-rename")
	if err := tt.Click(L.ActionSave()); err != nil {
		t.Fatal(err)
	}
	tt.Frame()
	e, _ := m.Snapshot().Endpoint(main.ID)
	if e.Name != "Office" || e.Kind != syncd.KindVPN || e.Approved != main.Approved || e.Verified != main.Verified || a.rename.open {
		t.Fatalf("renamed address = %+v", e)
	}
}
