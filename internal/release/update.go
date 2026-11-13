package release

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/version"
)

// The update channel of a signed release, set by tools/package.ps1 with
// -ldflags -X only when the package is built with the update signing key
// (MYGO_UPDATER_PRIVATE_KEY): UpdateFeed is the directory the manifests are
// read from (ending in "/") and UpdateKey the public key of mygo.json's
// updates.publicKey. A build without them offers no updates.
var (
	UpdateFeed string
	UpdateKey  string
)

// Limits of what an update check downloads, as MyGo's updater has them.
const (
	maxUpdateManifest = 1 << 20
	maxUpdateArchive  = 1 << 30
)

// Entries of an update archive: what it says it is, and the installer.
const (
	updateInfoName  = "update.json"
	updateSetupName = "setup.exe"
)

var plainVersion = regexp.MustCompile(`^\d+\.\d+\.\d+$`)

// Update is a newer package offered by the update feed: the manifest
// update-windows-<goarch>.json in MyGo's update format (`mygo build`'s
// update.Manifest), whose archive holds this product's per-user installer.
// Notes: the manifest itself is not signed; the archive is (Ed25519 over its
// SHA-256, keys from `mygo keygen`), and Download trusts only what the signed
// archive's update.json states about product, version and architecture.
type Update struct {
	Version   string `json:"version"`
	Notes     string `json:"notes,omitempty"`
	Date      string `json:"date,omitempty"`
	URL       string `json:"url"`
	Size      int64  `json:"size"`
	Signature string `json:"signature"`

	// manifest is where the manifest was read, which a relative URL
	// resolves against.
	manifest *url.URL
}

// updateInfo is the update.json inside a signed archive.
type updateInfo struct {
	Product string `json:"product"`
	Version string `json:"version"`
	Arch    string `json:"arch"`
}

// Getter opens a URL of the update feed.
type Getter func(ctx context.Context, rawURL string) (io.ReadCloser, error)

// UpdateManifestName returns the feed's manifest of an architecture ("ARM64"
// or "x64"), named as MyGo names its targets: update-windows-arm64.json.
func UpdateManifestName(arch string) string {
	return "update-windows-" + map[string]string{"ARM64": "arm64", "x64": "amd64"}[arch] + ".json"
}

// UpdateArchiveName returns the signed archive of a version and architecture.
func UpdateArchiveName(version, arch string) string {
	return "Nextcloud-MyGo-" + version + "-" + strings.ToLower(arch) + "-update.zip"
}

// HTTPGet fetches rawURL for the update feed.
// Notes: only HTTPS, or plain HTTP to a loopback address (local tests), as
// MyGo's updater allows; a redirect may not leave HTTPS. TLS is verified as
// always.
func HTTPGet(ctx context.Context, rawURL string) (io.ReadCloser, error) {
	if err := allowedURL(rawURL); err != nil {
		return nil, err
	}
	c := &http.Client{CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if len(via) >= 10 {
			return errors.New("too many redirects")
		}
		return allowedURL(req.URL.String())
	}}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		return nil, err
	}
	resp, err := c.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		return nil, fmt.Errorf("GET %s: %s", rawURL, resp.Status)
	}
	return resp.Body, nil
}

// allowedURL accepts https URLs and http ones to a loopback host.
func allowedURL(rawURL string) error {
	u, err := url.Parse(rawURL)
	if err != nil {
		return err
	}
	if u.Scheme == "https" {
		return nil
	}
	if ip := net.ParseIP(u.Hostname()); u.Scheme == "http" && (u.Hostname() == "localhost" || ip != nil && ip.IsLoopback()) {
		return nil
	}
	return fmt.Errorf("update URL %s is not HTTPS", rawURL)
}

// CheckUpdate reads the feed's manifest for arch and returns it when it
// offers a version newer than current, or nil.
// Inputs: feed is a directory URL ending in "/"; get fetches it (HTTPGet).
func CheckUpdate(ctx context.Context, get Getter, feed, arch, current string) (*Update, error) {
	base, err := url.Parse(feed)
	if err != nil {
		return nil, err
	}
	m := base.JoinPath(UpdateManifestName(arch))
	body, err := get(ctx, m.String())
	if err != nil {
		return nil, err
	}
	defer body.Close()
	var u Update
	if err := json.NewDecoder(io.LimitReader(body, maxUpdateManifest)).Decode(&u); err != nil {
		return nil, fmt.Errorf("update manifest: %w", err)
	}
	if !plainVersion.MatchString(u.Version) || u.URL == "" || u.Signature == "" {
		return nil, errors.New("update manifest is incomplete")
	}
	if !version.Older(current, u.Version) {
		return nil, nil
	}
	u.manifest = m
	return &u, nil
}

// Download fetches the update's archive into dir, checks its size and its
// signature against key, and returns the installer it carries.
// Inputs: key is the base64 Ed25519 public key (UpdateKey); arch and current
// describe the running package.
// Returns: the path of the installer, extracted into dir.
// Side effects: writes the archive (removed again) and the installer in dir.
// Notes: refuses an archive signed with another key or changed after signing,
// and one whose signed update.json names another product, another version
// than the manifest, another architecture or a version not newer than
// current, so that a changed manifest cannot offer an older signed package
// (PLAN.md §16.2). The installer still checks architecture and downgrades
// itself.
func (u *Update) Download(ctx context.Context, get Getter, key, arch, current, dir string) (string, error) {
	pub, err := base64.StdEncoding.DecodeString(strings.TrimSpace(key))
	if err != nil || len(pub) != ed25519.PublicKeySize {
		return "", errors.New("invalid update public key")
	}
	sig, err := base64.StdEncoding.DecodeString(u.Signature)
	if err != nil {
		return "", errors.New("invalid update signature")
	}
	if u.Size <= 0 || u.Size > maxUpdateArchive {
		return "", fmt.Errorf("invalid update size %d", u.Size)
	}
	src, err := url.Parse(u.URL)
	if err != nil {
		return "", err
	}
	if u.manifest != nil {
		src = u.manifest.ResolveReference(src)
	}
	body, err := get(ctx, src.String())
	if err != nil {
		return "", err
	}
	defer body.Close()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	archive := filepath.Join(dir, UpdateArchiveName(u.Version, arch))
	defer os.Remove(archive)
	f, err := os.Create(archive)
	if err != nil {
		return "", err
	}
	sum := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, sum), io.LimitReader(body, u.Size+1))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return "", fmt.Errorf("downloading the update: %w", err)
	}
	if n != u.Size {
		return "", fmt.Errorf("downloaded %d bytes of the update, want %d", n, u.Size)
	}
	if !ed25519.Verify(pub, sum.Sum(nil), sig) {
		return "", errors.New("the update is not signed with this app's key")
	}

	z, err := zip.OpenReader(archive)
	if err != nil {
		return "", err
	}
	defer z.Close()
	var info updateInfo
	if err := readZipJSON(&z.Reader, updateInfoName, &info); err != nil {
		return "", err
	}
	switch {
	case info.Product != "Nextcloud-MyGo":
		return "", fmt.Errorf("the update is for %q", info.Product)
	case info.Version != u.Version:
		return "", fmt.Errorf("the signed update is version %s, the manifest says %s", info.Version, u.Version)
	case info.Arch != arch:
		return "", fmt.Errorf("the update is for %s, this is %s", info.Arch, arch)
	case !version.Older(current, info.Version):
		return "", fmt.Errorf("the update %s is not newer than %s", info.Version, current)
	}
	in, err := z.Open(updateSetupName)
	if err != nil {
		return "", err
	}
	defer in.Close()
	setup := filepath.Join(dir, "Nextcloud-MyGo-"+info.Version+"-"+strings.ToLower(arch)+"-setup.exe")
	out, err := os.Create(setup)
	if err != nil {
		return "", err
	}
	_, err = io.Copy(out, in)
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(setup)
		return "", err
	}
	return setup, nil
}

// readZipJSON decodes one small JSON entry of an archive.
func readZipJSON(z *zip.Reader, name string, v any) error {
	f, err := z.Open(name)
	if err != nil {
		return fmt.Errorf("update archive: %w", err)
	}
	defer f.Close()
	return json.NewDecoder(io.LimitReader(f, maxUpdateManifest)).Decode(v)
}

// SignUpdate writes the signed update of one package into dir: the archive
// UpdateArchiveName (update.json and the installer setup, stored) and its
// manifest UpdateManifestName, which names the archive relative to itself.
// Inputs: key is the private key of `mygo keygen`; version and arch are the
// package's.
// Side effects: writes the two files; the key is only used to sign.
func SignUpdate(key ed25519.PrivateKey, setup, version, arch, dir string) error {
	if !plainVersion.MatchString(version) || UpdateManifestName(arch) == "update-windows-.json" {
		return fmt.Errorf("cannot sign version %q for %q", version, arch)
	}
	info, _ := json.Marshal(updateInfo{Product: "Nextcloud-MyGo", Version: version, Arch: arch})
	in, err := os.Open(setup)
	if err != nil {
		return err
	}
	defer in.Close()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, e := range []struct {
		name string
		r    io.Reader
	}{{updateInfoName, bytes.NewReader(info)}, {updateSetupName, in}} {
		w, err := zw.CreateHeader(&zip.FileHeader{Name: e.name, Method: zip.Store, Modified: time.Now()})
		if err != nil {
			return err
		}
		if _, err := io.Copy(w, e.r); err != nil {
			return err
		}
	}
	if err := zw.Close(); err != nil {
		return err
	}
	name := UpdateArchiveName(version, arch)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dir, name), buf.Bytes(), 0o644); err != nil {
		return err
	}
	sum := sha256.Sum256(buf.Bytes())
	m, _ := json.MarshalIndent(Update{
		Version:   version,
		Notes:     "Nextcloud MyGo " + version,
		Date:      time.Now().UTC().Format(time.RFC3339),
		URL:       url.PathEscape(name),
		Size:      int64(buf.Len()),
		Signature: base64.StdEncoding.EncodeToString(ed25519.Sign(key, sum[:])),
	}, "", "  ")
	return os.WriteFile(filepath.Join(dir, UpdateManifestName(arch)), append(m, '\n'), 0o644)
}
