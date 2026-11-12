// Package gui is Nextcloud MyGo's interface: the main window, first-run
// setup, Settings and the tray (PLAN.md §17, §17.1; the screens are in
// design/). It holds no sync state of its own: SyncD owns accounts,
// folders, addresses and decisions, and the interface shows the snapshot
// it reports and sends it commands (internal/syncd).
//
// Every field of App is read and written on the main thread only: views
// run there, and work done elsewhere hands its result back through later.
package gui

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"slices"
	"time"

	"github.com/egoist/mygo"
	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

type page int

// The main window's pages, in the sidebar's order.
const (
	pageOverview page = iota
	pageFolders
	pageNetwork
	pageActivity
)

// App is the interface's state.
type App struct {
	client syncd.Client
	dir    string
	ctx    context.Context
	cancel context.CancelFunc
	// later runs a function on the main thread and redraws; async runs
	// work off it. Tests run both inline.
	later func(func())
	async func(func())
	// openURL opens a sign-in page in the system browser; tests record it.
	openURL func(string)

	prefs  Prefs
	snap   syncd.Snapshot
	window *mygo.Window
	tray   *Tray
	// quitting lets the window close instead of going into the tray.
	quitting bool

	page    page
	account string // the account the Network page shows
	filter  int    // the Activity page's filter
	busy    map[string]bool
	toast   string
	toastAt time.Time

	settings settingsState
	addr     addrForm
	confirm  confirmState
	setup    setupState
	upd      updateState
	diag     diagState
	// renewing holds the browser sign-in under way for an account that
	// signs in again, by account ID.
	renewing map[string]syncd.Login
}

// Prefs are the interface's own settings (gui.json); everything about sync
// is SyncD's.
type Prefs struct {
	Theme    string `json:"theme"`    // "system", "light" or "dark"
	Language string `json:"language"` // "" follows Windows
	// StartInTray: opened at sign-in, show only the tray icon.
	StartInTray bool `json:"startInTray"`
	// CloseQuits: the close button closes the window instead of keeping
	// it in the tray; sync goes on either way.
	CloseQuits bool `json:"closeQuits"`
	// ToldTray: the first close into the tray said so.
	ToldTray bool `json:"toldTray"`

	NotifyDecisions bool `json:"notifyDecisions"`
	NotifySignIn    bool `json:"notifySignIn"`
	NotifyConflicts bool `json:"notifyConflicts"`
	NotifyUpdates   bool `json:"notifyUpdates"`

	CheckUpdates bool      `json:"checkUpdates"`
	AutoInstall  string    `json:"autoInstall"` // "never", "idle" or "daily"
	CheckedAt    time.Time `json:"checkedAt"`
}

func defaultPrefs() Prefs {
	return Prefs{Theme: "system", StartInTray: true, NotifyDecisions: true, NotifySignIn: true, NotifyConflicts: true, NotifyUpdates: true, CheckUpdates: true, AutoInstall: "never"}
}

// New makes the interface for a SyncD client; dir holds gui.json.
func New(client syncd.Client, dir string) *App {
	ctx, cancel := context.WithCancel(context.Background())
	a := &App{client: client, dir: dir, ctx: ctx, cancel: cancel, prefs: loadPrefs(dir), busy: map[string]bool{}, renewing: map[string]syncd.Login{}}
	a.later = a.onMain
	a.async = func(fn func()) { go fn() }
	a.openURL = func(url string) { _ = mygo.Shell.OpenExternal(url) }
	a.snap = client.Snapshot()
	return a
}

// loadPrefs reads gui.json over the defaults; a missing or broken file
// leaves the defaults.
func loadPrefs(dir string) Prefs {
	p := defaultPrefs()
	if b, err := os.ReadFile(filepath.Join(dir, "gui.json")); err == nil {
		_ = json.Unmarshal(b, &p)
	}
	return p
}

// setPrefs keeps p and writes gui.json (atomically: a crash leaves the old
// file).
func (a *App) setPrefs(p Prefs) {
	a.prefs = p
	if a.dir == "" {
		return
	}
	b, _ := json.MarshalIndent(p, "", "  ")
	tmp := filepath.Join(a.dir, "gui.json.tmp")
	if os.MkdirAll(a.dir, 0o700) == nil && os.WriteFile(tmp, b, 0o600) == nil {
		_ = os.Rename(tmp, filepath.Join(a.dir, "gui.json"))
	}
}

// Prefs returns the interface's settings (main thread).
func (a *App) Prefs() Prefs { return a.prefs }

// onMain runs fn on the main thread, redrawing the main window and the
// tray after it.
func (a *App) onMain(fn func()) {
	mygo.RunOnMain(func() {
		fn()
		if a.window != nil {
			a.window.Invalidate()
		}
		a.tray.refresh()
	})
}

// Start follows SyncD's snapshot and begins the update checks.
// Side effects: a subscription for the app's life; goroutines that end
// with Stop.
func (a *App) Start() {
	a.client.Subscribe(func() {
		s := a.client.Snapshot()
		a.later(func() { a.setSnapshot(s) })
	})
	a.setSnapshot(a.client.Snapshot())
	go a.updateLoop()
}

// Stop ends the background work.
func (a *App) Stop() { a.cancel() }

// setSnapshot shows a new snapshot, notifying what newly needs the user.
func (a *App) setSnapshot(s syncd.Snapshot) {
	old := a.snap
	a.snap = s
	a.notifyNew(old, s)
	if _, ok := s.Account(a.account); !ok && len(s.Accounts) > 0 {
		a.account = s.Accounts[0].ID
	}
}

// notifyNew shows a Windows notification for each decision and sign-in
// that was not there before, as Notifications allows. Notifications name
// a folder or an account, never a path or a code.
func (a *App) notifyNew(old, s syncd.Snapshot) {
	if !stateKnown(old) || !stateKnown(s) {
		return // the first known snapshot: nothing is new
	}
	for _, d := range s.Decisions {
		if slices.ContainsFunc(old.Decisions, func(o syncd.Decision) bool { return o.ID == d.ID }) {
			continue
		}
		f, _ := s.Folder(d.FolderID)
		switch {
		case d.Kind == syncd.DecisionBulkDeletions && a.prefs.NotifyDecisions:
			a.notify(d.ID, L.ReviewTitle(d.Count), L.AttnDeletionsBody(f.Name))
		case d.Kind == syncd.DecisionConflict && a.prefs.NotifyConflicts:
			a.notify(d.ID, L.AttnConflictTitle(d.File), L.AttnConflictBody(d.Copy, f.Name))
		}
	}
	for _, acc := range s.Accounts {
		was, ok := old.Account(acc.ID)
		if a.prefs.NotifySignIn && needsSignIn(s, acc) && (!ok || !needsSignIn(old, was)) {
			a.notify("signin-"+acc.ID, L.AttnSignInTitle(accountName(acc)), L.AttnSignInBody())
		}
	}
}

// notify shows a Windows notification off the main thread.
func (a *App) notify(id, title, body string) {
	if a.window == nil && a.tray == nil {
		return // tests
	}
	a.async(func() { _ = mygo.NewNotification(mygo.NotificationOptions{ID: id, Title: title, Body: body}).Show() })
}

// do runs a SyncD command off the main thread, keeping key busy meanwhile,
// and shows a failure as a toast.
func (a *App) do(key string, cmd func() error, done func(error)) {
	if a.busy[key] {
		return
	}
	a.busy[key] = true
	a.async(func() {
		err := cmd()
		a.later(func() {
			delete(a.busy, key)
			if done != nil {
				done(err)
				return
			}
			if err != nil {
				a.setToast(errText(err))
			}
		})
	})
}

// errText puts a SyncD error into words: its reason code when SyncD gave
// one, never the code itself.
func errText(err error) string {
	switch {
	case errors.Is(err, syncd.ErrRevocationUnconfirmed):
		return L.AccountRemovedRevocationUnconfirmed()
	case errors.Is(err, syncd.ErrCredentialCleanup):
		return L.CredentialCleanupUnconfirmed()
	}
	if t := reasonText(syncd.ReasonOf(err)); t != "" {
		return t
	}
	switch {
	case errors.Is(err, syncd.ErrStateUnavailable):
		return L.ReasonStateUnavailable()
	case errors.Is(err, syncd.ErrStale), errors.Is(err, syncd.ErrNotFound):
		return L.ToastStale()
	case errors.Is(err, syncd.ErrBusy):
		// Without a reason: no connection to SyncD to send it on.
		return L.ToastNotConnected()
	case errors.Is(err, context.DeadlineExceeded):
		// Accepted is not done, and no answer is not a failure (INV-12).
		return L.ToastNoAnswer()
	case errors.Is(err, syncd.ErrRefused):
		return L.ToastRefused()
	}
	return L.ToastFailed()
}

// setToast shows a short message at the foot of the window for a few
// seconds.
func (a *App) setToast(msg string) {
	a.toast, a.toastAt = msg, time.Now()
	at := a.toastAt
	a.async(func() {
		time.Sleep(4 * time.Second)
		a.later(func() {
			if a.toastAt == at {
				a.toast = ""
			}
		})
	})
}

// mainWindow returns the main window, made first when there is none: it
// exists only while shown, as in Kyokai (closing it into the tray ends it).
func (a *App) mainWindow() *mygo.Window {
	if a.window != nil {
		return a.window
	}
	w := mygo.NewWindow(mygo.WindowOptions{
		Title: "Nextcloud MyGo", Width: 1200, Height: 780, MinWidth: 1000, MinHeight: 660,
		TitleBarStyle: mygo.TitleBarHidden, HideWindowButtons: true,
		StateKey: "main", Hidden: true,
		Content: ui.View(a.view),
	})
	w.OnClose(a.handleClose)
	a.window = w
	return w
}

// Focus shows and raises the main window. Safe from any goroutine.
func (a *App) Focus() {
	mygo.RunOnMain(func() {
		w := a.mainWindow()
		w.Show()
		w.Focus()
	})
}

// handleClose keeps sync going whatever the close button does: into the
// tray (the default, told the first time), or the window closes and the
// interface quits while SyncD goes on.
func (a *App) handleClose(e *mygo.CloseEvent) {
	if a.quitting {
		return
	}
	if a.prefs.CloseQuits || a.tray == nil {
		a.quitting = true
		mygo.App.Quit()
		return
	}
	a.window = nil
	if !a.prefs.ToldTray {
		p := a.prefs
		p.ToldTray = true
		a.setPrefs(p)
		a.notify("tray", "Nextcloud MyGo", L.FirstClose())
	}
}

// QuitWindow closes the interface; SyncD keeps syncing.
func (a *App) QuitWindow() {
	a.quitting = true
	mygo.App.Quit()
}

// QuitAndStop stops background sync, then the interface; the only
// command that stops SyncD (PLAN.md §17 item 10). If SyncD does not stop,
// the interface stays and says so.
func (a *App) QuitAndStop() {
	a.do("shutdown", a.client.Shutdown, func(err error) {
		if err != nil {
			a.setToast(L.StopFailed())
			a.notify("shutdown", "Nextcloud MyGo", L.StopFailed())
			return
		}
		a.quitting = true
		mygo.App.Quit()
	})
}

// openPath opens a folder of this PC in Explorer.
func (a *App) openPath(path string) {
	a.async(func() {
		if err := mygo.Shell.OpenPath(path); err != nil {
			a.later(func() { a.setToast(L.ToastFailed()) })
		}
	})
}

// openBrowser opens a sign-in page; sample data opens nothing.
func (a *App) openBrowser(url string) {
	if a.snap.Daemon.Demo {
		return
	}
	open := a.openURL
	a.async(func() { open(url) })
}

// ApplyTheme sets the appearance: "system", "light" or "dark".
func ApplyTheme(theme string) {
	switch theme {
	case "light":
		mygo.Theme.SetSource(mygo.ThemeLight)
	case "dark":
		mygo.Theme.SetSource(mygo.ThemeDark)
	default:
		mygo.Theme.SetSource(mygo.ThemeSystem)
	}
}
