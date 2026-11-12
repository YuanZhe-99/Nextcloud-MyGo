package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestRequireNotices keeps every package from being staged without the license
// texts of what it ships (PLAN.md §16.5): a missing file, a missing directory
// and an empty directory each refuse, naming the entry.
func TestRequireNotices(t *testing.T) {
	complete := func(t *testing.T) string {
		dir := t.TempDir()
		for _, rel := range noticesRequired {
			p := filepath.Join(dir, filepath.FromSlash(rel))
			if filepath.Ext(rel) == "" && filepath.Base(rel) != "COPYING" && filepath.Base(rel) != "LICENSE" {
				p = filepath.Join(p, "LICENSE.txt") // a directory entry with one text
			}
			if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(p, []byte("text"), 0o644); err != nil {
				t.Fatal(err)
			}
		}
		return dir
	}
	for _, tc := range []struct {
		name    string
		damage  func(dir string) error
		missing string
	}{
		{"complete", func(string) error { return nil }, ""},
		{"no product license", func(d string) error { return os.Remove(filepath.Join(d, "LICENSE.txt")) }, "LICENSE.txt"},
		{"no OpenSSL texts", func(d string) error { return os.RemoveAll(filepath.Join(d, "licenses", "OpenSSL")) }, "licenses/OpenSSL"},
		{"empty Qt module", func(d string) error {
			return os.Remove(filepath.Join(d, "licenses", "Qt", "qtbase", "LICENSE.txt"))
		}, "licenses/Qt/qtbase"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			dir := complete(t)
			if err := tc.damage(dir); err != nil {
				t.Fatal(err)
			}
			err := requireNotices(dir)
			if tc.missing == "" {
				if err != nil {
					t.Fatalf("complete bundle refused: %v", err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tc.missing) {
				t.Fatalf("got %v, want a refusal naming %s", err, tc.missing)
			}
		})
	}
	if err := requireNotices(""); err == nil {
		t.Fatal("staging without -notices was accepted")
	}
}
