package gui

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/egoist/mygo"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/release"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/version"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// updateState is the app's own update (PLAN.md §16.2, §17.1).
type updateState struct {
	available  *release.Update
	checking   bool
	installing bool
	// failedFor is the version an automatic install failed for in this
	// run; it is not tried again until the next start.
	failedFor string
	// postponed: the last install waited for a safe point; deletion says
	// it waits for a deletion the user reviews (DECISION_PENDING) rather
	// than for sync to settle (QUIESCE_STALLED).
	postponed, deletion bool
}

// postponement tells whether PrepareUpdate's answer postpones the update
// (ErrBusy), and whether for a deletion waiting for the user
// (DECISION_PENDING) rather than for sync to settle (QUIESCE_STALLED).
func postponement(err error) (postponed, deletion bool) {
	return errors.Is(err, syncd.ErrBusy), syncd.ReasonOf(err) == "DECISION_PENDING"
}

// waitWords says why SyncD postponed an update: a deletion waits for the
// user, who can end it, or sync has not reached a safe point yet, which
// ends on its own.
func waitWords(deletion bool) string {
	if deletion {
		return L.UpdateWaitsDeletion()
	}
	return L.UpdateWaits()
}

// installRoot returns the install folder the running interface was
// installed in (%LOCALAPPDATA%\Programs\Nextcloud-MyGo, the parent of
// versions\<version>), or "" when it does not run from an install.
var installRoot = sync.OnceValue(func() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	versions := filepath.Dir(filepath.Dir(exe))
	root := filepath.Dir(versions)
	if !strings.EqualFold(filepath.Base(versions), "versions") {
		return ""
	}
	if _, err := os.Stat(filepath.Join(root, "install.json")); err != nil {
		return ""
	}
	return root
})

// updatesEnabled reports whether this build offers updates: a package built
// with the update signing key (release.UpdateFeed and UpdateKey), running
// from its install.
var updatesEnabled = sync.OnceValue(func() bool {
	return release.UpdateFeed != "" && release.UpdateKey != "" && installRoot() != ""
})

// packageArch is the package architecture of this build.
func packageArch() string {
	if runtime.GOARCH == "arm64" {
		return "ARM64"
	}
	return "x64"
}

// stagingDir is where updates are downloaded: next to the versions, inside
// the install folder, so that the uninstall removes it with the rest.
func stagingDir(root string) string { return filepath.Join(root, "update") }

// updateLoop checks once a day while Check for updates is on, and
// installs a found update when Install automatically says so. It runs
// only in builds that can update themselves (a signed release).
// Side effects: removes what an earlier update left in the staging folder.
func (a *App) updateLoop() {
	if !updatesEnabled() {
		return
	}
	os.RemoveAll(stagingDir(installRoot()))
	t := time.NewTicker(5 * time.Minute)
	defer t.Stop()
	for {
		a.later(func() {
			if a.prefs.CheckUpdates && time.Since(a.prefs.CheckedAt) > 24*time.Hour {
				a.checkUpdate()
			}
			a.autoInstall()
		})
		select {
		case <-a.ctx.Done():
			return
		case <-t.C:
		}
	}
}

// checkUpdate reads the update manifest of the latest release.
func (a *App) checkUpdate() {
	if a.upd.checking || !updatesEnabled() {
		return
	}
	a.upd.checking = true
	a.async(func() {
		ctx, cancel := context.WithTimeout(a.ctx, time.Minute)
		defer cancel()
		u, err := release.CheckUpdate(ctx, release.HTTPGet, release.UpdateFeed, packageArch(), version.Version)
		a.later(func() {
			a.upd.checking = false
			if err != nil {
				return
			}
			pr := a.prefs
			pr.CheckedAt = time.Now()
			a.setPrefs(pr)
			if u != nil && a.upd.available == nil && a.prefs.NotifyUpdates {
				a.notify("update", L.UpdateAvailable(u.Version), L.UpdateAvailableHint())
			}
			a.upd.available = u
		})
	})
}

// autoDue tells whether an automatic install may start now: the mode
// chosen, the computer idle for 15 minutes or within the chosen hour.
func autoDue(mode string, idle time.Duration, now time.Time) bool {
	switch mode {
	case "idle":
		return idle >= 15*time.Minute
	case "daily":
		return now.Hour() == 3
	}
	return false
}

// autoInstall installs a found update without asking when it is due.
func (a *App) autoInstall() {
	u := a.upd.available
	if u == nil || a.upd.installing || a.upd.failedFor == u.Version || !autoDue(a.prefs.AutoInstall, idleTime(), time.Now()) {
		return
	}
	a.installUpdate()
}

// installUpdate downloads the update and checks its signature, brings
// SyncD to a safe point, then hands over to the update's installer, which
// closes this interface, prepares and stops SyncD, switches to a new
// version directory and starts the new version (PLAN.md §16.2). SyncD
// refusing (a deletion waiting for the user, or files moving, a switch
// going on) postpones it and says which. Any failure keeps the installed
// version: nothing of it is touched before setup has the new one complete.
func (a *App) installUpdate() {
	u := a.upd.available
	root := installRoot()
	if u == nil || a.upd.installing || root == "" {
		return
	}
	a.upd.installing = true
	a.async(func() {
		dir := stagingDir(root)
		os.RemoveAll(dir)
		ctx, cancel := context.WithTimeout(a.ctx, 30*time.Minute)
		setup, err := u.Download(ctx, release.HTTPGet, release.UpdateKey, packageArch(), version.Version, dir)
		cancel()
		if err == nil {
			err = a.client.PrepareUpdate(u.Version)
		}
		if err == nil {
			var c *exec.Cmd
			if c, err = release.StartSetup(setup, filepath.Join(dir, "setup.log")); err == nil {
				// Setup closes this interface on its way; it returns while
				// the interface still runs only when it refused early.
				if err = c.Wait(); err == nil {
					err = errSetupDone
				}
			}
		}
		a.later(func() {
			a.upd.installing = false
			a.upd.postponed, a.upd.deletion = postponement(err)
			switch {
			case a.upd.postponed:
				// Not at a safe point: postponed, tried again later.
				a.setToast(waitWords(a.upd.deletion))
				return
			case errors.Is(err, errSetupDone):
				// Installed without closing this interface; the new one runs.
				a.quitting = true
				mygo.App.Quit()
			case err != nil:
				a.upd.failedFor = u.Version
				a.setToast(L.UpdateFailed())
			}
		})
	})
}

// errSetupDone: setup finished while this interface still ran.
var errSetupDone = errors.New("setup finished")
