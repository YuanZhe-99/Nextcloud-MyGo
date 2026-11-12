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
	waiting := waitingCount(s)
	pageHeader(c, L.NavActivity(), func() {
		ui.Segmented(c.Key("act-filter"), &a.filter, L.FilterAll(), L.FilterAttention(waiting), L.FilterFiles(), L.FilterNetwork())
	})
	// Every deletion review, one per folder that waits: several accounts
	// can each have one.
	var reviews []syncd.Decision
	for _, d := range s.Decisions {
		if d.Kind == syncd.DecisionBulkDeletions {
			reviews = append(reviews, d)
		}
	}
	pageBody(c, "act-body", func() {
		showReview := len(reviews) > 0 && (a.filter == filterAll || a.filter == filterAttention)
		grid := ui.Grid(c).Gap(20).FillWidth().AlignItems(ui.Start)
		if showReview {
			grid.ColumnTracks(ui.Fr(1.5), ui.Fr(1))
		} else {
			grid.ColumnTracks(ui.Fr(1))
		}
		grid.Children(func() {
			if showReview {
				ui.Column(c).Gap(16).MinWidth(0).Children(func() {
					for _, d := range reviews {
						a.reviewCard(c, d)
					}
				})
			}
			ui.Column(c).Gap(0).MinWidth(0).Children(func() {
				if a.filter == filterAll || a.filter == filterAttention {
					others := waiting - len(reviews)
					switch {
					case others > 0:
						ui.Box(c).Margin(0, 0, 8, 0).Children(func() { sectionTitle(c, L.AlsoNeedsAttention()) })
						ui.Column(c).Gap(10).Margin(0, 0, 18, 0).Children(func() { a.attentionItems(c, "act") })
					case len(reviews) == 0 && stateKnown(s):
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

// reviewCard is a deletion proposal: how many files, which way each
// deletion would go, what it covers when SyncD names it, and the answers:
// Decide later, the safe one, first; allowing asks a second time
// (PLAN.md §11.3, §17 item 6).
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
		// One row per direction: which side lost the files, and where the
		// deletion would be carried.
		direction := func(key string, from, to *ui.SVG, fromName, toName, count string) {
			ui.Row(c.Key(key)).FillWidth().Gap(14).Padding(16, 20).BorderWidth(0, 0, 1, 0).BorderColor(p.warnBorder).Children(func() {
				side := func(icon *ui.SVG, label string) {
					ui.Column(c).Width(130).Shrink(0).AlignItems(ui.Center).Gap(4).Children(func() {
						ui.Icon(c, icon).FontSize(28)
						muted(c, label, fontSmall).Ellipsis("…").MaxLines(1)
					})
				}
				side(from, fromName)
				ui.Column(c).Grow(1).AlignItems(ui.Center).Gap(4).Children(func() {
					ui.Text(c, count).FontSize(fontSmall).FontWeight(600).TextColor(p.warn).TextAlign(ui.Center).Wrap()
					ui.Row(c).FillWidth().Children(func() {
						ui.Box(c).Grow(1).Height(2).Stripes(p.warn, 6, 5, 0)
						ui.Icon(c, icChevron).FontSize(16).TextColor(p.warn).Rotate(-90)
					})
				})
				side(to, toName)
			})
		}
		if d.RemoteDeletes > 0 {
			direction("rv-remote", icMonitor, icCloud, L.ThisPC(), acc.Server, L.ReviewWouldDeleteServer(d.RemoteDeletes))
		}
		if d.LocalDeletes > 0 {
			direction("rv-local", icCloud, icMonitor, acc.Server, L.ThisPC(), L.ReviewWouldDeletePC(d.LocalDeletes))
		}
		ui.Column(c).FillWidth().Padding(12, 20).Gap(4).Children(func() {
			if len(d.Groups) > 0 {
				muted(c, L.ReviewWhat(), fontSmall).FontWeight(600).Margin(0, 0, 6, 0)
			}
			for i, g := range d.Groups {
				r := ui.Row(c).FillWidth().Gap(10).Padding(9, 0)
				if i < len(d.Groups)-1 {
					r.BorderWidth(0, 0, 1, 0).BorderColor(p.hover)
				}
				r.Children(func() {
					// SyncD names a group by a representative path and counts its
					// files; a size only when it sends one (1.9 does not).
					icon, name, what := icFolder, g.Name, L.ReviewFiles(g.Files)
					if g.Bytes > 0 {
						what = L.ReviewGroup(g.Files, size(g.Bytes))
					}
					if g.Loose {
						icon, name, what = icFile, L.ReviewLoose(g.Files, g.Name), ""
						if g.Bytes > 0 {
							what = size(g.Bytes)
						}
					}
					ui.Icon(c, icon).FontSize(16)
					ui.Text(c, name).Grow(1).MinWidth(0).Ellipsis("…").MaxLines(1)
					if what != "" {
						muted(c, what, 13).Shrink(0)
					}
				})
			}
			muted(c, L.ReviewKeep(), 13).Wrap()
			if !d.Expires.IsZero() {
				muted(c, L.ReviewExpires(d.Expires.Local().Format("15:04")), fontSmall).Wrap()
			}
		})
		ui.Row(c).FillWidth().Gap(10).Padding(14, 20).Background(p.subtle).BorderWidth(1, 0, 0, 0).BorderColor(p.hover).Children(func() {
			busy := a.busy["d-"+d.ID] || !a.snap.Daemon.Connected
			if primary(c, "rv-later-"+d.ID, L.ActionDecideLater(), nil).Disabled(busy).Clicked() {
				a.answer(d, syncd.ActionLater, "")
				a.page = pageOverview
			}
			if button(c, "rv-open-"+d.ID, L.ActionOpenFolder(), nil).Clicked() {
				a.openPath(f.LocalPath)
			}
			ui.Spacer(c)
			del := ui.ButtonBase(c.Key("rv-delete-"+d.ID)).Height(32).Padding(0, 14).Radius(radiusCtl).Border(1, p.dangerBorder).Background(p.pane).Disabled(busy).Label(L.ActionAllowDeletions(d.Count))
			del.Children(func() { ui.Text(c, L.ActionAllowDeletions(d.Count)).FontWeight(600).TextColor(p.danger) })
			if del.Clicked() {
				a.ask(L.ConfirmDeleteTitle(d.Count), L.ConfirmDeleteBody(f.Name), L.ConfirmDelete(), func() {
					a.answer(d, syncd.ActionApproveDeletion, L.ToastAllowed())
				})
			}
		})
	})
}

// answer resolves a decision once, bound to the proposal shown (its
// digest), and says what happened; accepted is not propagated: the
// folder's state shows the outcome.
func (a *App) answer(d syncd.Decision, act syncd.Action, done string) {
	a.do("d-"+d.ID, func() error { return a.client.ResolveDecision(d.ID, d.Digest, act) }, func(err error) {
		if err != nil {
			a.setToast(errText(err))
			return
		}
		if done != "" {
			a.setToast(done)
		}
	})
}

// recentList is the recent history, filtered.
func (a *App) recentList(c *ui.Context) {
	p := pal(c)
	s := a.snap
	ui.Box(c).Margin(0, 0, 4, 0).Children(func() { sectionTitle(c, L.Recent()) })
	var list []syncd.Activity
	for _, e := range s.Activity {
		isNet := e.Kind == syncd.ActSwitched || e.Kind == syncd.ActSwitchFailed
		if (a.filter == filterFiles && isNet) || (a.filter == filterNetwork && !isNet) {
			continue
		}
		list = append(list, e)
	}
	if len(list) == 0 {
		if stateKnown(s) {
			muted(c, L.NothingYet(), 13)
		}
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
		case syncd.ActDeletedLocal:
			icon, title, detail = icTrash, L.ActDeletedLocalN(e.Count), L.ActDeletedLocalDetail(f.Name)
		case syncd.ActRenamedRemote:
			icon, title, detail = icFile, L.ActRenamedRemoteN(e.Count), L.ActRenamedRemoteDetail(f.Name)
		case syncd.ActRenamedLocal:
			icon, title, detail = icFile, L.ActRenamedLocalN(e.Count), L.ActRenamedLocalDetail(f.Name)
		case syncd.ActSwitched:
			icon, title, detail = icGlobe, L.ActSwitched(e.Name), ""
		case syncd.ActSwitchFailed:
			icon, title, detail = icWarn, switchFailed(s, e), ""
		}
		r := ui.Row(c.Key(i)).FillWidth().Gap(12).Padding(10, 0).AlignItems(ui.Start)
		if i < len(list)-1 {
			r.BorderWidth(0, 0, 1, 0).BorderColor(p.hover)
		}
		r.Children(func() {
			ui.Icon(c, icon).FontSize(16).TextColor(p.muted)
			ui.Column(c).Grow(1).MinWidth(0).Children(func() {
				if e.Kind == syncd.ActSwitchFailed {
					// The reason is the point of the entry; it is never cut off.
					ui.Text(c, title).FontSize(13).Wrap()
				} else {
					ui.Text(c, title).FontSize(13).Ellipsis("…").MaxLines(1)
				}
				if detail != "" {
					muted(c, detail, fontSmall)
				}
			})
			muted(c, clock(e.At, now), fontSmall)
		})
	}
}
