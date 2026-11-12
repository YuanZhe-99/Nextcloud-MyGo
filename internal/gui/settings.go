package gui

import (
	"context"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/egoist/mygo"
	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/version"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

type settingsPage int

// Settings' pages, in the order of its list.
const (
	spGeneral settingsPage = iota
	spNotifications
	spSync
	spNetwork
	spAbout
	spDiagnostics
)

// settingsState is the Settings sheet while open.
type settingsState struct {
	open  bool
	page  settingsPage
	query string
}

// settingDef is one setting as search finds it.
type settingDef struct {
	page  settingsPage
	words func() string
}

// settingsIndex is every setting's words, for search.
var settingsIndex = []settingDef{
	{spGeneral, func() string { return L.Language() + " " + L.LanguageHint() }},
	{spGeneral, func() string { return L.Appearance() + " " + L.ThemeDark() + " " + L.ThemeLight() }},
	{spGeneral, func() string { return L.OpenAtSignIn() + " " + L.OpenAtSignInHint() }},
	{spGeneral, func() string { return L.StartInTray() + " " + L.StartInTrayHint() }},
	{spGeneral, func() string { return L.CloseButton() + " " + L.CloseButtonHint() }},
	{spGeneral, func() string { return L.AddAnotherAccount() }},
	{spNotifications, func() string {
		return L.NotifyDecisions() + " " + L.NotifySignIn() + " " + L.NotifyConflicts() + " " + L.NotifyUpdates()
	}},
	{spSync, func() string { return L.PauseEverything() + " " + L.DeletionProtection() }},
	{spNetwork, func() string { return L.NetworkSettingsRow() + " " + L.NetworkSettingsHint() }},
	{spAbout, func() string {
		return L.SectionUpdates() + " " + L.CheckDaily() + " " + L.AutoInstall() + " " + L.LicenseRow() + " " + L.ActionSourceCode()
	}},
	{spDiagnostics, func() string { return L.SyncServiceRow() + " " + L.SupportBundle() }},
}

// pageName is a Settings page's title.
func (pg settingsPage) name() string {
	switch pg {
	case spNotifications:
		return L.PageNotifications()
	case spSync:
		return L.PageSync()
	case spNetwork:
		return L.PageNetwork()
	case spAbout:
		return L.PageAbout()
	case spDiagnostics:
		return L.PageDiagnostics()
	}
	return L.PageGeneral()
}

// matches tells which pages hold a setting with every word of query.
func matches(query string) map[settingsPage]bool {
	words := strings.Fields(strings.ToLower(query))
	out := map[settingsPage]bool{}
	for _, d := range settingsIndex {
		text := strings.ToLower(d.page.name() + " " + d.words())
		all := true
		for _, w := range words {
			if !strings.Contains(text, w) {
				all = false
				break
			}
		}
		if all {
			out[d.page] = true
		}
	}
	return out
}

// settingsSheet is Settings over the dimmed window: search and the pages
// on the left, the page on the right (design/SettingsGeneral.html).
func (a *App) settingsSheet(c *ui.Context) {
	if !a.settings.open {
		return
	}
	p := pal(c)
	ui.DialogBase(c.Key("settings"), &a.settings.open, func(backdrop, panel ui.Element) {
		backdrop.Background(p.scrim)
		panel.WidthPercent(90).MaxWidth(1152).HeightPercent(90).MaxHeight(720).Radius(radiusDlg).Background(p.pane).Shadow(0, 18, 48, 0, ui.RGBA(10, 16, 24, 0.3)).Clip().Label(L.SettingsTitle()).Children(func() {
			ui.Row(c).FillWidth().Height(52).Shrink(0).Padding(0, 12, 0, 20).BorderWidth(0, 0, 1, 0).BorderColor(p.border).Children(func() {
				ui.Text(c, L.SettingsTitle()).FontSize(16).FontWeight(600).Grow(1).Level(1)
				b := ui.ButtonBase(c.Key("set-close")).Size(36, 36).Radius(radiusCtl).Center().Label(L.WindowClose())
				if b.Hovered() {
					b.Background(p.hover)
				}
				b.Children(func() { ui.Icon(c, icClose).FontSize(16) })
				if b.Clicked() {
					a.settings.open = false
				}
			})
			ui.Row(c).Grow(1).FillWidth().AlignItems(ui.Stretch).Children(func() {
				a.settingsNav(c)
				ui.Scroll(c.Key("set-page")).Grow(1).MinWidth(0).Padding(28, 40).Gap(22).Children(func() {
					a.settingsPageView(c)
				})
			})
		})
	})
}

// settingsNav is search and the list of pages, or the pages search found.
func (a *App) settingsNav(c *ui.Context) {
	p := pal(c)
	ui.Column(c).Width(248).Shrink(0).Padding(14, 12).Gap(2).Background(p.subtle).BorderWidth(0, 1, 0, 0).BorderColor(p.border).Children(func() {
		ui.SearchField(c.Key("set-search"), &a.settings.query).Placeholder(L.SearchSettings()).Margin(0, 0, 12, 0)
		found := matches(a.settings.query)
		item := func(pg settingsPage, dotted bool) {
			if a.settings.query != "" && !found[pg] {
				return
			}
			on := a.settings.page == pg
			b := ui.ButtonBase(c.Key("set-nav-"+itoa(int(pg)))).Height(34).Padding(0, 10).Radius(radiusCtl).Selected(on).Label(pg.name())
			switch {
			case on:
				b.Background(p.tint)
			case b.Hovered():
				b.Background(p.hover)
			}
			b.Children(func() {
				ui.Row(c).FillWidth().Gap(8).Children(func() {
					t := ui.Text(c, pg.name()).Grow(1)
					if on {
						t.FontWeight(600).TextColor(p.accentText)
					}
					if dotted {
						dot(c, p.accent, false)
					}
				})
			})
			if b.Clicked() {
				a.settings.page = pg
				if pg == spDiagnostics {
					a.loadDiagnostics()
				}
			}
		}
		group := func(name string) {
			if a.settings.query == "" {
				muted(c, name, fontSmall).FontWeight(600).Padding(10, 10, 4, 10)
			}
		}
		if a.settings.query != "" && len(found) == 0 {
			muted(c, L.NoSettingsFound(), 13).Padding(6, 10)
		}
		group(L.GroupPreferences())
		item(spGeneral, false)
		item(spNotifications, false)
		group(L.GroupSync())
		item(spSync, false)
		item(spNetwork, false)
		group("Nextcloud MyGo")
		item(spAbout, a.upd.available != nil)
		item(spDiagnostics, false)
	})
}

// settingsPageView is the page chosen: a title and a sentence, then its
// sections of rows.
func (a *App) settingsPageView(c *ui.Context) {
	head := func(about string) {
		ui.Column(c).Gap(4).MaxWidth(720).FillWidth().Children(func() {
			ui.Text(c, a.settings.page.name()).FontSize(22).FontWeight(600).Level(2)
			muted(c, about, fontBody)
		})
	}
	switch a.settings.page {
	case spNotifications:
		head(L.NotificationsAbout())
		a.notificationsPage(c)
	case spSync:
		head(L.SyncAbout())
		a.syncSettingsPage(c)
	case spNetwork:
		head(L.NetworkSettingsAbout())
		section(c, "", func(r *rows) {
			r.row(c, "set-net", icGlobe, L.NetworkSettingsRow(), L.NetworkSettingsHint(), func() {
				if button(c, "set-net-open", L.ActionOpenNetwork(), nil).Clicked() {
					a.settings.open, a.page = false, pageNetwork
				}
			})
		})
	case spAbout:
		head(L.AboutAbout())
		a.aboutPage(c)
	case spDiagnostics:
		head(L.DiagnosticsAbout())
		a.diagnosticsPage(c)
	default:
		head(L.GeneralAbout())
		a.generalPage(c)
	}
}

// rows builds a section's rounded list of rows.
type rows struct {
	n int
}

// row is an icon tile, a title over one sentence, and its control.
func (r *rows) row(c *ui.Context, key string, icon *ui.SVG, title, hint string, control func()) {

	p := pal(c)
	e := ui.Row(c.Key(key)).FillWidth().Gap(14).Padding(12, 16)
	if r.n > 0 {
		e.BorderWidth(1, 0, 0, 0).BorderColor(p.hover)
	}
	r.n++
	e.Children(func() {
		tile(c, icon, 32, p.hover, p.text)
		ui.Column(c).Grow(1).MinWidth(0).Children(func() {
			ui.Text(c, title).FontWeight(600)
			if hint != "" {
				muted(c, hint, 13).Wrap()
			}
		})
		ui.Row(c).Gap(8).Shrink(0).Children(control)
	})
}

// section is a muted title over a rounded list of rows.
func section(c *ui.Context, title string, build func(r *rows)) {
	p := pal(c)
	ui.Column(c).Gap(8).MaxWidth(720).FillWidth().Children(func() {
		if title != "" {
			muted(c, title, 13).FontWeight(600)
		}
		ui.Column(c).FillWidth().Border(1, p.border).Radius(radiusCard).Clip().Children(func() {
			build(&rows{})
		})
	})
}

func (a *App) generalPage(c *ui.Context) {
	section(c, L.SectionLanguageLook(), func(r *rows) {
		r.row(c, "set-lang", icLanguage, L.Language(), L.LanguageHint(), func() { a.languageSelect(c, "set-lang-sel") })
		r.row(c, "set-theme", icContrast, L.Appearance(), L.AppearanceHint(), func() {
			themes := []string{"system", "light", "dark"}
			cur := 0
			for i, t := range themes {
				if t == a.prefs.Theme {
					cur = i
				}
			}
			if ui.Segmented(c.Key("set-theme-seg"), &cur, L.ThemeSystem(), L.ThemeLight(), L.ThemeDark()).Label(L.Appearance()).Changed() {
				pr := a.prefs
				pr.Theme = themes[cur]
				a.setPrefs(pr)
				ApplyTheme(pr.Theme)
			}
		})
	})
	section(c, L.SectionStartClose(), func(r *rows) {
		r.row(c, "set-login", icPower, L.OpenAtSignIn(), L.OpenAtSignInHint(), func() {
			on := a.openAtLogin()
			if ui.Switch(c.Key("set-login-sw"), &on).Label(L.OpenAtSignIn()).Changed() && a.window != nil {
				_ = mygo.App.SetOpenAtLogin(on)
			}
		})
		r.row(c, "set-tray", icTray, L.StartInTray(), L.StartInTrayHint(), func() {
			on := a.prefs.StartInTray
			if ui.Switch(c.Key("set-tray-sw"), &on).Label(L.StartInTray()).Changed() {
				pr := a.prefs
				pr.StartInTray = on
				a.setPrefs(pr)
			}
		})
		r.row(c, "set-close", icWinX, L.CloseButton(), L.CloseButtonHint(), func() {
			opts := []string{L.CloseKeepsTray(), L.CloseCloses()}
			cur := opts[0]
			if a.prefs.CloseQuits {
				cur = opts[1]
			}
			if ui.Select(c.Key("set-close-sel"), &cur, opts).Label(L.CloseButton()).MinWidth(200).Changed() {
				pr := a.prefs
				pr.CloseQuits = cur == opts[1]
				a.setPrefs(pr)
			}
		})
	})
	section(c, L.SectionSetup(), func(r *rows) {
		r.row(c, "set-add", icPlus, L.AddAnotherAccount(), L.AddAnotherAccountHint(), func() {
			if button(c, "set-add-btn", L.ActionAddAccountShort(), nil).Clicked() {
				a.settings.open = false
				a.startSetup()
			}
		})
	})
}

// openAtLogin reads whether Windows starts the app at sign-in; tests have
// no app to ask.
func (a *App) openAtLogin() bool {
	if a.window == nil {
		return false
	}
	return mygo.App.OpenAtLogin()
}

func (a *App) notificationsPage(c *ui.Context) {
	section(c, L.SectionNotify(), func(r *rows) {
		// Each switch edits a copy of the settings, then saves it whole.
		toggle := func(key string, icon *ui.SVG, title, hint string, field func(*Prefs) *bool) {
			r.row(c, key, icon, title, hint, func() {
				pr := a.prefs
				if ui.Switch(c.Key(key+"-sw"), field(&pr)).Label(title).Changed() {
					a.setPrefs(pr)
				}
			})
		}
		toggle("n-dec", icTrash, L.NotifyDecisions(), L.NotifyDecisionsHint(), func(p *Prefs) *bool { return &p.NotifyDecisions })
		toggle("n-sign", icLock, L.NotifySignIn(), L.NotifySignInHint(), func(p *Prefs) *bool { return &p.NotifySignIn })
		toggle("n-conf", icFiles, L.NotifyConflicts(), L.NotifyConflictsHint(), func(p *Prefs) *bool { return &p.NotifyConflicts })
		toggle("n-upd", icDownload, L.NotifyUpdates(), L.NotifyUpdatesHint(), func(p *Prefs) *bool { return &p.NotifyUpdates })
	})
}

func (a *App) syncSettingsPage(c *ui.Context) {
	p := pal(c)
	section(c, L.SectionSyncAll(), func(r *rows) {
		r.row(c, "set-pause", icPause, L.PauseEverything(), L.PauseEverythingHint(), func() {
			on := a.snap.Daemon.AllPaused
			if ui.Switch(c.Key("set-pause-sw"), &on).Label(L.PauseEverything()).Disabled(a.busy["all"]).Changed() {
				cmd := a.client.ResumeAll
				if on {
					cmd = a.client.PauseAll
				}
				a.do("all", cmd, nil)
			}
		})
		r.row(c, "set-protect", icShield, L.DeletionProtection(), L.DeletionProtectionHint(), func() {
			pill(c, L.AlwaysOn(), p.tint, p.accentText)
		})
	})
}

func (a *App) aboutPage(c *ui.Context) {
	p := pal(c)
	u := &a.upd
	ui.Column(c).Gap(8).MaxWidth(720).FillWidth().Children(func() {
		muted(c, L.SectionUpdates(), 13).FontWeight(600)
		switch {
		case !mygo.Updater.Enabled():
			ui.Row(c).FillWidth().Gap(10).Padding(12, 16).Radius(radiusCard).Background(p.subtle).Children(func() {
				ui.Icon(c, icInfo).FontSize(16).TextColor(p.muted)
				muted(c, L.UpdatesOff(), 13).Wrap()
			})
		case u.available != nil:
			ui.Row(c).FillWidth().Gap(14).Padding(14, 16).Radius(radiusCard).Border(1, p.tintBorder).Background(p.tint.Alpha(0.35)).Children(func() {
				tile(c, icDownload, 32, p.accent, p.onAccent)
				ui.Column(c).Grow(1).MinWidth(0).Children(func() {
					ui.Text(c, L.UpdateAvailable(u.available.Version)).FontWeight(600)
					muted(c, L.UpdateAvailableHint(), 13).Wrap()
				})
				label := L.ActionInstallRestart()
				if u.installing {
					label = L.Installing()
				}
				if primary(c, "upd-install", label, nil).Disabled(u.installing).Clicked() {
					a.installUpdate()
				}
			})
		}
		section(c, "", func(r *rows) {
			hint := L.NotChecked()
			if !a.prefs.CheckedAt.IsZero() {
				hint = L.LastChecked(ago(a.prefs.CheckedAt, time.Now()))
			}
			if u.available == nil && !a.prefs.CheckedAt.IsZero() {
				hint = L.UpToDateApp() + " " + hint
			}
			r.row(c, "upd-daily", icRefresh, L.CheckDaily(), hint, func() {
				if button(c, "upd-check", L.ActionCheckNow(), nil).Disabled(!mygo.Updater.Enabled() || u.checking).Clicked() {
					a.checkUpdate()
				}
				on := a.prefs.CheckUpdates
				if ui.Switch(c.Key("upd-daily-sw"), &on).Label(L.CheckDaily()).Disabled(!mygo.Updater.Enabled()).Changed() {
					pr := a.prefs
					pr.CheckUpdates = on
					a.setPrefs(pr)
				}
			})
			r.row(c, "upd-auto", icClock, L.AutoInstall(), L.AutoInstallHint(), func() {
				modes := []string{"never", "idle", "daily"}
				opts := []string{L.AutoNever(), L.AutoIdle(), L.AutoDaily()}
				cur := opts[0]
				for i, m := range modes {
					if m == a.prefs.AutoInstall {
						cur = opts[i]
					}
				}
				if ui.Select(c.Key("upd-auto-sel"), &cur, opts).Label(L.AutoInstall()).MinWidth(210).Disabled(!a.prefs.CheckUpdates || !mygo.Updater.Enabled()).Changed() {
					for i, o := range opts {
						if o == cur {
							pr := a.prefs
							pr.AutoInstall = modes[i]
							a.setPrefs(pr)
						}
					}
				}
			})
		})
	})
	section(c, "Nextcloud MyGo", func(r *rows) {
		d := a.snap.Daemon
		state := L.SyncdNotConnected()
		switch {
		case d.Demo:
			state = L.SyncdSample()
		case d.Connected:
			state = L.SyncdRunning(strconv.Itoa(d.Protocol) + "." + strconv.Itoa(d.ProtocolMinor))
		case d.StartupState != "":
			state = L.SyncdWithoutState()
		}
		r.row(c, "ab-version", icInfo, L.VersionRow(version.Version), L.VersionHint(platformName(), a.snap.Daemon.Version, state), func() {
			if link(c, "ab-source", L.ActionSourceCode()).Clicked() {
				a.async(func() { _ = mygo.Shell.OpenExternal(sourceURL) })
			}
		})
		r.row(c, "ab-license", icDoc, L.LicenseRow(), L.LicenseHint(), func() {
			if button(c, "ab-licenses", L.ActionLicenses(), nil).Clicked() {
				a.async(func() { _ = mygo.Shell.OpenExternal(sourceURL + "/blob/main/THIRD_PARTY_NOTICES.md") })
			}
		})
	})
}

// sourceURL is where the source code lives.
const sourceURL = "https://github.com/YuanZhe-99/Nextcloud-MyGo"

// platformName is the OS and architecture this build runs on.
func platformName() string {
	osName := map[string]string{"windows": "Windows", "darwin": "macOS", "linux": "Linux"}[runtime.GOOS]
	arch := map[string]string{"arm64": "ARM64", "amd64": "x64"}[runtime.GOARCH]
	return strings.TrimSpace(osName + " " + arch)
}

// diagState is the Diagnostics page's last reading of SyncD and the last
// support bundle made.
type diagState struct {
	reading *syncd.RuntimeDiagnostics
	failed  string
	bundle  string
}

// loadDiagnostics reads SyncD's sanitized runtime counters
// (diagnostics.snapshot) off the main thread.
func (a *App) loadDiagnostics() {
	a.do("diag", func() error {
		ctx, cancel := context.WithTimeout(a.ctx, 10*time.Second)
		defer cancel()
		d, err := a.client.Diagnostics(ctx)
		a.later(func() {
			a.diag.reading, a.diag.failed = nil, ""
			if err != nil {
				a.diag.failed = errText(err)
				return
			}
			a.diag.reading = &d
		})
		return nil
	}, nil)
}

// diagnosticsPage is SyncD's own state as it reports it, and the support
// bundle it makes. Diagnostics may show codes, IDs, epochs and paths; the
// rest of the interface never does.
func (a *App) diagnosticsPage(c *ui.Context) {
	p := pal(c)
	section(c, L.SectionDiagnostics(), func(r *rows) {
		r.row(c, "dg-syncd", icPulse, L.SyncServiceRow(), L.SyncServiceHint(), func() {
			if button(c, "dg-refresh", L.ActionRefresh(), icRefresh).Disabled(a.busy["diag"]).Clicked() {
				a.loadDiagnostics()
			}
		})
		r.row(c, "dg-bundle", icArchive, L.SupportBundle(), L.SupportBundleHint(), func() {
			if button(c, "dg-export", L.ActionExport(), nil).Disabled(a.busy["export"] || a.snap.Daemon.Demo || a.snap.Daemon.StartupState == "").Clicked() {
				a.do("export", a.exportBundle, nil)
			}
		})
	})
	if a.diag.bundle != "" {
		ui.Row(c).MaxWidth(720).FillWidth().Gap(10).Padding(12, 16).Radius(radiusCard).Background(p.subtle).Children(func() {
			ui.Column(c).Grow(1).MinWidth(0).Children(func() {
				ui.Text(c, L.BundleSaved()).FontSize(13).FontWeight(600)
				muted(c, a.diag.bundle, fontSmall).Wrap()
			})
			if button(c, "dg-bundle-open", L.ActionOpenFolder(), nil).Clicked() {
				a.openPath(filepath.Dir(a.diag.bundle))
			}
			if button(c, "dg-bundle-copy", L.ActionCopyPath(), nil).Clicked() {
				mygo.Clipboard.WriteText(a.diag.bundle)
				a.setToast(L.ToastCopied())
			}
		})
	}
	if a.diag.failed != "" {
		ui.Text(c, a.diag.failed).FontSize(13).TextColor(p.danger).Wrap().MaxWidth(720)
	}
	if d := a.diag.reading; d != nil {
		grid := func(rows [][2]string) {
			ui.Grid(c).ColumnTracks(ui.Fixed(170), ui.Fr(1)).GapY(6).GapX(12).MaxWidth(720).FillWidth().Padding(12, 16).Radius(radiusCard).Border(1, p.border).Children(func() {
				for _, r := range rows {
					muted(c, r[0], 13)
					ui.Text(c, r[1]).FontSize(13).Selectable().Wrap()
				}
			})
		}
		grid([][2]string{
			{L.DiagVersion(), d.Version + " · " + d.Protocol},
			{L.DiagStartup(), string(d.StartupState) + " · " + strconv.FormatBool(d.StateAvailable) + " " + d.ReasonCode},
			{L.DiagNetwork(), d.NetworkGeneration},
			{L.DiagPaused(), strconv.FormatBool(d.AllPaused)},
		})
		for _, f := range d.Folders {
			grid([][2]string{
				{L.DiagFolder(), f.FolderID},
				{L.DiagState(), f.State + " " + f.ReasonCode + " " + strings.Join(f.Blockers, ",")},
				{L.DiagEpoch(), f.Epoch},
				{L.DiagTransfers(), L.DiagTransfersValue(u(f.ActiveUploads), u(f.ActiveDownloads), u(f.ActiveRequests))},
				{L.DiagUncertain(), u(f.UncertainWrites) + " · " + u(f.RetainedReplies)},
				{L.DiagTotals(), L.DiagTotalsValue(u(f.Requests), u(f.TransportFailures), size(int64(f.UploadBytes)), size(int64(f.DownloadBytes)))},
			})
		}
	}
}

// u is a counter as text.
func u(n uint64) string { return strconv.FormatUint(n, 10) }

// exportBundle asks SyncD for a sanitized support bundle and keeps its
// path for the page; nothing is uploaded.
func (a *App) exportBundle() error {
	path, err := a.client.ExportDiagnostics()
	if err != nil {
		return err
	}
	a.later(func() { a.diag.bundle = path })
	return nil
}
