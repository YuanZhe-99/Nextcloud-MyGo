package main

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/release"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
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

// TestPrepare covers §16.2's safe point before files are replaced: a deletion
// waiting for the user stops the install at once (installing never decides
// it), a busy SyncD is waited for up to prepareLimit, and a SyncD that has
// gone needs nothing. The clock is fake; SyncD's refusals come from the Mock,
// which carries SyncD's own reason codes.
func TestPrepare(t *testing.T) {
	pending := syncd.NewMock(true, nil).PrepareUpdate("0.1.1")
	busy := syncd.NewMock(true, nil)
	if err := busy.ResolveDecision("d-photos", "sha256:photos-142", syncd.ActionApproveDeletion); err != nil {
		t.Fatal(err)
	}
	stalled := busy.PrepareUpdate("0.1.1")
	if syncd.ReasonOf(pending) != "DECISION_PENDING" || syncd.ReasonOf(stalled) != "QUIESCE_STALLED" {
		t.Fatalf("Mock refusals: %v, %v", pending, stalled)
	}
	for _, c := range []struct {
		name    string
		replies []error // the last one repeats
		alive   bool
		want    error
		asks    int // 0: more than one minute's worth
	}{
		{"not running", []error{syncd.ErrDisconnected}, false, nil, 1},
		{"ready", []error{nil}, true, nil, 1},
		{"deletion pending", []error{pending}, true, errDeletionPending, 1},
		{"stalled then ready", []error{syncd.ErrDisconnected, stalled, context.DeadlineExceeded, stalled, nil}, true, nil, 5},
		{"stalled past deadline", []error{stalled}, true, errNotQuiet, 0},
		{"never answers", []error{syncd.ErrDisconnected}, true, errNoAnswer, 0},
		{"refused otherwise", []error{syncd.ErrRefused}, true, errNotPrepared, 1},
	} {
		t.Run(c.name, func(t *testing.T) {
			clock := time.Date(2026, 11, 12, 9, 0, 0, 0, time.UTC)
			start, asks := clock, 0
			err := prepare(func() error {
				asks++
				return c.replies[min(asks, len(c.replies))-1]
			}, func() bool { return c.alive }, func() time.Time { return clock }, func(d time.Duration) { clock = clock.Add(d) })
			if err != c.want {
				t.Fatalf("prepare = %v, want %v", err, c.want)
			}
			if c.asks > 0 && asks != c.asks {
				t.Errorf("asked %d times, want %d", asks, c.asks)
			}
			if c.asks == 0 && (asks < 60 || clock.Sub(start) < prepareLimit || clock.Sub(start) > prepareLimit+time.Second) {
				t.Errorf("gave up after %d asks and %v, want after %v", asks, clock.Sub(start), prepareLimit)
			}
		})
	}
}

// TestReleaseOnDemand covers the uninstall's on-demand release: every
// on-demand folder is removed through SyncD and no full-file folder is, a
// busy folder is asked again until releaseLimit, and a refused or timed-out
// folder is named by its local path. The clock is fake; refusals come from
// the Mock where it has them.
func TestReleaseOnDemand(t *testing.T) {
	stalled := syncd.NewMock(true, nil)
	if err := stalled.ResolveDecision("d-photos", "sha256:photos-142", syncd.ActionApproveDeletion); err != nil {
		t.Fatal(err)
	}
	busy := stalled.PrepareUpdate("0.1.1")
	if !errors.Is(busy, syncd.ErrBusy) {
		t.Fatalf("Mock refusal %v is not ErrBusy", busy)
	}
	folders := []syncd.Folder{
		{ID: "f-docs", LocalPath: `D:\Docs`, Mode: syncd.FolderOnDemand},
		{ID: "f-full", LocalPath: `D:\Full`, Mode: syncd.FolderFull},
		{ID: "f-old", LocalPath: `D:\Old`}, // a daemon before protocol 1.8: full files
		{ID: "f-photos", LocalPath: `D:\Photos`, Mode: syncd.FolderOnDemand},
	}
	for _, c := range []struct {
		name    string
		replies map[string][]error // per folder; the last one repeats
		failed  []string           // local paths named in the error
		late    bool               // gave up only at releaseLimit
	}{
		{"released", nil, nil, false},
		{"already gone", map[string][]error{"f-docs": {syncd.ErrNotFound}}, nil, false},
		{"busy then released", map[string][]error{"f-docs": {busy, syncd.ErrDisconnected, context.DeadlineExceeded, syncd.ErrStale, nil}}, nil, false},
		{"refused", map[string][]error{"f-photos": {syncd.ErrRefused}}, []string{`D:\Photos`}, false},
		{"busy past the limit", map[string][]error{"f-docs": {busy}}, []string{`D:\Docs`}, true},
		{"listed after its removal", map[string][]error{"f-docs": {nil}, "f-stuck": nil}, []string{`D:\Docs`}, true},
	} {
		t.Run(c.name, func(t *testing.T) {
			clock := time.Date(2026, 11, 14, 9, 0, 0, 0, time.UTC)
			start := clock
			listed := map[string]bool{}
			for _, f := range folders {
				listed[f.ID] = true
			}
			_, stuck := c.replies["f-stuck"]
			asks := map[string]int{}
			err := releaseOnDemand(func() []syncd.Folder {
				var out []syncd.Folder
				for _, f := range folders {
					if listed[f.ID] {
						out = append(out, f)
					}
				}
				return out
			}, func(id string) error {
				if id == "f-full" || id == "f-old" {
					t.Fatalf("full-file folder %s removed", id)
				}
				asks[id]++
				err := error(nil)
				if r := c.replies[id]; len(r) > 0 {
					err = r[min(asks[id], len(r))-1]
				}
				if (err == nil || errors.Is(err, syncd.ErrNotFound)) && !stuck {
					listed[id] = false
				}
				return err
			}, func() time.Time { return clock }, func(d time.Duration) { clock = clock.Add(d) })
			if len(c.failed) == 0 {
				if err != nil {
					t.Fatalf("releaseOnDemand = %v", err)
				}
				if listed["f-docs"] || listed["f-photos"] || !listed["f-full"] || !listed["f-old"] {
					t.Errorf("left listed: %v", listed)
				}
				return
			}
			if err == nil {
				t.Fatal("releaseOnDemand succeeded, want a failure")
			}
			for _, p := range c.failed {
				if !strings.Contains(err.Error(), p) {
					t.Errorf("error %q does not name %s", err, p)
				}
			}
			if strings.Contains(err.Error(), `D:\Full`) {
				t.Errorf("error names a full-file folder: %q", err)
			}
			if took := clock.Sub(start); c.late != (took >= releaseLimit) || took > releaseLimit+2*time.Second {
				t.Errorf("gave up after %v, want late=%v (limit %v)", took, c.late, releaseLimit)
			}
		})
	}
}

// TestReleaseWaitsForConversion covers a folder of an earlier version that
// SyncD turns on-demand just after its start: it is released too, and a
// folder that stays full files (a reason is recorded, or it never settles)
// does not hold the uninstall beyond settleLimit.
func TestReleaseWaitsForConversion(t *testing.T) {
	for _, c := range []struct {
		name     string
		after    time.Duration // when SyncD decides; 0 = never
		reason   string        // the decision: "" converts, else stays full
		released bool
	}{
		{"converted after the first read", 3 * time.Second, "", true},
		{"stays full with a reason", 3 * time.Second, "FOLDER_OWNED", false},
		{"never settles", 0, "", false},
	} {
		t.Run(c.name, func(t *testing.T) {
			clock := time.Date(2026, 11, 14, 9, 0, 0, 0, time.UTC)
			start := clock
			removed := false
			err := releaseOnDemand(func() []syncd.Folder {
				f := syncd.Folder{ID: "f-old", LocalPath: `D:\Old`, Mode: syncd.FolderFull, Conversion: syncd.ConversionPending}
				if c.after > 0 && clock.Sub(start) >= c.after {
					if f.ConversionReason = c.reason; c.reason == "" {
						f.Mode, f.Conversion = syncd.FolderOnDemand, syncd.ConversionConverted
					}
				}
				if removed {
					return nil
				}
				return []syncd.Folder{f}
			}, func(string) error { removed = true; return nil },
				func() time.Time { return clock }, func(d time.Duration) { clock = clock.Add(d) })
			if err != nil || removed != c.released {
				t.Fatalf("releaseOnDemand = %v, removed %v, want %v", err, removed, c.released)
			}
			if took := clock.Sub(start); took > settleLimit+2*time.Second {
				t.Errorf("waited %v, limit %v", took, settleLimit)
			}
		})
	}
}

// TestUninstallSequence covers the uninstall's order: no program file is
// removed until every on-demand folder is released, a failed release
// deletes nothing, and user data that stayed is reported only after the
// program is gone.
func TestUninstallSequence(t *testing.T) {
	failure := errors.New("D:\\Docs could not be released")
	kept := errors.New("accounts and settings were kept")
	for _, c := range []struct {
		name       string
		kept, fail error
		want       string
		err        error
	}{
		{"released", nil, nil, "release,backstop,program", nil},
		{"release failed", nil, failure, "release", failure},
		{"user data kept", kept, nil, "release,backstop,program", kept},
	} {
		t.Run(c.name, func(t *testing.T) {
			var order []string
			err := uninstallSequence(func() (error, error) {
				order = append(order, "release")
				return c.kept, c.fail
			}, func() error {
				order = append(order, "backstop")
				return nil
			}, func() error {
				order = append(order, "program")
				return nil
			})
			if got := strings.Join(order, ","); got != c.want {
				t.Errorf("order = %s, want %s", got, c.want)
			}
			if !errors.Is(err, c.err) || (c.err == nil) != (err == nil) {
				t.Errorf("uninstallSequence = %v, want %v", err, c.err)
			}
		})
	}
}

// TestOurTag keeps the navigation backstop to entries SyncD tagged: never
// the official client's "Nextcloud", nor an untagged entry.
func TestOurTag(t *testing.T) {
	for _, c := range []struct {
		v    string
		want bool
	}{
		{"Nextcloud-MyGo", true},
		{"Nextcloud", false},
		{"", false},
		{"nextcloud-mygo", false},
		{"Nextcloud-MyGo ", false},
		{"Nextcloud-MyGo-dev", false},
	} {
		if got := ourTag(c.v); got != c.want {
			t.Errorf("ourTag(%q) = %v, want %v", c.v, got, c.want)
		}
	}
}

// TestRootAccount reads the account of this user's SyncD sync roots only:
// never the official client's, another user's or a malformed ID.
func TestRootAccount(t *testing.T) {
	sid := "S-1-5-21-1-2-3-1001"
	for _, c := range []struct {
		id, sid, want string
	}{
		{"Nextcloud-MyGo!" + sid + "!acc-1!fold-1", sid, "acc-1"},
		{"Nextcloud!" + sid + "!user@host!1", sid, ""},
		{"Nextcloud-MyGo!S-1-5-21-1-2-3-1002!acc-1!fold-1", sid, ""},
		{"Nextcloud-MyGo!" + sid + "!acc-1", sid, ""},
		{"Nextcloud-MyGo!" + sid + "!!fold-1", sid, ""},
		{"Nextcloud-MyGo!!acc-1!fold-1", "", ""},
	} {
		got, ok := rootAccount(c.id, c.sid)
		if got != c.want || ok != (c.want != "") {
			t.Errorf("rootAccount(%q) = %q, %v; want %q", c.id, got, ok, c.want)
		}
	}
	if rootPath(`D:\Docs\`) != rootPath(`d:\docs`) || rootPath(`D:\Docs`) == rootPath(`D:\Docs2`) {
		t.Error("rootPath does not compare roots as Windows does")
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
