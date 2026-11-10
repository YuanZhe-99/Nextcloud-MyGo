// nextcloud-mygo connects the native interface to its paired SyncD executable.
// NEXTCLOUD_MYGO_MOCK=1 selects an explicit demo; NEXTCLOUD_MYGO_FRESH=1 empties it.
// NEXTCLOUD_MYGO_PROFILE selects an independent development profile.
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

// main starts the native UI and its client transport without coupling window closure to sync.
func main() {
	profile := os.Getenv("NEXTCLOUD_MYGO_PROFILE")
	name := "Nextcloud-MyGo"
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

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var client syncd.Client
	if os.Getenv("NEXTCLOUD_MYGO_MOCK") == "1" {
		mock := syncd.NewMock(os.Getenv("NEXTCLOUD_MYGO_FRESH") == "", nil)
		go mock.Run(ctx, 1500*time.Millisecond)
		client = mock
	} else {
		executable, err := os.Executable()
		if err != nil {
			log.Fatal(err)
		}
		remote := syncd.NewRemote(ctx, profile, filepath.Join(filepath.Dir(executable), "nextcloud-mygo-syncd.exe"))
		defer remote.Close()
		client = remote
	}

	a := gui.New(client, dir)
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

// suffix keeps the UI preferences of explicit development profiles separate.
func suffix(profile string) string {
	if profile == "" {
		return ""
	}
	return "-" + profile
}
