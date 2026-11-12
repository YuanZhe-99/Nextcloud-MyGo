package gui

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"math"
	"strconv"

	"github.com/egoist/mygo"
	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// Tray is the status icon: a click opens the flyout (design/Tray.html),
// a right click the menu. Both act through the same calls as the window.
type Tray struct {
	app    *App
	tray   *mygo.Tray
	flyout *mygo.Window
	light  bool
	last   string
}

// NewTray makes the status icon (main thread); nil when the desktop has
// no tray, and closing the window then quits.
func (a *App) NewTray() *Tray {
	light := lightTaskbar()
	t, err := mygo.NewTray(mygo.TrayOptions{Icon: trayGlyph(light), IconIsTemplate: true, ToolTip: "Nextcloud MyGo"})
	if err != nil {
		return nil
	}
	tr := &Tray{app: a, tray: t, light: light}
	t.OnClick(tr.toggleFlyout)
	a.tray = tr
	tr.refresh()
	return tr
}

// refresh swaps the glyph for the taskbar's appearance and rebuilds the
// menu when the state or the language changed (main thread).
func (t *Tray) refresh() {
	if t == nil {
		return
	}
	if light := lightTaskbar(); light != t.light {
		t.light = light
		_ = t.tray.SetIcon(trayGlyph(light), true)
	}
	a := t.app
	o := overallOf(a.snap)
	pauseLabel := L.ActionPauseSync()
	if a.snap.Daemon.AllPaused {
		pauseLabel = L.ActionResumeSync()
	}
	live := a.snap.Daemon.Connected
	key := o.word() + pauseLabel + L.TrayOpen() + strconv.FormatBool(live)
	if t.flyout != nil {
		t.flyout.Invalidate()
	}
	if key == t.last {
		return
	}
	t.last = key
	t.tray.SetToolTip("Nextcloud MyGo: " + o.word())
	t.tray.SetMenu(mygo.NewMenu([]*mygo.MenuItem{
		{Label: o.word(), Disabled: true},
		mygo.Separator(),
		{Label: L.TrayOpen(), Click: func(*mygo.MenuItem, *mygo.Window) { a.Focus() }},
		{Label: pauseLabel, Disabled: !live, Click: func(*mygo.MenuItem, *mygo.Window) { a.togglePause() }},
		{Label: L.ActionCheckNow(), Disabled: !live, Click: func(*mygo.MenuItem, *mygo.Window) { a.checkAll() }},
		mygo.Separator(),
		{Label: L.TrayCloseWindow(), Click: func(*mygo.MenuItem, *mygo.Window) { a.QuitWindow() }},
		{Label: L.TrayQuitStop(), Click: func(*mygo.MenuItem, *mygo.Window) { a.QuitAndStop() }},
	}))
}

// toggleFlyout opens the flyout by the icon, or closes it.
func (t *Tray) toggleFlyout() {
	if t.flyout != nil && t.flyout.IsVisible() {
		t.flyout.Hide()
		return
	}
	if t.flyout == nil {
		t.flyout = mygo.NewFlyout(mygo.FlyoutOptions{Tray: t.tray, Width: 380, Height: 560, Content: ui.View(t.app.flyoutView)})
	}
	t.flyout.Show()
	t.flyout.Focus()
}

// togglePause pauses every folder, or resumes them.
func (a *App) togglePause() {
	cmd := a.client.PauseAll
	if a.snap.Daemon.AllPaused {
		cmd = a.client.ResumeAll
	}
	a.do("all", cmd, nil)
}

// flyoutView is the tray's panel: the state, what needs the user, the
// folders, and the actions (design/Tray.html).
func (a *App) flyoutView(c *ui.Context) {
	applyTheme(c)
	p := pal(c)
	s := a.snap
	o := overallOf(s)
	hide := func() {
		if a.tray != nil && a.tray.flyout != nil {
			a.tray.flyout.Hide()
		}
	}
	ui.Column(c).Fill().Background(p.subtle).TextColor(p.text).FontSize(fontBody).Children(func() {
		ui.Row(c).FillWidth().Gap(12).Padding(16).Background(p.pane).BorderWidth(0, 0, 1, 0).BorderColor(p.border).Children(func() {
			ui.Box(c).Children(func() {
				mark(c, 36)
				ui.Box(c).Size(12, 12).Radius(6).Background(o.color(p)).Border(2, p.pane).Absolute().Right(-3).Bottom(-3)
			})
			ui.Column(c).Grow(1).Children(func() {
				ui.Text(c, "Nextcloud MyGo").FontWeight(600)
				sub := o.word()
				if len(s.Accounts) > 0 && s.Daemon.Connected {
					if e, _, ok := activeEndpoint(s, s.Accounts[0].ID); ok {
						sub += " · " + L.SidebarThrough(e.Name)
					}
				}
				muted(c, sub, fontSmall)
			})
		})
		ui.Column(c).FillWidth().Padding(12, 16).Gap(8).Children(func() {
			for _, d := range s.Decisions {
				if d.Kind != syncd.DecisionBulkDeletions {
					continue
				}
				f, _ := s.Folder(d.FolderID)
				if flyoutNotice(c, "fly-d-"+d.ID, icWarn, p.warnBg, p.warnBorder, p.warn, L.FolderNeedsReview()+": "+f.Name, L.FolderDeletions(d.Count)) {
					hide()
					a.page = pageActivity
					a.Focus()
				}
			}
			for _, acc := range s.Accounts {
				if needsSignIn(s, acc) && flyoutNotice(c, "fly-s-"+acc.ID, icLock, p.pane, p.border, p.muted, L.AttnSignInTitle(""), accountName(acc)) {
					hide()
					a.page = pageFolders
					a.Focus()
				}
			}
		})
		ui.Scroll(c.Key("fly-folders")).Grow(1).FillWidth().Padding(4, 16).Children(func() {
			muted(c, L.TrayFolders(), fontSmall).FontWeight(600).Padding(6, 0)
			for _, f := range s.Folders {
				word, col, hollow := folderWord(f, s, p)
				switch {
				case f.State == syncd.FolderSyncing && f.Phase == syncd.PhaseUploading:
					word = L.TrayUploading()
				case f.State == syncd.FolderSyncing && f.Phase == syncd.PhaseDownloading:
					word = L.TrayDownloading()
				case f.State == syncd.FolderNeedsReview:
					word = L.TrayWaitingForYou()
				}
				ui.Row(c.Key("fly-f-"+f.ID)).FillWidth().Gap(10).Padding(8, 0).Children(func() {
					dot(c, col, hollow)
					ui.Text(c, f.Name).FontSize(13).Grow(1).Ellipsis("…").MaxLines(1)
					muted(c, word, 13)
				})
			}
		})
		ui.Column(c).FillWidth().Padding(12, 16).Gap(8).Background(p.pane).BorderWidth(1, 0, 0, 0).BorderColor(p.border).Children(func() {
			ui.Row(c).FillWidth().Gap(8).Children(func() {
				if primary(c, "fly-open", L.TrayOpen(), nil).Grow(1).Clicked() {
					hide()
					a.Focus()
				}
				label := L.ActionPause()
				if s.Daemon.AllPaused {
					label = L.ActionResume()
				}
				if button(c, "fly-pause", label, nil).Disabled(!s.Daemon.Connected || a.busy["all"]).Clicked() {
					a.togglePause()
				}
				if button(c, "fly-check", L.ActionCheckNow(), nil).Disabled(!s.Daemon.Connected || s.Daemon.AllPaused).Clicked() {
					a.checkAll()
				}
			})
			ui.Row(c).FillWidth().Gap(8).Children(func() {
				if flatButton(c, "fly-close", L.TrayCloseWindow(), p.text).Clicked() {
					a.QuitWindow()
				}
				if flatButton(c, "fly-quit", L.TrayQuitStop(), p.danger).Clicked() {
					a.QuitAndStop()
				}
			})
		})
	})
}

// flyoutNotice is a card in the flyout; it reports a click.
func flyoutNotice(c *ui.Context, key string, icon *ui.SVG, bg, border, tone ui.Color, title, body string) bool {
	b := ui.ButtonBase(c.Key(key)).FillWidth().Padding(10, 12).Radius(radiusCard).Background(bg).Border(1, border).Label(title)
	b.Children(func() {
		ui.Row(c).FillWidth().Gap(10).Children(func() {
			ui.Icon(c, icon).FontSize(18).TextColor(tone)
			ui.Column(c).Grow(1).MinWidth(0).Children(func() {
				ui.Text(c, title).FontSize(13).FontWeight(600).Ellipsis("…").MaxLines(1)
				muted(c, body, fontSmall).Ellipsis("…").MaxLines(1)
			})
		})
	})
	return b.Clicked()
}

// flatButton is a button without a frame, its label in fg.
func flatButton(c *ui.Context, key, label string, fg ui.Color) ui.Element {
	p := pal(c)
	b := ui.ButtonBase(c.Key(key)).Grow(1).Height(30).Radius(radiusCtl).Center().Label(label)
	if b.Hovered() {
		b.Background(p.hover)
	}
	b.Children(func() { ui.Text(c, label).FontSize(13).TextColor(fg) })
	return b
}

// trayGlyph is the tray icon: the mark's three rings, white for a dark
// taskbar or black for a light one, 32 pixels square. A placeholder until
// the Nextcloud icon comes from the upstream release.
func trayGlyph(light bool) []byte {
	const n = 32
	img := image.NewNRGBA(image.Rect(0, 0, n, n))
	ink := color.NRGBA{255, 255, 255, 255}
	if light {
		ink = color.NRGBA{0, 0, 0, 255}
	}
	rings := []struct{ x, r float64 }{{5.5, 3.6}, {16, 6}, {26.5, 3.6}}
	for y := range n {
		for x := range n {
			cov := 0.0
			for _, rg := range rings {
				d := math.Hypot(float64(x)+0.5-rg.x, float64(y)+0.5-16) - rg.r
				cov = math.Max(cov, math.Max(0, 1-math.Abs(d)/1.6))
			}
			if cov > 0 {
				c := ink
				c.A = uint8(cov * 255)
				img.SetNRGBA(x, y, c)
			}
		}
	}
	var buf bytes.Buffer
	_ = png.Encode(&buf, img)
	return buf.Bytes()
}
