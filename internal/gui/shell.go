package gui

import (
	"strconv"

	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// view is the main window: the title bar over first-run setup, or over
// the sidebar and the pane of the page chosen; Settings, dialogs and the
// toast above them.
func (a *App) view(c *ui.Context) {
	applyTheme(c)
	p := pal(c)
	ui.Column(c).Fill().Background(p.base).TextColor(p.text).FontSize(fontBody).Children(func() {
		a.titleBar(c)
		switch {
		case a.setupShown():
			a.setupView(c)
		case !stateKnown(a.snap) && len(a.snap.Accounts) == 0:
			a.statusView(c)
		default:
			ui.Row(c).Grow(1).FillWidth().AlignItems(ui.Stretch).Padding(0, 8, 8, 0).Children(func() {
				a.sidebar(c)
				a.pane(c)
			})
		}
		a.toastView(c)
	})
	a.settingsSheet(c)
	a.addrDialog(c)
	a.confirmDialog(c)
}

// titleBar is the window's top: the mark and name, a handle that moves
// the window, and Windows' caption buttons drawn by the view.
func (a *App) titleBar(c *ui.Context) {
	p := pal(c)
	ui.Row(c).Height(titleBarH).Shrink(0).FillWidth().PaddingStart(16).Gap(10).DragWindow().Children(func() {
		mark(c, 20)
		muted(c, "Nextcloud MyGo", fontSmall)
		ui.Spacer(c)
		w := a.window
		caption := func(key string, glyph *ui.SVG, label string, close bool) ui.Element {
			b := ui.ButtonBase(c.Key(key)).Size(46, titleBarH).Center().Label(label).Tooltip(label)
			fg := p.text
			if b.Hovered() {
				if close {
					b.Background(ui.Hex("#C42B1C"))
					fg = ui.Hex("#FFFFFF")
				} else {
					b.Background(p.hover)
				}
			}
			b.Children(func() { ui.Icon(c, glyph).FontSize(10).TextColor(fg) })
			return b
		}
		if caption("win-min", winMin, L.WindowMinimize(), false).Clicked() && w != nil {
			w.Minimize()
		}
		maximized := w != nil && w.IsMaximized()
		glyph, label := winMax, L.WindowMaximize()
		if maximized {
			glyph, label = winRest, L.WindowRestore()
		}
		if caption("win-max", glyph, label, false).MaximizeButton().Clicked() && w != nil {
			if maximized {
				w.Unmaximize()
			} else {
				w.Maximize()
			}
		}
		if caption("win-close", winClose, L.WindowClose(), true).Clicked() && w != nil {
			w.Close()
		}
	})
}

// sidebar is the accounts, the pages, Settings and the sync state.
func (a *App) sidebar(c *ui.Context) {
	p := pal(c)
	s := a.snap
	ui.Column(c).Width(sidebarW).Shrink(0).Padding(4, 12, 12, 12).Gap(2).Label(L.NavOverview()).Children(func() {
		if len(s.Accounts) > 0 {
			a.sidebarAccounts(c)
		}
		a.navItem(c, pageOverview, icHome, L.NavOverview(), 0)
		a.navItem(c, pageFolders, icFolder, L.NavFolders(), 0)
		a.navItem(c, pageNetwork, icGlobe, L.NavNetwork(), 0)
		a.navItem(c, pageActivity, icPulse, L.NavActivity(), waitingCount(s))
		ui.Spacer(c)
		if sbRow(c, "sb-settings", icGear, L.NavSettings(), false, 0).Clicked() {
			a.openSettings(spGeneral)
		}
		o := overallOf(s)
		b := ui.ButtonBase(c.Key("sb-status")).Padding(10).Margin(6, 0, 0, 0).Radius(radiusCtl).BorderWidth(1, 0, 0, 0).BorderColor(p.border).Label(o.word())
		b.Children(func() {
			ui.Row(c).Gap(10).Children(func() {
				dot(c, o.color(p), false)
				ui.Column(c).Children(func() {
					ui.Text(c, o.word()).FontSize(13).FontWeight(600)
					if len(s.Accounts) > 0 && s.Daemon.Connected {
						// The account the Network page shows: the first unless chosen.
						if e, _, ok := activeEndpoint(s, a.account); ok {
							muted(c, L.SidebarThrough(e.Name), fontSmall)
						}
					}
				})
			})
		})
		if b.Clicked() {
			a.page = pageNetwork
		}
	})
}

// sidebarShown is how many accounts the sidebar lists before it counts
// the rest.
const sidebarShown = 4

// sidebarAccounts lists the accounts, each opening Accounts & folders: its
// name and server, or that it needs signing in again.
func (a *App) sidebarAccounts(c *ui.Context) {
	p := pal(c)
	s := a.snap
	list := s.Accounts
	if len(list) > sidebarShown {
		list = list[:sidebarShown-1]
	}
	ui.Column(c).FillWidth().Padding(4).Margin(0, 0, 12, 0).Gap(2).Radius(radiusCard).Background(p.pane.Alpha(0.55)).Children(func() {
		for _, acc := range list {
			b := ui.ButtonBase(c.Key("sb-acc-" + acc.ID)).FillWidth().Padding(6).Radius(radiusCtl).Label(accountName(acc))
			if b.Hovered() {
				b.Background(p.pane.Alpha(0.8))
			}
			b.Children(func() {
				ui.Row(c).FillWidth().Gap(10).Children(func() {
					avatar(c, acc, 28)
					ui.Column(c).Grow(1).MinWidth(0).Children(func() {
						ui.Text(c, acc.DisplayName).FontSize(13).FontWeight(600).Ellipsis("…").MaxLines(1)
						if needsSignIn(s, acc) {
							ui.Text(c, L.SidebarSignIn()).FontSize(fontSmall).TextColor(p.warn).Ellipsis("…").MaxLines(1)
						} else {
							muted(c, acc.Server, fontSmall).Ellipsis("…").MaxLines(1)
						}
					})
				})
			})
			if b.Clicked() {
				a.page, a.account = pageFolders, acc.ID
			}
		}
		if more := len(s.Accounts) - len(list); more > 0 {
			if link(c, "sb-acc-more", L.SidebarMoreAccounts(more)).Margin(2, 6).Clicked() {
				a.page = pageFolders
			}
		}
	})
}

// navItem is a page's row in the sidebar, with a count of what waits.
func (a *App) navItem(c *ui.Context, pg page, icon *ui.SVG, label string, count int) {
	if sbRow(c, "nav-"+label, icon, label, a.page == pg, count).Clicked() {
		a.page = pg
	}
}

// sbRow is a row of the sidebar; selected shows the accent's bar.
func sbRow(c *ui.Context, key string, icon *ui.SVG, label string, selected bool, count int) ui.Element {
	p := pal(c)
	b := ui.ButtonBase(c.Key(key)).Height(36).Padding(0, 10).Radius(radiusCtl).Label(label)
	switch {
	case selected:
		b.Background(p.pane.Alpha(0.9))
	case b.Hovered():
		b.Background(p.pane.Alpha(0.5))
	}
	b.Children(func() {
		if selected {
			ui.Box(c).Absolute().Left(0).Top(10).Size(3, 16).Radius(2).Background(p.accent)
		}
		ui.Row(c).Gap(12).FillWidth().Children(func() {
			ui.Icon(c, icon).FontSize(18)
			t := ui.Text(c, label).Grow(1)
			if selected {
				t.FontWeight(600)
			}
			if count > 0 {
				ui.Box(c).MinWidth(20).Height(20).Padding(0, 6).Radius(10).Background(p.warn).Center().Children(func() {
					ui.Text(c, itoa(count)).FontSize(fontSmall).FontWeight(600).TextColor(p.onWarn)
				})
			}
		})
	})
	return b
}

// avatar is an account's initial in a circle.
func avatar(c *ui.Context, acc syncd.Account, size float32) {
	p := pal(c)
	name := acc.DisplayName
	if name == "" {
		name = acc.User
	}
	initial := "?"
	for _, r := range name {
		initial = string(r)
		break
	}
	ui.Box(c).Size(size, size).Shrink(0).Radius(size / 2).Background(p.tint).Center().Children(func() {
		ui.Text(c, initial).FontWeight(600).TextColor(p.accentText)
	})
}

// pane is the page chosen, on the rounded pane beside the sidebar.
func (a *App) pane(c *ui.Context) {
	p := pal(c)
	ui.Column(c).Grow(1).MinWidth(0).Background(p.pane).Border(1, p.border).Radius(radiusCard).Shadow(0, 2, 8, 0, ui.RGBA(20, 30, 45, 0.06)).Clip().Children(func() {
		switch {
		case a.snap.Daemon.Demo:
			ui.Row(c).FillWidth().Padding(6, 28).Gap(8).Background(p.subtle).BorderWidth(0, 0, 1, 0).BorderColor(p.border).Children(func() {
				ui.Icon(c, icInfo).FontSize(14).TextColor(p.muted)
				muted(c, L.DemoBanner(), fontSmall)
			})
		case !a.snap.Daemon.Connected:
			// The last known lists, never as current (PLAN.md §13.6).
			ui.Row(c).FillWidth().Padding(6, 28).Gap(8).Background(p.warnBg).BorderWidth(0, 0, 1, 0).BorderColor(p.warnBorder).Role(ui.RoleStatus).Children(func() {
				ui.Icon(c, icWarn).FontSize(14).TextColor(p.warn)
				ui.Text(c, L.DisconnectedBanner()).FontSize(fontSmall).Wrap().Grow(1).MinWidth(0)
			})
		}
		switch a.page {
		case pageFolders:
			a.foldersPage(c)
		case pageNetwork:
			a.networkPage(c)
		case pageActivity:
			a.activityPage(c)
		default:
			a.overviewPage(c)
		}
	})
}

// statusView stands for the pages while SyncD's state is not known: still
// connecting, or running without its state (a recovery start). Nothing is
// shown as empty or healthy meanwhile (INV-15).
func (a *App) statusView(c *ui.Context) {
	p := pal(c)
	d := a.snap.Daemon
	ui.Column(c).Grow(1).FillWidth().Center().Padding(32).Children(func() {
		ui.Column(c).MaxWidth(520).Gap(14).Padding(28).Radius(radiusDlg).Background(p.pane).Border(1, p.border).Role(ui.RoleStatus).Children(func() {
			if d.StartupState == "" {
				ui.Row(c).Gap(12).Children(func() {
					ui.Spinner(c).Size(20, 20)
					ui.Text(c, L.DaemonDisconnected()).FontSize(fontSection).FontWeight(600)
				})
				muted(c, L.DaemonConnectingHint(), 13).Wrap()
			} else {
				ui.Row(c).Gap(12).Children(func() {
					ui.Icon(c, icWarn).FontSize(22).TextColor(p.warn)
					ui.Text(c, L.StateUnavailableTitle()).FontSize(fontSection).FontWeight(600).Wrap().Grow(1).MinWidth(0)
				})
				muted(c, startupWord(d.StartupState), 13).Wrap()
				muted(c, L.StateUnavailableBody(), 13).Wrap()
			}
			ui.Row(c).Gap(10).Children(func() {
				if button(c, "st-diag", L.PageDiagnostics(), icLog).Clicked() {
					a.openSettings(spDiagnostics)
				}
				if button(c, "st-settings", L.NavSettings(), icGear).Clicked() {
					a.openSettings(spGeneral)
				}
			})
		})
	})
}

// pageHeader is a page's title and the buttons at its end.
func pageHeader(c *ui.Context, title string, actions func()) {
	ui.Row(c).FillWidth().Padding(20, 28, 12, 28).Gap(12).Shrink(0).Children(func() {
		ui.Text(c, title).FontSize(fontTitle).FontWeight(600).Grow(1).Level(1)
		if actions != nil {
			actions()
		}
	})
}

// pageBody is a page's scrolling content under its header.
func pageBody(c *ui.Context, key string, fn func()) {
	ui.Scroll(c.Key(key)).Grow(1).FillWidth().Padding(4, 28, 24, 28).Gap(20).Children(fn)
}

// toastView is the short message at the window's foot.
func (a *App) toastView(c *ui.Context) {
	if a.toast == "" {
		return
	}
	p := pal(c)
	ui.Box(c).Attach(ui.AnchorBottom, ui.AnchorBottom).Bottom(24).Padding(10, 16).Radius(radiusCard).Background(p.text).Role(ui.RoleStatus).Children(func() {
		ui.Text(c, a.toast).TextColor(p.pane)
	})
}

// openSettings opens the Settings sheet at a page.
func (a *App) openSettings(pg settingsPage) {
	a.settings.open, a.settings.page, a.settings.query = true, pg, ""
	if pg == spDiagnostics {
		a.loadDiagnostics()
	}
}

// itoa is a count for a badge: more than 99 shows as 99+.
func itoa(n int) string {
	if n > 99 {
		return "99+"
	}
	return strconv.Itoa(n)
}
