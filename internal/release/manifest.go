// Package release describes one packaged UI + SyncD combination: the manifest
// tools/pkg writes into a package and the installer reads back.
package release

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"strings"
)

// ManifestName is the manifest's file name at the root of a package.
const ManifestName = "manifest.json"

// Manifest records what a package contains and which contracts its halves speak.
// Notes: TestBuild, Signed and AutoUpdate state the release posture plainly
// (PLAN.md §16.2): until a trusted signing chain exists every package is an
// unsigned test build with automatic updates off.
type Manifest struct {
	Product    string `json:"product"`
	Version    string `json:"version"`
	Arch       string `json:"arch"` // "ARM64" or "x64"
	TestBuild  bool   `json:"testBuild"`
	Signed     bool   `json:"signed"`
	AutoUpdate bool   `json:"autoUpdate"`
	UI         Half   `json:"ui"`
	SyncD      Half   `json:"syncd"`
	Files      []File `json:"files"`
}

// Half names one process of the combination and the contracts it speaks.
// Notes: ControlSchema is the Control DB user_version SyncD migrates to; it
// is zero for the UI, which never opens the Control DB.
type Half struct {
	Executable    string   `json:"executable"`
	Protocol      Protocol `json:"protocol"`
	ControlSchema int      `json:"controlSchema,omitempty"`
}

// Protocol is the IPC protocol version a half speaks (protocol/contract.json).
type Protocol struct {
	Major int `json:"major"`
	Minor int `json:"minor"`
}

// File is one packaged file, by slash-separated path relative to the package root.
type File struct {
	Path   string `json:"path"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

// Parse decodes and validates a manifest read from a package or an installer.
// Returns: an error for unknown fields, a missing half or an unsafe file path.
func Parse(data []byte) (Manifest, error) {
	var m Manifest
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err := d.Decode(&m); err != nil {
		return m, fmt.Errorf("manifest: %w", err)
	}
	if m.Product != "Nextcloud-MyGo" || m.Version == "" || (m.Arch != "ARM64" && m.Arch != "x64") {
		return m, fmt.Errorf("manifest: not a Nextcloud-MyGo ARM64/x64 package")
	}
	if m.UI.Executable == "" || m.SyncD.Executable == "" || m.SyncD.ControlSchema <= 0 {
		return m, fmt.Errorf("manifest: missing UI or SyncD")
	}
	for _, f := range m.Files {
		if !Local(f.Path) {
			return m, fmt.Errorf("manifest: unsafe path %q", f.Path)
		}
	}
	return m, nil
}

// Local reports whether p is a clean, relative, slash-separated path that
// stays inside the package root.
func Local(p string) bool {
	return p != "" && p == path.Clean(p) && !strings.Contains(p, `\`) && filepath.IsLocal(filepath.FromSlash(p))
}

// Hash returns the size and hex SHA-256 of a file.
func Hash(name string) (int64, string, error) {
	f, err := os.Open(name)
	if err != nil {
		return 0, "", err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	return n, hex.EncodeToString(h.Sum(nil)), err
}

// Verify checks that dir holds exactly the manifest's files with their sizes
// and hashes, apart from the manifest itself.
// Returns: the first difference found.
func (m Manifest) Verify(dir string) error {
	want := map[string]File{}
	for _, f := range m.Files {
		want[f.Path] = f
	}
	err := filepath.WalkDir(dir, func(p string, e os.DirEntry, err error) error {
		if err != nil || e.IsDir() {
			return err
		}
		rel, err := filepath.Rel(dir, p)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		if rel == ManifestName {
			return nil
		}
		f, ok := want[rel]
		if !ok {
			return fmt.Errorf("%s is not in the manifest", rel)
		}
		delete(want, rel)
		size, sum, err := Hash(p)
		if err != nil {
			return err
		}
		if size != f.Size || sum != f.SHA256 {
			return fmt.Errorf("%s does not match the manifest", rel)
		}
		return nil
	})
	if err != nil {
		return err
	}
	for p := range want {
		return fmt.Errorf("%s is missing", p)
	}
	return nil
}
