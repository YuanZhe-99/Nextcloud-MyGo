package gui

import (
	"time"

	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// The Activity page's filters.
const (
	filterAll = iota
	filterAttention
	filterFiles
	filterNetwork
)

// activityPage is the deletion review, what else needs the user, and the
// recent history (design/Activity.html).
func (a *App) activityPage(c *ui.Context) {
	s := a.snap
	waiting := len(s.Decisions)
	for _, acc := range s.Accounts {
		if !acc.SignedIn {
			waiting++
		}
	}
	pageHeader(c, L.NavActivity(), func() {
		ui.Segmented(c.Key("act-filter"), &a.filter, L.FilterAll(), L.FilterAttention(waiting), L.FilterFiles(), L.FilterNetwork())
	})
	var review *syncd.Decision
	for i := range s.Decisions {
		if s.Decisions[i].Kind == syncd.DecisionLocalDeletions {
			review = &s.Decisions[i]
			break
		}
	}
	pageBody(c, "act-body", func() {
		showReview := review != nil && (a.filter == filterAll || a.filter == filterAttention)
		grid := ui.Grid(c).Gap(20).FillWidth().AlignItems(ui.Start)
		if showReview {
			grid.ColumnTracks(ui.Fr(1.5), ui.Fr(1))
		} else {
			grid.ColumnTracks(ui.Fr(1))
		}
		grid.Children(func() {
			if showReview {
				a.reviewCard(c, *review)
			}
			ui.Column(c).Gap(0).MinWidth(0).Children(func() {
				if a.filter == filterAll || a.filter == filterAttention {
					others := waiting
					if review != nil {
						others--
					}
					switch {
					case others > 0:
						ui.Box(c).Margin(0, 0, 8, 0).Children(func() { sectionTitle(c, L.AlsoNeedsAttention()) })
						ui.Column(c).Gap(10).Margin(0, 0, 18, 0).Children(func() { a.attentionItems(c, "act") })
					case review == nil:
						muted(c, L.NothingNeedsYou(), fontBody).Margin(0, 0, 18, 0)
					}
				}
				if a.filter != filterAttention {
					a.recentList(c)
				}
			})
		})
	})
}

// reviewCard is a deletion proposal: how many files, which way the
// deletion would go, what it covers, and the three answers, the safe one
// first (PLAN.md §11.3, §17 item 6).
func (a *App) reviewCard(c *ui.Context, d syncd.Decision) {
	p := pal(c)
	f, _ := a.snap.Folder(d.FolderID)
	acc, _ := a.snap.Account(d.AccountID)
	ui.Column(c.Key("review-"+d.ID)).Border(1, p.warnBorder).Radius(radiusDlg).Clip().MinWidth(0).Children(func() {
		ui.Column(c).FillWidth().Padding(18, 20, 14, 20).Gap(6).Background(p.warnBg).Children(func() {
			ui.Text(c, L.ProtectionLabel(f.Name)).FontSize(fontSmall).FontWeight(600).TextColor(p.warn)
			ui.Text(c, L.ReviewTitle(d.Count)).FontSize(20).FontWeight(600).Level(2)
			muted(c, L.ReviewBody(f.Name), 13).Wrap()
		})
		ui.Row(c).FillWidth().Gap(14).Padding(16, 20).BorderWidth(0, 0, 1, 0).BorderColor(p.warnBorder).Children(func() {
			side := func(icon *ui.SVG, label, count string) {
				ui.Column(c).Width(130).Shrink(0).AlignItems(ui.Center).Gap(4).Children(func() {
					ui.Icon(c, icon).FontSize(28)
					muted(c, label, fontSmall)
					ui.Text(c, count).FontSize(13).FontWeight(600)
				})
			}
			side(icMonitor, L.ThisPC(), L.ReviewDeleted(d.Count))
			ui.Column(c).Grow(1).AlignItems(ui.Center).Gap(4).Children(func() {
				ui.Text(c, L.ReviewWouldDelete()).FontSize(fontSmall).FontWeight(600).TextColor(p.warn)
				ui.Row(c).FillWidth().Children(func() {
					ui.Box(c).Grow(1).Height(2).Stripes(p.warn, 6, 5, 0)
					ui.Icon(c, icChevron).FontSize(16).TextColor(p.warn).Rotate(-90)
				})
			})
			side(icCloud, acc.Server, L.ReviewStillThere(d.Count))
		})
		ui.Column(c).FillWidth().Padding(12, 20).Children(func() {
			muted(c, L.ReviewWhat(), fontSmall).FontWeight(600).Margin(0, 0, 6, 0)
			for i, g := range d.Groups {
				r := ui.Row(c).FillWidth().Gap(10).Padding(9, 0)
				if i < len(d.Groups)-1 {
					r.BorderWidth(0, 0, 1, 0).BorderColor(p.hover)
				}
				r.Children(func() {
					icon, name, what := icFolder, g.Name, L.ReviewGroup(g.Files, size(g.Bytes))
					if g.Loose {
						icon, name, what = icFile, L.ReviewLoose(g.Files, g.Name), size(g.Bytes)
					}
					ui.Icon(c, icon).FontSize(16)
					ui.Text(c, name).Grow(1)
					muted(c, what, 13)
				})
			}
		})
		ui.Row(c).FillWidth().Gap(10).Padding(14, 20).Background(p.subtle).BorderWidth(1, 0, 0, 0).BorderColor(p.hover).Children(func() {
			busy := a.busy["d-"+d.ID]
			if primary(c, "rv-restore", L.ActionRestorePC(), nil).Disabled(busy).Clicked() {
				a.answer(d, syncd.ActionRestore, L.ToastRestoring())
			}
			del := ui.ButtonBase(c.Key("rv-delete")).Height(32).Padding(0, 14).Radius(radiusCtl).Border(1, p.dangerBorder).Background(p.pane).Disabled(busy).Label(L.ActionDeleteServer(d.Count))
			del.Children(func() { ui.Text(c, L.ActionDeleteServer(d.Count)).FontWeight(600).TextColor(p.danger) })
			if del.Clicked() {
				a.ask(L.ConfirmDeleteTitle(d.Count), L.ConfirmDeleteBody(f.Name), L.ConfirmDelete(), func() {
					a.answer(d, syncd.ActionDeleteRemote, L.ToastDeleted())
				})
			}
			ui.Spacer(c)
			if link(c, "rv-later", L.ActionDecideLater()).Clicked() {
				a.page = pageOverview
			}
		})
	})
}

// answer resolves a decision once, bound to the proposal shown (its
// digest), and says what happened.
func (a *App) answer(d syncd.Decision, act syncd.Action, done string) {
	a.do("d-"+d.ID, func() error { return a.client.ResolveDecision(d.ID, d.Digest, act) }, func(err error) {
		if err != nil {
			a.setToast(errText(err))
			return
		}
		a.setToast(done)
	})
}

// recentList is the recent history, filtered.
func (a *App) recentList(c *ui.Context) {
	p := pal(c)
	s := a.snap
	ui.Box(c).Margin(0, 0, 4, 0).Children(func() { sectionTitle(c, L.Recent()) })
	var list []syncd.Activity
	for _, e := range s.Activity {
		isNet := e.Kind == syncd.ActSwitched
		if (a.filter == filterFiles && isNet) || (a.filter == filterNetwork && !isNet) {
			continue
		}
		list = append(list, e)
	}
	if len(list) == 0 {
		muted(c, L.NothingYet(), 13)
		return
	}
	now := time.Now()
	for i, e := range list {
		if i == 30 {
			break
		}
		f, _ := s.Folder(e.FolderID)
		icon, title, detail := icUp, "", f.Name
		switch e.Kind {
		case syncd.ActUploaded:
			title = L.ActUploaded(e.Name)
			if e.Name == "" {
				title = L.ActUploadedN(e.Count)
			}
		case syncd.ActDownloaded:
			icon, title = icDown, L.ActDownloaded(e.Name)
			if e.Name == "" {
				title = L.ActDownloadedN(e.Count)
			}
		case syncd.ActDeleted:
			icon, title, detail = icTrash, L.ActDeleted(e.Name), L.ActDeletedDetail(f.Name)
			if e.Name == "" {
				title = L.ActDeletedN(e.Count)
			}
		case syncd.ActSwitched:
			icon, title, detail = icGlobe, L.ActSwitched(e.Name), ""
		}
		r := ui.Row(c.Key(i)).FillWidth().Gap(12).Padding(10, 0).AlignItems(ui.Start)
		if i < len(list)-1 {
			r.BorderWidth(0, 0, 1, 0).BorderColor(p.hover)
		}
		r.Children(func() {
			ui.Icon(c, icon).FontSize(16).TextColor(p.muted)
			ui.Column(c).Grow(1).MinWidth(0).Children(func() {
				ui.Text(c, title).FontSize(13).Ellipsis("…").MaxLines(1)
				if detail != "" {
					muted(c, detail, fontSmall)
				}
			})
			muted(c, clock(e.At, now), fontSmall)
		})
	}
}
