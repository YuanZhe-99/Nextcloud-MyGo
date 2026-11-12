//go:build windows

// Command setup installs, upgrades and uninstalls Nextcloud-MyGo for the
// current user, without elevation. Built with the payload tag it carries one
// package (tools/package.ps1) and installs it; built without, it is the
// uninstall.exe that package ships.
//
//	Nextcloud-MyGo-<version>-<arch>-test-setup.exe [-no-launch]
//	uninstall.exe -uninstall [-remove-user-data]
//
// See doc/en-us/packaging.md for the layout and the upgrade sequence.
package main

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/release"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
)

// Executable names inside a version directory.
const (
	uiExe    = "Nextcloud-MyGo.exe"
	syncdExe = "nextcloud-mygo-syncd.exe"
)

// main parses the flags and runs the install or the uninstall.
func main() {
	uninstall := flag.Bool("uninstall", false, "uninstall Nextcloud-MyGo for this user")
	removeData := flag.Bool("remove-user-data", false, "with -uninstall: also remove the accounts, their saved credentials and the settings; synced files always stay")
	noLaunch := flag.Bool("no-launch", false, "do not start Nextcloud-MyGo after installing")
	flag.Parse()
	programs, err := programsDir()
	if err == nil {
		// Work from the programs folder so that shutdown's relative path to
		// SyncD exists (same volume).
		err = os.Chdir(programs)
	}
	if err == nil {
		root := filepath.Join(programs, "Nextcloud-MyGo")
		// NEXTCLOUD_MYGO_PROFILE is the interface's development profile; the
		// installed app runs the default one.
		profile := os.Getenv("NEXTCLOUD_MYGO_PROFILE")
		switch {
		case *uninstall:
			err = uninstallApp(root, profile, *removeData)
		case payload != nil:
			err = install(root, profile, !*noLaunch)
		default:
			err = errors.New("this build carries no package; run it with -uninstall")
		}
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "Nextcloud-MyGo setup:", err)
		os.Exit(1)
	}
}

// step prints one stage of the install or uninstall.
func step(format string, args ...any) { fmt.Printf("- "+format+"\n", args...) }

// install puts the payload in a fresh version directory, stops the running
// version, then moves the shortcut, the Apps entry and the startup entry over.
// Side effects: writes under root, the Start menu and HKCU; may stop and
// start Nextcloud-MyGo.
// Notes: the previous version directory is removed only after the new one is
// complete, verified and current (PLAN.md §16.2): a failure at any earlier
// point leaves the last bootable version as it was.
func install(root, profile string, start bool) error {
	z, err := zip.NewReader(bytes.NewReader(payload), int64(len(payload)))
	if err != nil {
		return fmt.Errorf("payload: %w", err)
	}
	mf, err := z.Open(release.ManifestName)
	if err != nil {
		return fmt.Errorf("payload: %w", err)
	}
	var buf bytes.Buffer
	_, err = buf.ReadFrom(mf)
	mf.Close()
	if err != nil {
		return err
	}
	m, err := release.Parse(buf.Bytes())
	if err != nil {
		return err
	}
	fmt.Printf("Nextcloud-MyGo %s for %s: TEST BUILD, unsigned, automatic updates off\n", m.Version, m.Arch)

	native, err := nativeMachine()
	if err != nil {
		return err
	}
	if want := map[string]uint16{"ARM64": 0xaa64, "x64": 0x8664}[m.Arch]; native != want {
		return fmt.Errorf("this is the %s package, but Windows here runs on another architecture (machine %#04x); use the matching package", m.Arch, native)
	}
	oldDir, installed, ok, err := readState(root)
	if err != nil {
		return fmt.Errorf("existing install: %w", err)
	}
	var cur *release.Manifest
	if ok {
		cur = &installed
		step("installed: %s (%s)", installed.Version, oldDir)
	}
	if err := admit(cur, m); err != nil {
		return err
	}

	dir := newVersionDir(root, m.Version)
	step("extracting to %s", dir)
	if err := extract(z, m, dir); err != nil {
		return err
	}
	if err := stopApp(root, profile); err != nil {
		os.RemoveAll(dir)
		return fmt.Errorf("%w; the installed version was kept", err)
	}

	step("switching to %s", filepath.Base(dir))
	exe := filepath.Join(dir, uiExe)
	if err := copyUninstaller(dir, root); err != nil {
		return err
	}
	if err := writeState(root, dir); err != nil {
		return err
	}
	if err := writeShortcut(exe); err != nil {
		return fmt.Errorf("Start menu shortcut: %w", err)
	}
	var size int64
	for _, f := range m.Files {
		size += f.Size
	}
	if err := writeAppsEntry(root, exe, m.Version, m.Arch, uint32(size/1024)); err != nil {
		return fmt.Errorf("Apps entry: %w", err)
	}
	if err := startupEntries(root, func(c string) string { s, _ := retarget(c, root, exe); return s }); err != nil {
		return fmt.Errorf("startup entry: %w", err)
	}
	for _, old := range otherVersions(root, dir) {
		if err := removeAll(old); err != nil {
			step("could not remove %s yet (%v); the next install or the uninstall removes it", old, err)
		} else {
			step("removed %s", filepath.Base(old))
		}
	}
	step("installed %s %s in %s", m.Version, m.Arch, root)
	if start {
		return launch(exe)
	}
	return nil
}

// copyUninstaller places the version's uninstall.exe at the install root,
// where the Apps entry finds it whichever version is current.
func copyUninstaller(dir, root string) error {
	data, err := os.ReadFile(filepath.Join(dir, "uninstall.exe"))
	if err != nil {
		return err
	}
	tmp := filepath.Join(root, "uninstall.exe.new")
	if err := os.WriteFile(tmp, data, 0o755); err != nil {
		return err
	}
	return os.Rename(tmp, filepath.Join(root, "uninstall.exe"))
}

// stopApp stops the interface and SyncD processes running from root.
// Returns: an error only when a process outlives both the request and termination.
// Notes: the interface goes first, so that it cannot start SyncD again. SyncD
// is asked to stop through daemon.shutdown and gets 30 seconds; one that does
// not stop is terminated, which SyncD recovers from like any crash.
// daemon.prepareUpdate, the safe-point request of PLAN.md §16.2, replaces the
// shutdown here once SyncD implements it (doc/en-us/packaging.md).
func stopApp(root, profile string) error {
	ps, err := processesUnder(root)
	if err != nil {
		return err
	}
	var ui, daemons []process
	for _, p := range ps {
		switch {
		case equalBase(p.path, uiExe):
			ui = append(ui, p)
		case equalBase(p.path, syncdExe):
			daemons = append(daemons, p)
		}
	}
	if len(ui) > 0 {
		step("closing the interface (%d)", len(ui))
		if left := terminate(ui); len(left) > 0 {
			return fmt.Errorf("the interface did not close")
		}
	}
	if len(daemons) == 0 {
		return nil
	}
	step("asking SyncD to stop (%d)", len(daemons))
	seen := map[string]bool{}
	for _, p := range daemons {
		if !seen[p.path] {
			seen[p.path] = true
			if err := shutdown(profile, p.path, daemons); err != nil {
				step("SyncD did not accept the stop request: %v", err)
			}
		}
	}
	left := waitExit(daemons, 30*time.Second)
	if len(left) > 0 {
		step("SyncD did not stop in time; ending it (%d)", len(left))
		if left = terminate(left); len(left) > 0 {
			return fmt.Errorf("SyncD did not stop")
		}
	}
	return nil
}

// shutdown sends daemon.shutdown to the profile's SyncD over the
// authenticated pipe, which only a SyncD started from executable answers.
// Notes: syncd.Remote starts SyncD when no pipe exists, and SyncD may exit
// between the process scan and the dial. The client therefore gets the
// executable as a path relative to the working directory: the pipe check
// resolves it to the same file, while the launcher refuses relative paths,
// so this call can never start SyncD (doc/en-us/packaging.md asks for a
// connect-only client instead). It returns once the request completes or
// every process in ps has exited, whichever comes first.
func shutdown(profile, executable string, ps []process) error {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	wd, err := os.Getwd()
	if err != nil {
		return err
	}
	rel, err := filepath.Rel(wd, executable)
	if err != nil || filepath.IsAbs(rel) {
		return fmt.Errorf("no relative path from %s to SyncD", wd)
	}
	r := syncd.NewRemote(ctx, profile, rel)
	defer r.Close()
	for {
		if len(waitExit(ps, 0)) == 0 {
			return nil
		}
		err := r.Shutdown()
		if !errors.Is(err, syncd.ErrBusy) {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(200 * time.Millisecond):
		}
	}
}

// uninstallApp stops Nextcloud-MyGo, removes its startup entry, shortcut,
// Apps entry and program files, and with removeData its accounts and settings.
// Side effects: deletes the install root (the last part once this process
// exits); never touches synced folders or server content.
func uninstallApp(root, profile string, removeData bool) error {
	dir, _, ok, err := readState(root)
	if err != nil {
		step("install state unreadable (%v); removing what is there", err)
	}
	if err := stopApp(root, profile); err != nil {
		return err
	}
	var dataErr error
	if removeData {
		if !ok {
			dataErr = errors.New("no installed SyncD to remove the accounts with; user data kept")
		} else {
			dataErr = removeUserData(root, profile, filepath.Join(dir, syncdExe))
		}
	} else {
		step("keeping accounts, settings and synced files (use -remove-user-data to remove accounts and settings)")
	}
	step("removing the startup entry, shortcut and Apps entry")
	if err := startupEntries(root, func(string) string { return "" }); err != nil {
		return err
	}
	if link, err := shortcutPath(); err == nil {
		if err := os.Remove(link); err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	if err := deleteAppsEntry(); err != nil {
		return err
	}
	step("removing %s", root)
	for _, p := range []string{filepath.Join(root, "versions"), filepath.Join(root, "install.json")} {
		if err := removeAll(p); err != nil {
			return err
		}
	}
	self, _ := os.Executable()
	if within(self, root) {
		if err := removeLater(self, root); err != nil {
			return err
		}
	} else if err := os.RemoveAll(root); err != nil {
		return err
	}
	if dataErr != nil {
		return dataErr
	}
	step("uninstalled")
	return nil
}

// removeUserData removes every account of the profile through SyncD, which
// deletes their saved credentials and revokes the app passwords on the
// server, then deletes the profile's SyncD data and interface settings.
// Notes: an account SyncD cannot remove keeps the data (and with it the
// credential reference) in place and is reported; an unconfirmed server
// revocation is reported but does not block the uninstall (PLAN.md §16.4).
func removeUserData(root, profile, executable string) error {
	step("removing accounts through SyncD")
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	r := syncd.NewRemote(ctx, profile, executable)
	for !r.Snapshot().Daemon.Connected {
		select {
		case <-ctx.Done():
			r.Close()
			return errors.New("SyncD did not start; accounts and settings were kept")
		case <-time.After(200 * time.Millisecond):
		}
	}
	failed := 0
	for _, a := range r.Snapshot().Accounts {
		switch err := r.RemoveAccount(a.ID); {
		case errors.Is(err, syncd.ErrRevocationUnconfirmed):
			step("removed an account; the server did not confirm revoking its app password, which can be removed in the server's security settings")
		case err != nil:
			failed++
			step("could not remove an account: %v", err)
		default:
			step("removed an account and revoked its app password")
		}
	}
	err := r.Shutdown()
	r.Close()
	if err != nil {
		step("SyncD stop request: %v", err)
	}
	if err := stopApp(root, profile); err != nil {
		return err
	}
	if failed > 0 {
		return fmt.Errorf("%d account(s) could not be removed; accounts and settings were kept", failed)
	}
	local, err := localAppData()
	if err != nil {
		return err
	}
	sum := sha256.Sum256([]byte(profile))
	data := filepath.Join(local, "Nextcloud-MyGo", "syncd", hex.EncodeToString(sum[:])[:24])
	config, err := os.UserConfigDir()
	if err != nil {
		return err
	}
	settings := filepath.Join(config, "Nextcloud-MyGo")
	if profile != "" {
		settings += "-" + profile
	}
	for _, p := range []string{data, settings} {
		if err := removeAll(p); err != nil {
			return err
		}
		step("removed %s", p)
	}
	// Remove the parents only when no other profile is left in them.
	os.Remove(filepath.Join(local, "Nextcloud-MyGo", "syncd"))
	os.Remove(filepath.Join(local, "Nextcloud-MyGo"))
	return nil
}

// removeAll removes a path, retrying for five seconds while Windows still
// holds files of a process that has just exited.
func removeAll(p string) error {
	var err error
	for range 10 {
		if err = os.RemoveAll(p); err == nil {
			return nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return err
}

// equalBase reports whether a path names the given file, ignoring case.
func equalBase(path, name string) bool {
	return strings.EqualFold(filepath.Base(path), name)
}
