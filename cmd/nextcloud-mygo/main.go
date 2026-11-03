// nextcloud-mygo is the interface of Nextcloud MyGo: its window, setup,
// Settings and tray (PLAN.md §17). Until SyncD exists it runs against
// syncd.Mock, which shows sample data and says so; NEXTCLOUD_MYGO_FRESH=1
// starts it empty, as on a first run. NEXTCLOUD_MYGO_PROFILE runs a separate
// development instance with its own name and settings.
package main

import (
	"context"
	"log"
	"os"
	"path/filepath"
	"time"

	"github.com/egoist/mygo"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/gui"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/version"
	_ "github.com/YuanZhe-99/Nextcloud-MyGo/locales" // the catalogs make their languages the app's
)

func main() {
	profile := os.Getenv("NEXTCLOUD_MYGO_PROFILE")
	name := "Nextcloud MyGo"
	if profile != "" {
		name += "-" + profile
	}
	mygo.App.SetName(name)
	mygo.App.SetVersion(version.Version)
	if !mygo.App.RequestSingleInstanceLock() {
		return
	}
	base, err := os.UserConfigDir()
	if err != nil {
		log.Fatal(err)
	}
	// Separate from the official client's own folder (PLAN.md §4.5).
	dir := filepath.Join(base, "Nextcloud-MyGo"+suffix(profile))

	mock := syncd.NewMock(os.Getenv("NEXTCLOUD_MYGO_FRESH") == "", nil)
	ctx, cancel := context.WithCancel(context.Background())
	go mock.Run(ctx, 1500*time.Millisecond)

	a := gui.New(mock, dir)
	gui.ApplyTheme(a.Prefs().Theme)
	if lang := a.Prefs().Language; lang != "" {
		mygo.App.SetLocale(lang)
	}
	mygo.App.OnSecondInstance(func([]string, string) { a.Focus() })
	mygo.App.WhenReady(func() {
		// Opened at sign-in it starts in the tray, without a window, unless
		// General says otherwise.
		hidden := mygo.App.WasOpenedAtLogin() && a.Prefs().StartInTray
		a.NewTray()
		if !hidden {
			a.Focus()
		}
		// Closing the window keeps the app in the tray.
		mygo.App.OnWindowAllClosed(func() {})
		a.Start()
	})
	if err := mygo.App.Run(); err != nil {
		log.Fatal(err)
	}
	cancel()
	a.Stop()
}

func suffix(profile string) string {
	if profile == "" {
		return ""
	}
	return "-" + profile
}
