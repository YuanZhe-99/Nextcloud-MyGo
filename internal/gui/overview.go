package gui

import (
	"time"

	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// overviewPage is the sync state, the address in use, what needs the
// user, and the folders (design/Main.html).
func (a *App) overviewPage(c *ui.Context) {
	s := a.snap
	pageHeader(c, L.NavOverview(), func() {
		if s.Daemon.AllPaused {
			if button(c, "ov-resume", L.ActionResumeSync(), icPlay).Clicked() {
				a.do("all", a.client.ResumeAll, nil)
			}
		} else if button(c, "ov-pause", L.ActionPauseSync(), icPause).Clicked() {
			a.do("all", a.client.PauseAll, nil)
		}
		if primary(c, "ov-check", L.ActionCheckNow(), icRefresh).Disabled(s.Daemon.AllPaused).Clicked() {
			a.checkAll()
		}
	})
	pageBody(c, "ov-body", func() {
		ui.Grid(c).Columns(2).ColumnTracks(ui.Fr(1.4), ui.Fr(1)).Gap(16).FillWidth().Children(func() {
			a.heroCard(c)
			a.throughCard(c)
		})
		a.attentionList(c)
		a.folderCards(c)
	})
}

// checkAll asks every folder that can sync to check now.
func (a *App) checkAll() {
	ids := []string{}
	for _, f := range a.snap.Folders {
		if f.State == syncd.FolderUpToDate || f.State == syncd.FolderOffline || f.State == syncd.FolderError {
			ids = append(ids, f.ID)
		}
	}
	a.do("check", func() error {
		for _, id := range ids {
			if err := a.client.SyncNow(id); err != nil {
				return err
			}
		}
		return nil
	}, nil)
}

// heroCard is what sync does now, with the phase rather than an invented
// total (PLAN.md §17 item 9).
func (a *App) heroCard(c *ui.Context) {
	p := pal(c)
	s := a.snap
	o := overallOf(s)
	var title, detail string
	var moving *syncd.Folder
	for i := range s.Folders {
		if s.Folders[i].State == syncd.FolderSyncing {
			moving = &s.Folders[i]
			break
		}
	}
	switch {
	case o == overallPaused:
		title, detail = L.OverviewPaused(), L.DetailAllPaused()
	case moving != nil:
		title = L.OverviewSyncing()
		switch moving.Phase {
		case syncd.PhaseUploading:
			detail = L.DetailUploading(moving.Name, moving.FilesDone)
		case syncd.PhaseDownloading:
			detail = L.DetailDownloading(moving.Name, moving.FilesDone)
		default:
			detail = L.DetailChecking(moving.Name)
		}
	case o == overallNeedsYou:
		title, detail = L.OverviewNeedsYou(), L.DetailWaiting()
	default:
		title, detail = L.OverviewUpToDate(), L.DetailAllUpToDate()
	}
	icon, iconBg, iconFg := icCloudUp, p.tint, p.accentText
	// The icon follows the title: what moves wins over what waits.
	switch {
	case o == overallPaused:
		icon, iconBg, iconFg = icPause, p.hover, p.muted
	case moving != nil:
	case o == overallNeedsYou:
		icon, iconBg, iconFg = icWarn, p.warnBg, p.warn
	default:
		icon = icCheck
	}
	card(c).Padding(20).Children(func() {
		ui.Row(c).Gap(16).AlignItems(ui.Start).Children(func() {
			ui.Box(c).Size(44, 44).Shrink(0).Radius(22).Background(iconBg).Center().Children(func() {
				ui.Icon(c, icon).FontSize(22).TextColor(iconFg)
			})
			ui.Column(c).Grow(1).MinWidth(0).Gap(6).Children(func() {
				ui.Text(c, title).FontSize(20).FontWeight(600)
				muted(c, detail, fontBody)
				if moving != nil {
					// Activity of unknown length: no total exists yet.
					ui.Progress(c, -1).FillWidth().Height(4).Margin(6, 0, 0, 0)
				}
				if !s.Daemon.CheckedAt.IsZero() {
					muted(c, L.DetailLastCheck(ago(s.Daemon.CheckedAt, time.Now())), fontSmall)
				}
			})
		})
	})
}

// throughCard is the address the first account uses and why.
func (a *App) throughCard(c *ui.Context) {
	p := pal(c)
	s := a.snap
	if len(s.Accounts) == 0 {
		return
	}
	e, n, ok := activeEndpoint(s, s.Accounts[0].ID)
	b := ui.ButtonBase(c.Key("ov-through")).Label(L.ConnectedThrough()).Border(1, p.border).Radius(radiusCard).Padding(20)
	if b.Hovered() {
		b.Background(p.subtle)
	}
	b.Children(func() {
		ui.Column(c).FillWidth().Gap(8).Children(func() {
			ui.Row(c).Gap(8).Children(func() {
				muted(c, L.ConnectedThrough(), fontSmall).FontWeight(600)
				word := L.PolicyAuto()
				if n.Policy == syncd.PolicyFixed {
					word = L.PolicyFixed()
				}
				pill(c, word, p.tint, p.accentText)
			})
			if !ok {
				muted(c, L.StateOffline(), fontBody)
				return
			}
			ui.Row(c).Gap(10).Children(func() {
				ui.Icon(c, kindIcon(e.Kind)).FontSize(20).TextColor(p.success)
				ui.Text(c, e.Name).FontSize(18).FontWeight(600)
			})
			muted(c, L.VerifiedAs(hostOf(e.URL)), 13)
			why := L.OnlyAddress()
			if !n.Switched.IsZero() {
				why = L.SwitchedAgo(ago(n.Switched, time.Now()), reasonSentence(n.SwitchReason))
			}
			ui.Box(c).FillWidth().Padding(8, 10).Radius(radiusCtl).Background(p.subtle).Children(func() {
				ui.Text(c, why).FontSize(13).Wrap()
			})
		})
	})
	if b.Clicked() {
		a.page, a.account = pageNetwork, s.Accounts[0].ID
	}
}

// attentionList is every decision and signed-out account, each with the
// one button that deals with it.
func (a *App) attentionList(c *ui.Context) {
	s := a.snap
	n := len(s.Decisions)
	for _, acc := range s.Accounts {
		if !acc.SignedIn {
			n++
		}
	}
	if n == 0 {
		return
	}
	ui.Column(c).FillWidth().Gap(10).Children(func() {
		sectionTitle(c, L.NeedsAttention())
		a.attentionItems(c, "ov")
	})
}

// attentionItems are the cards of what needs the user, shared by
// Overview and Activity.
func (a *App) attentionItems(c *ui.Context, key string) {
	p := pal(c)
	s := a.snap
	for _, d := range s.Decisions {
		f, _ := s.Folder(d.FolderID)
		switch d.Kind {
		case syncd.DecisionLocalDeletions:
			if key == "act" {
				continue // Activity shows it in full
			}
			notice(c, key == "act", icWarn, p.warnBg, p.warnBorder, p.warn, L.AttnDeletionsTitle(d.Count, f.Name), L.AttnDeletionsBody(), func() {
				if filled(c, key+"-review-"+d.ID, L.ActionReview(), nil, p.warn, p.onWarn).Clicked() {
					a.page = pageActivity
				}
			})
		case syncd.DecisionConflict:
			notice(c, key == "act", icFiles, p.subtle, p.border, p.accentText, L.AttnConflictTitle(d.File), L.AttnConflictBody(d.Copy, f.Name), func() {
				if button(c, key+"-show-"+d.ID, L.ActionShowInFolder(), nil).Clicked() {
					a.openPath(f.LocalPath)
				}
				if button(c, key+"-resolve-"+d.ID, L.ActionMarkResolved(), nil).Clicked() {
					a.do("d-"+d.ID, func() error { return a.client.ResolveDecision(d.ID, d.Digest, syncd.ActionResolved) }, nil)
				}
			})
		}
	}
	for _, acc := range s.Accounts {
		if acc.SignedIn {
			continue
		}
		notice(c, key == "act", icLock, p.subtle, p.border, p.muted, L.AttnSignInTitle(accountName(acc)), L.AttnSignInBody(), func() {
			if button(c, key+"-signin-"+acc.ID, L.ActionSignInBrowser(), nil).Disabled(a.busy["login-"+acc.ID]).Clicked() {
				a.reauthenticate(acc.ID)
			}
		})
	}
}

// notice is a card of something that needs the user: an icon, a title and
// a sentence, and its buttons at the end, or under the words when stacked
// (a narrow column).
func notice(c *ui.Context, stacked bool, icon *ui.SVG, bg, border, tone ui.Color, title, body string, actions func()) {
	ui.Row(c).FillWidth().Gap(14).Padding(14, 16).Radius(radiusCard).Background(bg).Border(1, border).AlignItems(ui.Start).Children(func() {
		ui.Icon(c, icon).FontSize(20).TextColor(tone)
		ui.Column(c).Grow(1).MinWidth(0).Gap(2).Children(func() {
			ui.Text(c, title).FontWeight(600).Wrap()
			muted(c, body, 13).Wrap()
			if stacked {
				ui.Row(c).Gap(8).Wrap().Margin(8, 0, 0, 0).Children(actions)
			}
		})
		if !stacked {
			ui.Row(c).Gap(8).Shrink(0).AlignSelf(ui.Center).Children(actions)
		}
	})
}

// reauthenticate signs an account in again through the browser, keeping
// its identity (accounts.reauthenticate).
func (a *App) reauthenticate(accountID string) {
	key := "login-" + accountID
	a.do(key, func() error {
		l, err := a.client.Reauthenticate(accountID)
		if err != nil {
			return err
		}
		a.later(func() { a.openBrowser(l.LoginURL) })
		_, err = a.client.WaitLogin(a.ctx, l.ID)
		return err
	}, func(err error) {
		if err != nil {
			a.setToast(L.SignInFailed())
		}
	})
}

// folderCards are the folders, four to a row.
func (a *App) folderCards(c *ui.Context) {
	p := pal(c)
	s := a.snap
	ui.Column(c).FillWidth().Gap(10).Children(func() {
		ui.Row(c).FillWidth().Children(func() {
			sectionTitle(c, L.FoldersTitle())
			ui.Spacer(c)
			if link(c, "ov-manage", L.ActionManageFolders()).Clicked() {
				a.page = pageFolders
			}
		})
		if len(s.Folders) == 0 {
			muted(c, L.NoFolders(), fontBody)
			return
		}
		ui.Grid(c).Columns(4).Gap(12).FillWidth().Children(func() {
			for _, f := range s.Folders {
				acc, _ := s.Account(f.AccountID)
				word, col, hollow := folderWord(f, p)
				border, bg := p.border, p.pane
				if f.State == syncd.FolderNeedsReview {
					border, bg = p.warnBorder, p.warnBg
				}
				ui.Column(c.Key("ov-f-"+f.ID)).Border(1, border).Background(bg).Radius(radiusCard).Padding(14).Gap(6).MinWidth(0).Children(func() {
					ui.Row(c).Gap(8).Children(func() {
						ui.Icon(c, icFolder).FontSize(16)
						ui.Text(c, f.Name).FontWeight(600).Ellipsis("…").MaxLines(1)
					})
					muted(c, accountName(acc), fontSmall).Ellipsis("…").MaxLines(1)
					ui.Row(c).Gap(6).Children(func() {
						dot(c, col, hollow)
						ui.Text(c, word).FontSize(13).FontWeight(600).TextColor(col)
					})
				})
			}
		})
	})
}
