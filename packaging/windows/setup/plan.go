package main

import (
	"archive/zip"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/release"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/version"
)

// state is install.json at the install root: the one version directory the
// shortcut, the Apps entry and the startup entry point to.
type state struct {
	Current string `json:"current"` // slash path relative to the root, "versions/<version>-<id>"
}

// readState returns the installed version's directory and manifest.
// Returns: ok=false when nothing is installed under root.
func readState(root string) (dir string, m release.Manifest, ok bool, err error) {
	data, err := os.ReadFile(filepath.Join(root, "install.json"))
	if os.IsNotExist(err) {
		return "", m, false, nil
	}
	if err != nil {
		return "", m, false, err
	}
	var s state
	if err := json.Unmarshal(data, &s); err != nil || !release.Local(s.Current) || !strings.HasPrefix(s.Current, "versions/") {
		return "", m, false, fmt.Errorf("install.json is damaged")
	}
	dir = filepath.Join(root, filepath.FromSlash(s.Current))
	data, err = os.ReadFile(filepath.Join(dir, release.ManifestName))
	if err != nil {
		return dir, m, false, err
	}
	m, err = release.Parse(data)
	return dir, m, err == nil, err
}

// writeState points install.json at dir, replacing it in one rename.
func writeState(root, dir string) error {
	rel, err := filepath.Rel(root, dir)
	if err != nil {
		return err
	}
	data, _ := json.Marshal(state{Current: filepath.ToSlash(rel)})
	tmp := filepath.Join(root, "install.json.new")
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, filepath.Join(root, "install.json"))
}

// admit decides whether next may replace the installed package.
// Returns: nil for a first install, a same-version reinstall or an upgrade;
// an error the user can act on otherwise.
// Notes: PLAN.md §16.3 — no downgrade over a newer version or a newer
// Control schema, whose data an older SyncD cannot open; no silent switch of
// architecture.
func admit(installed *release.Manifest, next release.Manifest) error {
	if installed == nil {
		return nil
	}
	switch {
	case installed.Arch != next.Arch:
		return fmt.Errorf("Nextcloud-MyGo %s is installed for %s. Uninstall it first (your files and settings stay), then install the %s package", installed.Version, installed.Arch, next.Arch)
	case version.Older(next.Version, installed.Version):
		return fmt.Errorf("Nextcloud-MyGo %s is installed; installing the older %s over it is not supported", installed.Version, next.Version)
	case next.SyncD.ControlSchema < installed.SyncD.ControlSchema:
		return fmt.Errorf("the installed version keeps its data in a newer format (%d) than this package reads (%d)", installed.SyncD.ControlSchema, next.SyncD.ControlSchema)
	}
	return nil
}

// newVersionDir names a fresh version directory under root, so that an
// install never writes into the directory a running or last bootable version uses.
func newVersionDir(root, v string) string {
	b := make([]byte, 4)
	rand.Read(b)
	return filepath.Join(root, "versions", v+"-"+hex.EncodeToString(b))
}

// extract unpacks the payload into dir and verifies it against the manifest.
// Side effects: writes dir+".partial" and renames it to dir only after
// verification; a failure removes the partial directory and leaves every
// other version untouched.
func extract(z *zip.Reader, m release.Manifest, dir string) (err error) {
	tmp := dir + ".partial"
	defer func() {
		if err != nil {
			os.RemoveAll(tmp)
		}
	}()
	for _, f := range z.File {
		if strings.HasSuffix(f.Name, "/") {
			continue
		}
		if !release.Local(f.Name) {
			return fmt.Errorf("payload: unsafe path %q", f.Name)
		}
		if err := unzip(f, filepath.Join(tmp, filepath.FromSlash(f.Name))); err != nil {
			return err
		}
	}
	if err := m.Verify(tmp); err != nil {
		return fmt.Errorf("payload: %w", err)
	}
	return os.Rename(tmp, dir)
}

// unzip writes one archive entry to dst.
func unzip(f *zip.File, dst string) error {
	r, err := f.Open()
	if err != nil {
		return err
	}
	defer r.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	w, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(w, r); err != nil {
		w.Close()
		return err
	}
	return w.Close()
}

// within reports whether path lies inside dir (case-insensitively, as
// Windows paths compare).
func within(path, dir string) bool {
	rel, err := filepath.Rel(strings.ToLower(filepath.Clean(dir)), strings.ToLower(filepath.Clean(path)))
	return err == nil && rel != "." && filepath.IsLocal(rel)
}

// startupTarget parses a Run entry's command line.
// Returns: the executable and the rest of the command line.
func startupTarget(command string) (exe, rest string) {
	if s, ok := strings.CutPrefix(command, `"`); ok {
		exe, rest, _ = strings.Cut(s, `"`)
		return exe, rest
	}
	exe, rest, _ = strings.Cut(command, " ")
	if rest != "" {
		rest = " " + rest
	}
	return exe, rest
}

// retarget rewrites a Run entry that starts an executable under root so that
// it starts exe instead, keeping its arguments.
// Returns: ok=false for entries of other applications, which stay untouched.
func retarget(command, root, exe string) (string, bool) {
	old, rest := startupTarget(command)
	if !within(old, root) {
		return command, false
	}
	return `"` + exe + `"` + rest, true
}

// otherVersions lists version directories under root other than keep,
// including partial ones that a failed install left behind.
func otherVersions(root, keep string) []string {
	entries, _ := os.ReadDir(filepath.Join(root, "versions"))
	var out []string
	for _, e := range entries {
		p := filepath.Join(root, "versions", e.Name())
		if e.IsDir() && !strings.EqualFold(p, keep) {
			out = append(out, p)
		}
	}
	return out
}
