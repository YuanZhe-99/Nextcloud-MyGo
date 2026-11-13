package release

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// signedFeed signs a fake installer for version and arch with key into a
// served directory and returns the feed URL and the directory.
func signedFeed(t *testing.T, key ed25519.PrivateKey, version, arch string) (string, string) {
	t.Helper()
	dir := t.TempDir()
	setup := filepath.Join(t.TempDir(), "setup.exe")
	if err := os.WriteFile(setup, []byte("MZ installer "+version), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := SignUpdate(key, setup, version, arch, dir); err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(http.FileServer(http.Dir(dir)))
	t.Cleanup(srv.Close)
	return srv.URL + "/", dir
}

// editManifest rewrites one field of a served manifest.
func editManifest(t *testing.T, dir, arch string, edit func(*Update)) {
	t.Helper()
	p := filepath.Join(dir, UpdateManifestName(arch))
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	var u Update
	if err := json.Unmarshal(b, &u); err != nil {
		t.Fatal(err)
	}
	edit(&u)
	b, _ = json.Marshal(u)
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

func newKey(t *testing.T) (ed25519.PrivateKey, string) {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return priv, base64.StdEncoding.EncodeToString(pub)
}

// TestUpdateSignature covers the update channel's acceptance (PLAN.md §16.2,
// §23.3-5): only an archive signed with the trusted key, unchanged, stating
// the manifest's newer version and this architecture becomes an installer.
func TestUpdateSignature(t *testing.T) {
	key, pub := newKey(t)
	_, otherPub := newKey(t)
	other, _ := newKey(t)
	ctx := context.Background()

	cases := []struct {
		name    string
		trust   string
		current string
		// prepare changes the served files after signing.
		prepare func(t *testing.T, dir string)
		want    string // "" accepted, else part of the refusal
	}{
		{name: "accepted", trust: pub, current: "0.1.0"},
		{name: "wrong key", trust: otherPub, current: "0.1.0", want: "not signed with this app's key"},
		{name: "tampered payload", trust: pub, current: "0.1.0", prepare: func(t *testing.T, dir string) {
			p := filepath.Join(dir, UpdateArchiveName("0.1.1", "ARM64"))
			b, _ := os.ReadFile(p)
			i := bytes.Index(b, []byte("installer"))
			b[i] ^= 1 // same size, other bytes
			os.WriteFile(p, b, 0o644)
		}, want: "not signed with this app's key"},
		{name: "tampered manifest signature", trust: pub, current: "0.1.0", prepare: func(t *testing.T, dir string) {
			editManifest(t, dir, "ARM64", func(u *Update) {
				sum := [32]byte{}
				u.Signature = base64.StdEncoding.EncodeToString(ed25519.Sign(other, sum[:]))
			})
		}, want: "not signed with this app's key"},
		{name: "tampered manifest version", trust: pub, current: "0.1.0", prepare: func(t *testing.T, dir string) {
			editManifest(t, dir, "ARM64", func(u *Update) { u.Version = "0.2.0" })
		}, want: "the signed update is version 0.1.1, the manifest says 0.2.0"},
		{name: "tampered manifest size", trust: pub, current: "0.1.0", prepare: func(t *testing.T, dir string) {
			editManifest(t, dir, "ARM64", func(u *Update) { u.Size-- })
		}, want: "want"},
		{name: "wrong architecture", trust: pub, current: "0.1.0", prepare: func(t *testing.T, dir string) {
			// An x64 update served as the ARM64 one: correctly signed.
			setup := filepath.Join(t.TempDir(), "setup.exe")
			os.WriteFile(setup, []byte("MZ x64"), 0o644)
			x := t.TempDir()
			if err := SignUpdate(key, setup, "0.1.1", "x64", x); err != nil {
				t.Fatal(err)
			}
			b, _ := os.ReadFile(filepath.Join(x, UpdateManifestName("x64")))
			os.WriteFile(filepath.Join(dir, UpdateManifestName("ARM64")), b, 0o644)
			a, _ := os.ReadFile(filepath.Join(x, UpdateArchiveName("0.1.1", "x64")))
			os.WriteFile(filepath.Join(dir, UpdateArchiveName("0.1.1", "x64")), a, 0o644)
		}, want: "the update is for x64, this is ARM64"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			feed, dir := signedFeed(t, key, "0.1.1", "ARM64")
			if tc.prepare != nil {
				tc.prepare(t, dir)
			}
			u, err := CheckUpdate(ctx, HTTPGet, feed, "ARM64", tc.current)
			if err != nil || u == nil {
				t.Fatalf("CheckUpdate = %v, %v", u, err)
			}
			stage := t.TempDir()
			setup, err := u.Download(ctx, HTTPGet, tc.trust, "ARM64", tc.current, stage)
			if tc.want == "" {
				if err != nil {
					t.Fatal(err)
				}
				if b, _ := os.ReadFile(setup); string(b) != "MZ installer 0.1.1" {
					t.Fatalf("installer = %q", b)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("Download = %v, want %q", err, tc.want)
			}
			if entries, _ := os.ReadDir(stage); len(entries) != 0 {
				t.Fatalf("a refused update left %v", entries)
			}
		})
	}
}

// TestUpdateNotNewer: the manifest offers nothing at or below the running
// version, and a signed archive of an older version behind a manifest that
// claims a newer one is refused (PLAN.md §16.3, no downgrade).
func TestUpdateNotNewer(t *testing.T) {
	key, pub := newKey(t)
	ctx := context.Background()
	feed, dir := signedFeed(t, key, "0.1.1", "ARM64")
	for _, current := range []string{"0.1.1", "0.2.0"} {
		if u, err := CheckUpdate(ctx, HTTPGet, feed, "ARM64", current); err != nil || u != nil {
			t.Fatalf("running %s: CheckUpdate = %v, %v; want nothing", current, u, err)
		}
	}
	// Running 0.1.0 and 0.1.1's signed archive, but a manifest claiming 0.1.2
	// for it while the app runs 0.1.1.
	editManifest(t, dir, "ARM64", func(u *Update) { u.Version = "0.1.2" })
	u, err := CheckUpdate(ctx, HTTPGet, feed, "ARM64", "0.1.1")
	if err != nil || u == nil {
		t.Fatalf("CheckUpdate = %v, %v", u, err)
	}
	if _, err := u.Download(ctx, HTTPGet, pub, "ARM64", "0.1.1", t.TempDir()); err == nil {
		t.Fatal("an older signed update behind a newer manifest was accepted")
	}
}

// TestUpdateURLs: the feed is HTTPS, or HTTP only to a loopback address.
func TestUpdateURLs(t *testing.T) {
	for u, ok := range map[string]bool{
		"https://github.com/x/y/releases/latest/download/": true,
		"http://127.0.0.1:8080/":                           true,
		"http://[::1]/":                                    true,
		"http://localhost/":                                true,
		"http://github.com/":                               false,
		"file:///C:/updates/":                              false,
	} {
		if err := allowedURL(u); (err == nil) != ok {
			t.Errorf("allowedURL(%s) = %v", u, err)
		}
	}
}

// TestInstalledUpdate runs the update path of an installed package on this
// machine: it serves NEXTCLOUD_MYGO_UPDATE_DIR (tools/package.ps1's signed
// update of a -TestVersion package) over loopback HTTP, checks, downloads and
// verifies it with NEXTCLOUD_MYGO_UPDATE_KEY as the interface does, and hands
// it to the installer with release.StartSetup, which upgrades the running
// install of NEXTCLOUD_MYGO_UPDATE_CURRENT. tools/package-smoke.ps1
// -UpdateDir runs it and checks the result; it is skipped otherwise.
func TestInstalledUpdate(t *testing.T) {
	dir, key, current := os.Getenv("NEXTCLOUD_MYGO_UPDATE_DIR"), os.Getenv("NEXTCLOUD_MYGO_UPDATE_KEY"), os.Getenv("NEXTCLOUD_MYGO_UPDATE_CURRENT")
	if dir == "" || key == "" || current == "" {
		t.Skip("run by tools/package-smoke.ps1 -UpdateDir")
	}
	arch := map[string]string{"arm64": "ARM64", "amd64": "x64"}[runtime.GOARCH]
	srv := httptest.NewServer(http.FileServer(http.Dir(dir)))
	defer srv.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()
	u, err := CheckUpdate(ctx, HTTPGet, srv.URL+"/", arch, current)
	if err != nil || u == nil {
		t.Fatalf("CheckUpdate = %v, %v", u, err)
	}
	stage := t.TempDir()
	setup, err := u.Download(ctx, HTTPGet, key, arch, current, stage)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("verified the signed %s update; handing over to %s", u.Version, filepath.Base(setup))
	log := filepath.Join(stage, "setup.log")
	c, err := StartSetup(setup, log)
	if err != nil {
		t.Fatal(err)
	}
	err = c.Wait()
	f, _ := os.Open(log)
	out, _ := io.ReadAll(f)
	f.Close()
	t.Logf("setup:\n%s", out)
	if err != nil {
		t.Fatalf("setup: %v", err)
	}
}
