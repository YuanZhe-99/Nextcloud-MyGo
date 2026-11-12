package gui

import (
	"context"
	"errors"
	"time"

	"github.com/egoist/mygo"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// updateState is the app's own update (PLAN.md §16.2, §17.1).
type updateState struct {
	available  *mygo.Update
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

// updateLoop checks once a day while Check for updates is on, and
// installs a found update when Install automatically says so. It runs
// only in builds that can update themselves (a signed release).
func (a *App) updateLoop() {
	if !mygo.Updater.Enabled() {
		return
	}
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

// checkUpdate reads the signed update of the latest release.
func (a *App) checkUpdate() {
	if a.upd.checking {
		return
	}
	a.upd.checking = true
	a.async(func() {
		ctx, cancel := context.WithTimeout(a.ctx, time.Minute)
		defer cancel()
		u, err := mygo.Updater.Check(ctx)
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

// installUpdate brings SyncD to a safe point first, then installs and
// relaunches; SyncD refusing (a deletion waiting for the user, or files
// moving, a switch going on) postpones it and says which (PLAN.md §16.2).
func (a *App) installUpdate() {
	u := a.upd.available
	if u == nil || a.upd.installing {
		return
	}
	a.upd.installing = true
	a.async(func() {
		err := a.client.PrepareUpdate(u.Version)
		if err == nil {
			err = u.Install(a.ctx, nil)
		}
		a.later(func() {
			a.upd.installing = false
			a.upd.postponed, a.upd.deletion = postponement(err)
			switch {
			case a.upd.postponed:
				// Not at a safe point: postponed, tried again later.
				a.setToast(waitWords(a.upd.deletion))
				return
			case err != nil:
				a.upd.failedFor = u.Version
				a.setToast(L.UpdateFailed())
				return
			}
			a.quitting = true
			mygo.App.Relaunch()
		})
	})
}
