package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/release"
)

// TestAdmit covers §16.3: reinstall and upgrade proceed; a downgrade, an
// older Control schema or another architecture is refused.
func TestAdmit(t *testing.T) {
	pkg := func(v, arch string, schema int) release.Manifest {
		return release.Manifest{Version: v, Arch: arch, SyncD: release.Half{ControlSchema: schema}}
	}
	cur := pkg("0.1.0", "ARM64", 9)
	for _, c := range []struct {
		name      string
		installed *release.Manifest
		next      release.Manifest
		refused   string
	}{
		{"first install", nil, pkg("0.1.0", "ARM64", 9), ""},
		{"reinstall", &cur, pkg("0.1.0", "ARM64", 9), ""},
		{"upgrade", &cur, pkg("0.1.1", "ARM64", 10), ""},
		{"downgrade", &cur, pkg("0.0.9", "ARM64", 9), "older"},
		{"older schema", &cur, pkg("0.1.0", "ARM64", 8), "newer format"},
		{"other architecture", &cur, pkg("0.1.0", "x64", 9), "Uninstall it first"},
	} {
		err := admit(c.installed, c.next)
		if c.refused == "" && err != nil || c.refused != "" && (err == nil || !strings.Contains(err.Error(), c.refused)) {
			t.Errorf("%s: admit = %v, want refusal %q", c.name, err, c.refused)
		}
	}
}

// payloadOf zips files and returns the archive with a manifest describing want.
func payloadOf(t *testing.T, files, want map[string]string) (*zip.Reader, release.Manifest) {
	t.Helper()
	m := release.Manifest{Product: "Nextcloud-MyGo", Version: "0.1.0", Arch: "ARM64"}
	for p, body := range want {
		sum := sha256.Sum256([]byte(body))
		m.Files = append(m.Files, release.File{Path: p, Size: int64(len(body)), SHA256: hex.EncodeToString(sum[:])})
	}
	var b bytes.Buffer
	w := zip.NewWriter(&b)
	for p, body := range files {
		f, _ := w.Create(p)
		f.Write([]byte(body))
	}
	w.Close()
	z, err := zip.NewReader(bytes.NewReader(b.Bytes()), int64(b.Len()))
	if err != nil && !errors.Is(err, zip.ErrInsecurePath) {
		t.Fatal(err)
	}
	return z, m
}

// TestExtract covers the §16.2 rule that a failed install never removes the
// last bootable version: a payload that does not match its manifest leaves
// no new directory and the current one untouched.
func TestExtract(t *testing.T) {
	good := map[string]string{"Nextcloud-MyGo.exe": "ui", "plugins/tls/q.dll": "tls"}
	for _, c := range []struct {
		name  string
		files map[string]string
		ok    bool
	}{
		{"complete", good, true},
		{"altered", map[string]string{"Nextcloud-MyGo.exe": "UI", "plugins/tls/q.dll": "tls"}, false},
		{"missing", map[string]string{"Nextcloud-MyGo.exe": "ui"}, false},
		{"extra", map[string]string{"Nextcloud-MyGo.exe": "ui", "plugins/tls/q.dll": "tls", "x.dll": "x"}, false},
		{"escaping", map[string]string{"../x.dll": "x"}, false},
	} {
		t.Run(c.name, func(t *testing.T) {
			root := t.TempDir()
			current := filepath.Join(root, "versions", "0.1.0-00000000")
			os.MkdirAll(current, 0o755)
			os.WriteFile(filepath.Join(current, "Nextcloud-MyGo.exe"), []byte("old"), 0o644)
			z, m := payloadOf(t, c.files, good)
			dir := filepath.Join(root, "versions", "0.1.0-11111111")
			err := extract(z, m, dir)
			if (err == nil) != c.ok {
				t.Fatalf("extract = %v, want ok=%v", err, c.ok)
			}
			entries, _ := os.ReadDir(filepath.Join(root, "versions"))
			if want := map[bool]int{true: 2, false: 1}[c.ok]; len(entries) != want {
				t.Errorf("versions has %d entries, want %d", len(entries), want)
			}
			if old, _ := os.ReadFile(filepath.Join(current, "Nextcloud-MyGo.exe")); string(old) != "old" {
				t.Error("the current version changed")
			}
			if _, err := os.Stat(filepath.Join(root, "x.dll")); err == nil {
				t.Error("a payload path escaped the version directory")
			}
		})
	}
}

// TestState round-trips install.json and lists the versions an install removes.
func TestState(t *testing.T) {
	root := t.TempDir()
	if _, _, ok, err := readState(root); ok || err != nil {
		t.Fatalf("empty root: ok=%v err=%v", ok, err)
	}
	dir := newVersionDir(root, "0.1.0")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, release.ManifestName), []byte(`{"product":"Nextcloud-MyGo","version":"0.1.0","arch":"ARM64","testBuild":true,"signed":false,"autoUpdate":false,
		"ui":{"executable":"Nextcloud-MyGo.exe","protocol":{"major":1,"minor":7}},"syncd":{"executable":"nextcloud-mygo-syncd.exe","protocol":{"major":1,"minor":7},"controlSchema":9},"files":[]}`), 0o644)
	if err := writeState(root, dir); err != nil {
		t.Fatal(err)
	}
	got, m, ok, err := readState(root)
	if err != nil || !ok || got != dir || m.SyncD.ControlSchema != 9 {
		t.Fatalf("readState = %q %v %v %v", got, m.SyncD.ControlSchema, ok, err)
	}
	old := filepath.Join(root, "versions", "0.0.9-aaaaaaaa")
	os.MkdirAll(old, 0o755)
	os.MkdirAll(dir+"x.partial", 0o755)
	if others := otherVersions(root, dir); len(others) != 2 {
		t.Errorf("otherVersions = %v, want the old and the partial directory", others)
	}
	os.WriteFile(filepath.Join(root, "install.json"), []byte(`{"current":"../elsewhere"}`), 0o644)
	if _, _, _, err := readState(root); err == nil {
		t.Error("install.json pointing outside versions/ was accepted")
	}
}

// TestRetarget covers R19's startup side: only this app's Run entries move to
// the new version (keeping their arguments) or go on uninstall; other
// applications' entries, the official client's included, stay untouched.
func TestRetarget(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows paths")
	}
	root := `C:\Users\u\AppData\Local\Programs\Nextcloud-MyGo`
	exe := root + `\versions\0.1.1-b\Nextcloud-MyGo.exe`
	for _, c := range []struct{ command, want string }{
		{`"` + root + `\versions\0.1.0-a\Nextcloud-MyGo.exe" --mygo-opened-at-login`, `"` + exe + `" --mygo-opened-at-login`},
		{`"C:\Users\U\APPDATA\LOCAL\PROGRAMS\NEXTCLOUD-MYGO\versions\0.1.0-a\Nextcloud-MyGo.exe"`, `"` + exe + `"`},
		{`"C:\Program Files\Nextcloud\nextcloud.exe" --background`, ""},
		{`"` + root + `-other\app.exe"`, ""},
		{`C:\Windows\app.exe -x`, ""},
	} {
		got, ok := retarget(c.command, root, exe)
		if c.want == "" {
			if ok || got != c.command {
				t.Errorf("retarget(%s) = %s, %v; want untouched", c.command, got, ok)
			}
		} else if !ok || got != c.want {
			t.Errorf("retarget(%s) = %s, want %s", c.command, got, c.want)
		}
	}
}
