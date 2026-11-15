package main

import (
	"archive/zip"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"time"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/release"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/version"
)

// prepareLimit bounds how long setup waits for a running SyncD to reach the
// update safe point.
const prepareLimit = 2 * time.Minute

// Why setup stops before replacing anything, worded for the person running it.
var (
	errDeletionPending = errors.New("Nextcloud MyGo is waiting for you to review a deletion. Open Nextcloud MyGo, review it, then run setup again")
	errNotQuiet        = errors.New("Nextcloud MyGo is still busy (signing in, checking or switching a server address, or finishing a sync) and did not get ready within 2 minutes. Wait a few minutes, then run setup again")
	errNoAnswer        = errors.New("Nextcloud MyGo's background sync is running but did not answer within 2 minutes. Quit Nextcloud MyGo from its tray icon, then run setup again")
	errNotPrepared     = errors.New("Nextcloud MyGo could not get ready for the update. Quit Nextcloud MyGo from its tray icon, then run setup again")
)

// onStep, when set, also receives the text of each step; setup's window
// shows it under the progress bar. It is set before the work starts.
var onStep func(string)

// step prints one stage of the install or uninstall, in every mode, so the
// in-app update's log keeps it too.
func step(format string, args ...any) {
	s := fmt.Sprintf(format, args...)
	fmt.Println("- " + s)
	if onStep != nil {
		onStep(s)
	}
}

// options are setup's command line.
type options struct {
	uninstall  bool // run the uninstall (uninstall.exe -uninstall)
	removeData bool // with uninstall: also remove accounts and settings
	noLaunch   bool // do not start Nextcloud MyGo after installing
	quiet      bool // no window: progress on stdout, the error on stderr
	update     bool // started by the in-app update: no click needed, relaunch on failure
}

// task is what setup does.
type task int

const (
	taskNone      task = iota // a build without a package, run without -uninstall
	taskInstall               // install or upgrade the payload
	taskUninstall             // uninstall
)

// parseArgs reads setup's command line.
// Returns: the options, or an error naming a flag setup does not know or a
// combination it refuses (-update or -no-launch with -uninstall,
// -remove-user-data without it); usage text goes to out.
func parseArgs(args []string, out io.Writer) (options, error) {
	var o options
	fs := flag.NewFlagSet("setup", flag.ContinueOnError)
	fs.SetOutput(out)
	fs.BoolVar(&o.uninstall, "uninstall", false, "uninstall Nextcloud MyGo for this user")
	fs.BoolVar(&o.removeData, "remove-user-data", false, "with -uninstall: also remove the accounts, their saved credentials and the settings; synced files always stay")
	fs.BoolVar(&o.noLaunch, "no-launch", false, "do not start Nextcloud MyGo after installing")
	fs.BoolVar(&o.quiet, "quiet", false, "show no window: print the progress to stdout and an error to stderr")
	fs.BoolVar(&o.update, "update", false, "install as the in-app update: no confirmation, start the new version, reopen the installed one on failure")
	if err := fs.Parse(args); err != nil {
		return o, err
	}
	switch {
	case fs.NArg() > 0:
		return o, fmt.Errorf("unexpected argument %q", fs.Arg(0))
	case o.uninstall && (o.update || o.noLaunch):
		return o, errors.New("-update and -no-launch are for installing, not with -uninstall")
	case o.removeData && !o.uninstall:
		return o, errors.New("-remove-user-data needs -uninstall")
	}
	return o, nil
}

// task returns what setup does with these options; hasPayload tells whether
// this build carries a package.
func (o options) task(hasPayload bool) task {
	switch {
	case o.uninstall:
		return taskUninstall
	case hasPayload:
		return taskInstall
	}
	return taskNone
}

// prepare asks a running SyncD for the update safe point (daemon.prepareUpdate,
// PLAN.md §16.2) until it is ready.
// Inputs: ask sends the request; running reports whether that SyncD's process
// is still alive; now and sleep are the clock.
// Returns: nil once SyncD is ready, or when its process has gone and there is
// nothing left to prepare; errDeletionPending at once when a deletion waits
// for the user; errNotQuiet (still busy) or errNoAnswer (never connected) once
// prepareLimit has passed; errNotPrepared at once for any other refusal.
// Notes: installing never makes a deletion decision for the user, so that
// refusal is final; QUIESCE_STALLED (a switch, renewal, address check or
// sign-in running, or the running cycle not finished in SyncD's own bound), a
// missing connection, a configuration revision that moved meanwhile and the
// client's own request deadline are retried.
func prepare(ask func() error, running func() bool, now func() time.Time, sleep func(time.Duration)) error {
	deadline := now().Add(prepareLimit)
	for {
		err := ask()
		switch {
		case err == nil:
			return nil
		case syncd.ReasonOf(err) == "DECISION_PENDING":
			return errDeletionPending
		case !running():
			return nil
		case !errors.Is(err, syncd.ErrBusy) && !errors.Is(err, syncd.ErrStale) && !errors.Is(err, context.DeadlineExceeded):
			return errNotPrepared
		case !now().Before(deadline):
			if errors.Is(err, syncd.ErrDisconnected) {
				return errNoAnswer
			}
			return errNotQuiet
		}
		sleep(time.Second)
	}
}

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
		return fmt.Errorf("Nextcloud MyGo %s is installed for %s. Uninstall it first (your files and settings stay), then install the %s package", installed.Version, installed.Arch, next.Arch)
	case version.Older(next.Version, installed.Version):
		return fmt.Errorf("Nextcloud MyGo %s is installed; installing the older %s over it is not supported", installed.Version, next.Version)
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

// releaseLimit bounds how long the uninstall waits for SyncD to release the
// on-demand folders; a folder in the middle of a sync cycle is released once
// the cycle ends.
const releaseLimit = 5 * time.Minute

// appTag is the ApplicationName SyncD writes on its Explorer navigation
// entries (on-demand.md); the official client writes "Nextcloud".
const appTag = "Nextcloud-MyGo"

// ourTag reports whether an ApplicationName value marks an entry of
// Nextcloud-MyGo: an exact match, never the official client's "Nextcloud".
func ourTag(v string) bool { return v == appTag }

// rootAccount parses a Cloud Files sync root ID of SyncD for the user sid
// ("Nextcloud-MyGo!<sid>!<account>!<folder>", on-demand.md).
// Returns: the account ID, and ok=false for another provider's root (the
// official client's are "Nextcloud!…") or another user's.
func rootAccount(id, sid string) (account string, ok bool) {
	rest, ok := strings.CutPrefix(id, appTag+"!"+sid+"!")
	if !ok || sid == "" {
		return "", false
	}
	account, _, ok = strings.Cut(rest, "!")
	if !ok || account == "" {
		return "", false
	}
	return account, true
}

// rootPath normalizes a sync root path for comparison: cleaned, without a
// trailing separator, lower case (NTFS names compare case-insensitively).
func rootPath(p string) string {
	return strings.ToLower(strings.TrimRight(filepath.Clean(p), `\/`))
}

// releaseOnDemand makes SyncD remove every on-demand folder (folders.remove),
// which turns downloaded files into ordinary files, deletes the online-only
// placeholders (the server keeps them) and ends the folder's Windows sync
// root registration; full-file folders are left alone.
// Inputs: folders reads SyncD's current folder list; remove sends
// folders.remove and waits for its answer; now and sleep are the clock.
// Returns: nil once no on-demand folder is left; otherwise an error naming
// the local path of each folder SyncD refused or did not release within
// releaseLimit, worded for the person running the uninstall.
// Notes: a folder in a sync cycle, an address switch or a sign-in renewal
// answers QUIESCE_STALLED (ErrBusy) and is asked again each second; a lost
// connection, a moved configuration revision or a request deadline is asked
// again too. A folder already gone (NOT_FOUND) counts as released. Folders
// released before a failure stay released.
func releaseOnDemand(folders func() []syncd.Folder, remove func(id string) error, now func() time.Time, sleep func(time.Duration)) error {
	deadline := now().Add(releaseLimit)
	settled := now().Add(settleLimit)
	failed := map[string]string{} // folder ID -> local path
	released := map[string]bool{}
	waiting := false
	for {
		var pending []syncd.Folder
		for _, f := range folders() {
			if f.Mode == syncd.FolderOnDemand && failed[f.ID] == "" {
				pending = append(pending, f)
			}
		}
		if len(pending) == 0 {
			// A folder of an earlier version becomes on-demand at SyncD's
			// start; give that start a moment to decide before trusting an
			// empty list.
			if settling(folders()) && now().Before(settled) {
				sleep(time.Second)
				continue
			}
			break
		}
		late := !now().Before(deadline)
		for _, f := range pending {
			err := remove(f.ID)
			switch {
			case err == nil, errors.Is(err, syncd.ErrNotFound):
				// SyncD's folder list follows its answer shortly; one that
				// still lists the folder at the deadline did not let go.
				if late && released[f.ID] {
					failed[f.ID] = f.LocalPath
				} else if !released[f.ID] {
					released[f.ID] = true
					step("released the on-demand folder %s", f.LocalPath)
				}
			case errors.Is(err, syncd.ErrBusy), errors.Is(err, syncd.ErrStale), errors.Is(err, context.DeadlineExceeded):
				if late {
					failed[f.ID] = f.LocalPath
					step("SyncD did not release %s within %v", f.LocalPath, releaseLimit)
				} else if !waiting {
					waiting = true
					step("SyncD is still busy with a folder; waiting up to %v for it to finish", releaseLimit)
				}
			default:
				failed[f.ID] = f.LocalPath
				step("SyncD could not release %s: %v", f.LocalPath, err)
			}
		}
		sleep(time.Second)
	}
	if len(failed) == 0 {
		return nil
	}
	paths := make([]string, 0, len(failed))
	for _, p := range failed {
		paths = append(paths, p)
	}
	slices.Sort(paths)
	return fmt.Errorf("these on-demand folders could not be released from Windows, so the program was not removed:\n  %s\n"+
		"Open Nextcloud MyGo, remove each of them there (their files stay on the server), then run the uninstall again", strings.Join(paths, "\n  "))
}

// settleLimit is how long releaseOnDemand waits for SyncD to decide the
// automatic conversion of folders an earlier version added.
const settleLimit = 20 * time.Second

// settling reports a full-file folder SyncD may still turn on-demand: one
// awaiting conversion with no reason recorded yet.
func settling(folders []syncd.Folder) bool {
	return slices.ContainsFunc(folders, func(f syncd.Folder) bool {
		return f.Mode != syncd.FolderOnDemand && f.Conversion == syncd.ConversionPending && f.ConversionReason == ""
	})
}

// uninstallSequence runs the uninstall in its fixed order: release (SyncD
// gives up every on-demand folder, and with -remove-user-data the accounts),
// then the registry backstop, then removal of the program.
// Inputs: release returns kept, a failure to report once the program is
// gone (user data kept), and abort, which stops the uninstall before
// anything else is deleted.
// Returns: abort as it is; otherwise the program removal's error, or kept and
// the backstop's error joined.
// Notes: no program file goes while an on-demand folder may still be
// registered, since its online-only files could no longer be opened.
func uninstallSequence(release func() (kept, abort error), backstop, removeProgram func() error) error {
	kept, err := release()
	if err != nil {
		return err
	}
	backErr := backstop()
	if err := removeProgram(); err != nil {
		return err
	}
	return errors.Join(kept, backErr)
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
