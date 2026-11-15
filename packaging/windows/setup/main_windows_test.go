package main

import (
	"crypto/rand"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestSyncRoots reads this user's real sync root registrations (read-only):
// every one listed is a Nextcloud-MyGo root of this user's SID, never the
// official client's.
func TestSyncRoots(t *testing.T) {
	sid, err := userSID()
	if err != nil || !strings.HasPrefix(sid, "S-1-") {
		t.Fatalf("userSID = %q, %v", sid, err)
	}
	roots, err := syncRoots(sid)
	if err != nil {
		t.Fatal(err)
	}
	for _, r := range roots {
		if !strings.HasPrefix(r.id, "Nextcloud-MyGo!"+sid+"!") || r.account == "" {
			t.Errorf("listed %+v", r)
		}
	}
	t.Logf("%d Nextcloud-MyGo sync root(s) registered for this user", len(roots))
}

// TestRemoveAllUnderAppData checks that -remove-user-data removes a non-empty
// settings folder directly under %APPDATA% (packaging.md, uninstall). Go's
// os.RemoveAll failed there on a Windows 11 build with "openfdat ...: too many
// levels of symbolic links", so the uninstall kept the settings and exited 1.
func TestRemoveAllUnderAppData(t *testing.T) {
	config, err := os.UserConfigDir()
	if err != nil {
		t.Skip(err)
	}
	suffix := make([]byte, 6)
	rand.Read(suffix)
	dir := filepath.Join(config, "Nextcloud-MyGo-removetest-"+hex.EncodeToString(suffix))
	t.Cleanup(func() { removeTree(dir) })
	for _, f := range []string{"gui.json", filepath.Join("nested", "window-state.json")} {
		p := filepath.Join(dir, f)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("{}"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := removeAll(dir); err != nil {
		t.Fatalf("removeAll: %v", err)
	}
	if _, err := os.Lstat(dir); !os.IsNotExist(err) {
		t.Fatalf("folder still there: %v", err)
	}
	if err := removeAll(dir); err != nil {
		t.Fatalf("removing a missing folder: %v", err)
	}
}
