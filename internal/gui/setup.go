package gui

import (
	"errors"
	"os"
	"path/filepath"
	"strings"

	"github.com/egoist/mygo"
	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// The setup's steps.
const (
	stepServer = iota + 1
	stepSignIn
	stepFolders
	stepStart
)

// setupState is first-run setup, or adding an account or folders later:
// nothing syncs until Start syncing (PLAN.md §17.1).
type setupState struct {
	active bool
	step   int
	// addOnly: folders for an account that exists; setup ends after
	// step 3.
	addOnly   bool
	server    string
	login     syncd.Login
	accountID string
	remote    []syncd.RemoteFolder
	chosen    map[string]bool
	base      string
	atLogin   bool
	notify    bool
	failed    string
	// remoteErr says why the server's folders could not be listed, or "".
	remoteErr string
}

// setupShown tells whether the window shows setup instead of the pages:
// while it runs, and always while SyncD reports no account (an unknown
// list is not an empty one).
func (a *App) setupShown() bool {
	return a.setup.active || (stateKnown(a.snap) && len(a.snap.Accounts) == 0)
}

// startSetup begins setup at its first step.
func (a *App) startSetup() {
	home, _ := os.UserHomeDir()
	a.setup = setupState{active: true, step: stepServer, atLogin: true, notify: true, base: filepath.Join(home, "Nextcloud"), chosen: map[string]bool{}}
}

// startAddFolders opens setup at its folders step for an account.
func (a *App) startAddFolders(accountID string) {
	a.startSetup()
	a.setup.step, a.setup.addOnly, a.setup.accountID = stepFolders, true, accountID
	a.loadRemote(accountID)
}

// setupView is setup in the main window: the steps beside the step shown
// (design/SetupWelcome.html … SetupFinish.html).
func (a *App) setupView(c *ui.Context) {
	if !a.setup.active {
		a.startSetup()
	}
	p := pal(c)
	st := &a.setup
	ui.Row(c).Grow(1).FillWidth().AlignItems(ui.Stretch).Children(func() {
		ui.Column(c).Width(300).Shrink(0).Padding(32, 28).Gap(28).Children(func() {
			mark(c, 64)
			ui.Column(c).Gap(14).Label(L.SetupStepServer()).Children(func() {
				for i, name := range []string{L.SetupStepServer(), L.SetupStepSignIn(), L.SetupStepFolders(), L.SetupStepStart()} {
					setupStep(c, i+1, st.step, name)
				}
			})
			ui.Spacer(c)
			if st.step == stepServer {
				ui.Column(c).Gap(6).Children(func() {
					muted(c, L.LanguageLabel(), fontSmall)
					a.languageSelect(c, "setup-lang")
				})
			}
			if st.step >= stepFolders {
				if acc, ok := a.snap.Account(st.accountID); ok {
					ui.Row(c).Gap(10).Children(func() {
						avatar(c, acc, 32)
						ui.Column(c).Children(func() {
							ui.Text(c, L.SignedInAs(acc.DisplayName)).FontSize(13).FontWeight(600)
							muted(c, accountName(acc), fontSmall)
						})
					})
				}
			}
		})
		ui.Column(c).Grow(1).MinWidth(0).Background(p.pane).Padding(48, 56, 32, 56).Gap(20).Children(func() {
			switch st.step {
			case stepSignIn:
				a.setupSignIn(c)
			case stepFolders:
				a.setupFolders(c)
			case stepStart:
				a.setupStart(c)
			default:
				a.setupServer(c)
			}
		})
	})
}

// setupStep is a step in the list: done, current or to come.
func setupStep(c *ui.Context, n, current int, name string) {
	p := pal(c)
	ui.Row(c).Gap(12).Children(func() {
		switch {
		case n < current:
			ui.Box(c).Size(24, 24).Radius(12).Background(p.success).Center().Children(func() {
				ui.Icon(c, icCheck).FontSize(12).TextColor(p.pane)
			})
			ui.Text(c, name).FontWeight(600).TextColor(p.success)
		case n == current:
			ui.Box(c).Size(24, 24).Radius(12).Background(p.accent).Center().Children(func() {
				ui.Text(c, itoa(n)).FontSize(fontSmall).TextColor(p.onAccent)
			})
			ui.Text(c, name).FontWeight(600)
		default:
			ui.Box(c).Size(24, 24).Radius(12).Border(1.5, p.muted).Center().Children(func() {
				ui.Text(c, itoa(n)).FontSize(fontSmall).TextColor(p.muted)
			})
			muted(c, name, fontBody)
		}
	})
}

// setupTitle is a step's heading and sentence.
func setupTitle(c *ui.Context, title, body string) {
	ui.Text(c, title).FontSize(28).FontWeight(600).Level(1)
	muted(c, body, fontBody).Wrap().MaxWidth(520).LineHeight(1.5)
}

// setupFoot is the step's Back and Continue.
func (a *App) setupFoot(c *ui.Context, back func(), next string, nextOff bool, onNext func()) {
	ui.Spacer(c)
	ui.Row(c).FillWidth().Gap(10).Children(func() {
		if back != nil && button(c, "setup-back", L.ActionBack(), nil).Clicked() {
			back()
		}
		ui.Spacer(c)
		if primary(c, "setup-next", next, nil).Disabled(nextOff).Clicked() {
			onNext()
		}
	})
}

// setupServer gates sign-in on the negotiated backend capability.
func (a *App) setupServer(c *ui.Context) {
	p := pal(c)
	st := &a.setup
	setupTitle(c, L.SetupWelcomeTitle(), L.SetupWelcomeBody())
	if !a.snap.Daemon.Connected {
		muted(c, L.DaemonDisconnected(), fontBody).Wrap()
	} else if !a.snap.Daemon.Capabilities["login"] {
		muted(c, L.LoginUnavailable(), fontBody).Wrap()
	}
	ui.Column(c).Gap(6).MaxWidth(480).Margin(8, 0, 0, 0).Children(func() {
		ui.Text(c, L.ServerAddressLabel()).FontSize(13).FontWeight(600)
		in := ui.TextInput(c.Key("setup-server"), &st.server).Placeholder("cloud.example.com").Label(L.ServerAddressLabel()).AutoFocus()
		if st.failed != "" {
			in.Error(st.failed)
		}
		muted(c, L.ServerAddressHint(), fontSmall).Wrap()
		if in.Submitted() {
			a.beginLogin()
		}
	})
	ui.Row(c).Gap(10).MaxWidth(480).Padding(12, 14).Radius(radiusCard).Background(p.subtle).Children(func() {
		ui.Icon(c, icLock).FontSize(18).TextColor(p.muted)
		muted(c, L.SignInNote(), 13).Wrap().Grow(1).MinWidth(0)
	})
	var back func()
	if len(a.snap.Accounts) > 0 {
		back = func() { a.setup = setupState{} }
	}
	a.setupFoot(c, back, L.ActionContinue(), strings.TrimSpace(st.server) == "" || a.busy["login"] || !a.loginAvailable(), a.beginLogin)
}

// loginAvailable requires a live negotiated login capability.
func (a *App) loginAvailable() bool {
	return a.snap.Daemon.Connected && a.snap.Daemon.Capabilities["login"]
}

// beginLogin starts Login Flow v2 and waits for the browser step.
func (a *App) beginLogin() {
	st := &a.setup
	server := strings.TrimSpace(st.server)
	if server == "" || a.busy["login"] || !a.loginAvailable() {
		return
	}
	a.do("login", func() error {
		l, err := a.client.StartLogin(server)
		if err != nil {
			return err
		}
		a.later(func() {
			a.setup.login, a.setup.step, a.setup.failed = l, stepSignIn, ""
			a.openBrowser(l.LoginURL)
		})
		id, err := a.client.WaitLogin(a.ctx, l.ID)
		a.later(func() {
			if a.setup.step != stepSignIn || a.setup.login.ID != l.ID {
				return // gone back meanwhile
			}
			if err != nil {
				a.setup.step, a.setup.failed = stepServer, L.SignInFailed()
				if t := reasonText(syncd.ReasonOf(err)); t != "" {
					a.setup.failed += " " + t
				}
				return
			}
			a.setup.accountID, a.setup.step = id, stepFolders
			a.loadRemote(id)
		})
		return nil
	}, func(err error) {
		if err != nil {
			a.setup.failed = L.AddressInvalid()
			if t := reasonText(syncd.ReasonOf(err)); t != "" {
				a.setup.failed = t
			}
		}
	})
}

func (a *App) setupSignIn(c *ui.Context) {
	p := pal(c)
	st := &a.setup
	setupTitle(c, L.SetupSignInTitle(), L.SetupSignInBody(st.server))
	ui.Row(c).Gap(24).Padding(24).MaxWidth(500).Radius(radiusDlg).Border(1, p.border).Children(func() {
		ui.Column(c).Width(96).AlignItems(ui.Center).Gap(6).Children(func() {
			ui.Icon(c, icBrowser).FontSize(40)
			muted(c, L.Browser(), fontSmall)
		})
		ui.Column(c).Grow(1).AlignItems(ui.Center).Gap(8).Children(func() {
			ui.Spinner(c).Size(20, 20)
			muted(c, L.WaitingBrowser(), 13).TextAlign(ui.Center).Role(ui.RoleStatus)
		})
		ui.Column(c).Width(96).AlignItems(ui.Center).Gap(6).Children(func() {
			mark(c, 40)
			muted(c, "Nextcloud MyGo", fontSmall)
		})
	})
	ui.Row(c).Gap(16).Children(func() {
		if link(c, "setup-again", L.OpenBrowserAgain()).Clicked() {
			a.openBrowser(st.login.LoginURL)
		}
		if link(c, "setup-copy", L.CopySignInLink()).Clicked() {
			mygo.Clipboard.WriteText(st.login.LoginURL)
			a.setToast(L.LinkCopied())
		}
	})
	muted(c, L.SignInFoot(), fontSmall).Wrap().MaxWidth(500)
	a.setupFoot(c, func() {
		id := st.login.ID
		a.do("login-cancel", func() error { return a.client.CancelLogin(id) }, nil)
		a.setup.step = stepServer
	}, L.ActionContinue(), true, func() {})
}

// loadRemote lists an account's folders for step 3; the user's home and
// documents folders start chosen when they exist on the server.
func (a *App) loadRemote(accountID string) {
	a.setup.remoteErr = ""
	a.do("remote", func() error {
		list, err := a.client.RemoteFolders(accountID)
		a.later(func() {
			a.setup.remote = list
			if err != nil {
				a.setup.remoteErr = errText(err)
			}
			for _, r := range list {
				if !r.Encrypted && (r.Path == "/Documents" || r.Path == "/Photos") {
					a.setup.chosen[r.Path] = true
				}
			}
		})
		return nil // shown in the step itself
	}, nil)
}

// setupFolders lists the server's folders to choose from; while file sync
// is not a supported capability, it says so first.
func (a *App) setupFolders(c *ui.Context) {
	p := pal(c)
	st := &a.setup
	setupTitle(c, L.SetupFoldersTitle(), L.SetupFoldersBody())
	if !a.snap.Daemon.Capabilities["fullSync"] {
		// File sync is not a supported capability yet: it says so before
		// anything is chosen, and never as supported.
		ui.Row(c).Gap(10).MaxWidth(620).Padding(12, 14).Radius(radiusCard).Background(p.warnBg).Border(1, p.warnBorder).Children(func() {
			ui.Icon(c, icWarn).FontSize(18).TextColor(p.warn)
			ui.Text(c, L.SyncPreview()).FontSize(13).Wrap().Grow(1).MinWidth(0)
		})
	}
	if st.remoteErr != "" {
		ui.Row(c).Gap(10).Children(func() {
			ui.Text(c, st.remoteErr).FontSize(13).TextColor(p.danger).Wrap().Grow(1).MinWidth(0)
			if button(c, "setup-retry", L.ActionTryAgain(), nil).Disabled(a.busy["remote"]).Clicked() {
				a.loadRemote(st.accountID)
			}
		})
	}
	ui.Row(c).Gap(10).Children(func() {
		muted(c, L.InsideLabel(), 13)
		ui.Text(c, st.base).FontSize(13).FontWeight(600)
		if button(c, "setup-base", L.ActionChange(), nil).Height(28).Clicked() {
			a.async(func() {
				dirs, err := mygo.Dialog.Open(mygo.OpenDialogOptions{Directory: true, CreateDirectories: true, DefaultPath: st.base})
				if err == nil && len(dirs) == 1 {
					a.later(func() { a.setup.base = dirs[0] })
				}
			})
		}
	})
	card(c).FillWidth().Clip().Children(func() {
		for i, r := range st.remote {
			name := strings.TrimPrefix(r.Path, "/")
			row := ui.Row(c.Key("rf-"+r.Path)).FillWidth().Gap(12).Padding(12, 14)
			if i < len(st.remote)-1 {
				row.BorderWidth(0, 0, 1, 0).BorderColor(p.hover)
			}
			if r.Encrypted {
				row.Background(p.subtle)
			}
			row.Children(func() {
				on := st.chosen[r.Path]
				if ui.Checkbox(c.Key("rf-cb-"+r.Path), &on, name).Disabled(r.Encrypted).Changed() {
					st.chosen[r.Path] = on
				}
				ui.Spacer(c)
				switch {
				case r.Encrypted:
					muted(c, L.EncryptedUnsupported(), 13)
				case on:
					muted(c, "→ "+filepath.Join(filepath.Base(st.base), name), 13)
				default:
					muted(c, L.NotSynced(), 13)
				}
			})
		}
	})
	n := 0
	for _, on := range st.chosen {
		if on {
			n++
		}
	}
	// No way back to signing in: that is a new login from step 1. Adding
	// folders to an account that exists can be left.
	var back func()
	if st.addOnly {
		back = func() { a.setup = setupState{} }
	}
	next := L.ActionContinue()
	if st.addOnly {
		next = L.ActionStartSyncing()
	}
	a.setupFoot(c, back, next, n == 0, func() {
		if st.addOnly {
			a.finishSetup()
			return
		}
		st.step = stepStart
	})
}

func (a *App) setupStart(c *ui.Context) {
	p := pal(c)
	st := &a.setup
	acc, _ := a.snap.Account(st.accountID)
	n := 0
	for _, on := range st.chosen {
		if on {
			n++
		}
	}
	setupTitle(c, L.SetupStartTitle(), L.SetupStartBody(n, accountName(acc)))
	card(c).FillWidth().Clip().Children(func() {
		switchRow(c, "setup-login", icPower, L.StartAtSignIn(), L.StartAtSignInHint(), &st.atLogin, true)
		switchRow(c, "setup-notify", icBell, L.NotifyMe(), L.NotifyMeHint(), &st.notify, false)
	})
	ui.Row(c).FillWidth().Gap(14).Padding(14, 16).Radius(radiusCard).Background(p.subtle).Children(func() {
		ui.Icon(c, icHouse).FontSize(22).TextColor(p.muted)
		ui.Column(c).Grow(1).MinWidth(0).Gap(2).Children(func() {
			ui.Text(c, L.OtherAddressTitle()).FontWeight(600).Wrap()
			muted(c, L.OtherAddressBody(), 13).Wrap()
		})
		if button(c, "setup-addr", L.ActionAddAddress(), nil).Clicked() {
			a.addr = addrForm{open: true, accountID: st.accountID}
		}
	})
	a.setupFoot(c, func() { st.step = stepFolders }, L.ActionStartSyncing(), false, a.finishSetup)
}

// switchRow is a row with an icon tile, a title and a sentence, and a
// switch bound to on.
func switchRow(c *ui.Context, key string, icon *ui.SVG, title, hint string, on *bool, rule bool) bool {
	p := pal(c)
	changed := false
	r := ui.Row(c.Key(key)).FillWidth().Gap(14).Padding(14, 16)
	if rule {
		r.BorderWidth(0, 0, 1, 0).BorderColor(p.hover)
	}
	r.Children(func() {
		tile(c, icon, 36, p.tint, p.accentText)
		ui.Column(c).Grow(1).MinWidth(0).Gap(2).Children(func() {
			ui.Text(c, title).FontWeight(600)
			muted(c, hint, 13).Wrap()
		})
		changed = ui.Switch(c.Key(key+"-sw"), on).Label(title).Changed()
	})
	return changed
}

// finishSetup adds the chosen folders and applies the choices; sync
// starts only now.
func (a *App) finishSetup() {
	st := a.setup
	var add [][2]string
	for _, r := range st.remote {
		if st.chosen[r.Path] && !r.Encrypted {
			add = append(add, [2]string{r.Path, filepath.Join(st.base, strings.TrimPrefix(r.Path, "/"))})
		}
	}
	if !st.addOnly {
		pr := a.prefs
		pr.NotifyDecisions, pr.NotifySignIn, pr.NotifyConflicts = st.notify, st.notify, st.notify
		a.setPrefs(pr)
		if a.window != nil {
			_ = mygo.App.SetOpenAtLogin(st.atLogin)
		}
	}
	a.setup = setupState{}
	a.page = pageOverview
	demo := a.snap.Daemon.Demo
	a.do("add-folders", func() error {
		for _, f := range add {
			// SyncD inspects an existing local folder only: make the empty
			// one chosen, never anything inside it; sample data makes none.
			if !demo {
				if err := os.MkdirAll(f[1], 0o700); err != nil {
					return err
				}
			}
			if err := a.client.AddFolder(st.accountID, f[0], f[1]); err != nil {
				return folderError{name: filepath.Base(f[1]), err: err}
			}
		}
		return nil
	}, func(err error) {
		var fe folderError
		switch {
		case errors.As(err, &fe):
			a.setToast(L.AddFolderFailed(fe.name) + " " + errText(fe.err))
		case err != nil:
			a.setToast(errText(err))
		}
	})
}

// folderError is a folder that could not be added, by name.
type folderError struct {
	name string
	err  error
}

func (e folderError) Error() string { return e.name + ": " + e.err.Error() }
func (e folderError) Unwrap() error { return e.err }

// languageSelect chooses the interface's language; empty follows Windows.
func (a *App) languageSelect(c *ui.Context, key string) {
	names := []string{L.FollowWindows(), "English", "简体中文"}
	tags := []string{"", "en", "zh-Hans"}
	cur := names[0]
	for i, t := range tags {
		if t == a.prefs.Language {
			cur = names[i]
		}
	}
	if ui.Select(c.Key(key), &cur, names).Label(L.Language()).MinWidth(180).Changed() {
		for i, n := range names {
			if n == cur {
				pr := a.prefs
				pr.Language = tags[i]
				a.setPrefs(pr)
				mygo.App.SetLocale(tags[i])
			}
		}
	}
}
