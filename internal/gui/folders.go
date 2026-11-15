package gui

import (
	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// The columns of a folder's row: name, paths, state, buttons.
var folderTracks = []ui.Track{ui.Fr(1.2), ui.Fr(1.5), ui.Fr(1.1), ui.Fixed(210)}

// foldersPage is each account with its folders (design/Folders.html).
func (a *App) foldersPage(c *ui.Context) {
	s := a.snap
	pageHeader(c, L.NavFolders(), func() {
		b := button(c, "fo-add-account", L.ActionAddAccount(), icPlus).Disabled(!a.canAddAccount())
		if !a.canAddAccount() {
			b.Tooltip(L.OneAccountOnly())
		}
		if b.Clicked() {
			a.startSetup()
		}
	})
	pageBody(c, "fo-body", func() {
		for _, acc := range s.Accounts {
			a.accountSection(c, acc)
		}
		ui.Row(c).Gap(8).Children(func() {
			ui.Icon(c, icInfo).FontSize(14).TextColor(pal(c).muted)
			muted(c, L.RemoveNote(), fontSmall)
		})
	})
}

// accountSection is an account's header and its folders' rows.
func (a *App) accountSection(c *ui.Context, acc syncd.Account) {
	p := pal(c)
	s := a.snap
	approved := 0
	for _, e := range s.Endpoints {
		if e.AccountID == acc.ID && e.Approved && e.Verified == syncd.VerifyVerified {
			approved++
		}
	}
	signIn := needsSignIn(s, acc)
	live := s.Daemon.Connected
	card(c.Key("acc-" + acc.ID)).FillWidth().Clip().Children(func() {
		ui.Row(c).FillWidth().Gap(12).Padding(14, 16).Background(p.subtle).BorderWidth(0, 0, 1, 0).BorderColor(p.border).Children(func() {
			avatar(c, acc, 36)
			ui.Column(c).Grow(1).MinWidth(0).Children(func() {
				ui.Text(c, accountName(acc)).FontSize(fontSection).FontWeight(600).Level(2)
				if !signIn {
					muted(c, L.AccountSignedIn(approved), fontSmall)
				} else {
					state, _ := authWords(acc)
					ui.Text(c, state).FontSize(fontSmall).FontWeight(600).TextColor(p.warn).Wrap()
				}
			})
			if signIn {
				a.signInButtons(c, "acc", acc.ID)
			} else if button(c, "acc-add-"+acc.ID, L.ActionAddFolder(), icPlus).Disabled(!live).Clicked() {
				a.startAddFolders(acc.ID)
			}
			more := button(c, "acc-more-"+acc.ID, "", icMore).Label(L.ActionMore()).Tooltip(L.ActionMore())
			more.PopoverMenuButton(func(m *ui.Menu) {
				_, renewing := a.renewing[acc.ID]
				if m.Item(L.MenuSignInAgain()).Disabled(!live || renewing || a.busy["login-"+acc.ID]).Chosen() {
					a.reauthenticate(acc.ID)
				}
				m.Separator()
				if m.Item(L.MenuRemoveAccount()).Danger().Disabled(!live || a.busy["acc-rm-"+acc.ID]).Chosen() {
					a.ask(L.ConfirmRemoveAccountTitle(accountName(acc)), L.ConfirmRemoveAccountBody(), L.ConfirmRemoveAccount(), func() {
						a.removeAccount(acc.ID)
					})
				}
			})
		})
		ui.Grid(c).ColumnTracks(folderTracks...).FillWidth().Padding(8, 16).GapX(12).BorderWidth(0, 0, 1, 0).BorderColor(p.hover).Children(func() {
			for _, h := range []string{L.ColFolder(), L.ColPaths(), L.ColStatus(), ""} {
				muted(c, h, fontSmall).FontWeight(600)
			}
		})
		n := 0
		for _, f := range s.Folders {
			if f.AccountID == acc.ID {
				a.folderRow(c, f, n > 0)
				n++
			}
		}
		if n == 0 && stateKnown(s) {
			ui.Box(c).Padding(14, 16).Children(func() { muted(c, L.NoFolders(), 13) })
		}
	})
}

// removeAccount removes the account with its folders, as its question
// said: SyncD stops their syncing itself, waiting for a running cycle, and
// every file stays on both sides (protocol 1.9).
func (a *App) removeAccount(accountID string) {
	a.do("acc-rm-"+accountID, func() error { return a.client.RemoveAccount(accountID) }, nil)
}

// canAddAccount tells whether another account may be added: always the
// first, a further one only where SyncD supports several (multiAccount).
func (a *App) canAddAccount() bool {
	return len(a.snap.Accounts) == 0 || a.snap.Daemon.Capabilities["multiAccount"]
}

// folderRow is a folder: where it syncs, its state, and its buttons.
func (a *App) folderRow(c *ui.Context, f syncd.Folder, rule bool) {
	p := pal(c)
	word, col, hollow := folderWord(f, a.snap, p)
	detail := folderDetail(f, a.snap)
	note, closable := conversionLine(f, a.prefs.ConversionNotesClosed)
	box := ui.Column(c.Key("fr-"+f.ID)).FillWidth().Padding(12, 16).Gap(8)
	if rule {
		box.BorderWidth(1, 0, 0, 0).BorderColor(p.hover)
	}
	box.Children(func() {
		a.folderGrid(c, f, word, detail, col, hollow)
		if note == "" {
			return
		}
		ui.Row(c).FillWidth().Gap(8).AlignItems(ui.Center).Children(func() {
			ui.Icon(c, icInfo).FontSize(14).TextColor(p.muted)
			muted(c, note, fontSmall).Wrap().Grow(1).MinWidth(0)
			if !closable {
				return
			}
			b := ui.ButtonBase(c.Key("fr-note-close-"+f.ID)).Size(28, 28).Radius(radiusCtl).Center().Label(L.ActionDismiss()).Tooltip(L.ActionDismiss())
			if b.Hovered() {
				b.Background(p.hover)
			}
			b.Children(func() { ui.Icon(c, icClose).FontSize(14) })
			if b.Clicked() {
				a.closeConversionNote(f.ID)
			}
		})
	})
}

// closeConversionNote keeps a converted folder's note closed in gui.json,
// forgetting folders no longer in SyncD's list.
func (a *App) closeConversionNote(folderID string) {
	pr := a.prefs
	closed := []string{folderID}
	for _, id := range pr.ConversionNotesClosed {
		if _, ok := a.snap.Folder(id); ok && id != folderID {
			closed = append(closed, id)
		}
	}
	pr.ConversionNotesClosed = closed
	a.setPrefs(pr)
}

// folderGrid is a folder row's columns: name and mode, paths, state, and
// buttons.
func (a *App) folderGrid(c *ui.Context, f syncd.Folder, word, detail string, col ui.Color, hollow bool) {
	p := pal(c)
	ui.Grid(c).ColumnTracks(folderTracks...).FillWidth().GapX(12).AlignItems(ui.Center).Children(func() {
		ui.Column(c).Gap(2).MinWidth(0).Children(func() {
			ui.Text(c, f.Name).FontWeight(600).Ellipsis("…").MaxLines(1)
			if modeShown(a.snap, f) {
				muted(c, modeWord(f), fontSmall).Ellipsis("…").MaxLines(1)
			}
		})
		muted(c, f.LocalPath+"  ⇄  "+f.RemotePath, 13).Ellipsis("…").MaxLines(1)
		ui.Column(c).Gap(2).Children(func() {
			ui.Row(c).Gap(6).Children(func() {
				dot(c, col, hollow)
				ui.Text(c, word).FontSize(13).FontWeight(600).TextColor(col)
			})
			if detail != "" {
				muted(c, detail, fontSmall).Wrap()
			}
		})
		ui.Row(c).Gap(8).Justify(ui.End).Children(func() {
			busy := a.busy["f-"+f.ID] || !a.snap.Daemon.Connected
			if button(c, "fr-open-"+f.ID, L.ActionOpen(), nil).Clicked() {
				a.openPath(f.LocalPath)
			}
			switch {
			case f.State == syncd.FolderNeedsReview:
				if filled(c, "fr-review-"+f.ID, L.ActionReview(), nil, p.warn, p.onWarn).Clicked() {
					a.page = pageActivity
				}
			case f.UserPaused:
				if button(c, "fr-resume-"+f.ID, L.ActionResume(), nil).Disabled(busy).Clicked() {
					a.do("f-"+f.ID, func() error { return a.client.ResumeFolder(f.ID) }, nil)
				}
			case f.State != syncd.FolderWaitingSignIn:
				if button(c, "fr-pause-"+f.ID, L.ActionPause(), nil).Disabled(busy).Clicked() {
					a.do("f-"+f.ID, func() error { return a.client.PauseFolder(f.ID) }, nil)
				}
			}
			button(c, "fr-more-"+f.ID, "", icMore).Label(L.ActionMore()).Tooltip(L.ActionMore()).PopoverMenuButton(func(m *ui.Menu) {
				canCheck := !f.UserPaused && !a.snap.Daemon.AllPaused && f.State != syncd.FolderNeedsReview && f.State != syncd.FolderWaitingSignIn
				if m.Item(L.ActionCheckNow()).Disabled(busy || !canCheck).Chosen() {
					a.do("f-"+f.ID, func() error { return a.client.SyncNow(f.ID) }, nil)
				}
				a.availabilityItems(m, f, busy)
				m.Separator()
				if m.Item(L.MenuStopFolder(f.Name)).Danger().Disabled(busy).Chosen() {
					a.ask(L.ConfirmStopFolderTitle(f.Name), L.ConfirmStopFolderBody(), L.ConfirmStopFolder(), func() {
						a.do("f-"+f.ID, func() error { return a.client.RemoveFolder(f.ID) }, nil)
					})
				}
			})
		})
	})
}

// availabilityItems are a folder menu's choices of how its files stay on
// this PC (protocol 1.12): for an on-demand folder, always here, on demand
// or freed (asked first); for a full-file folder where on-demand files
// exist, switching to them in place (asked first). Nothing from an earlier
// SyncD, which cannot do it.
func (a *App) availabilityItems(m *ui.Menu, f syncd.Folder, busy bool) {
	if a.snap.Daemon.ProtocolMinor < 12 {
		return
	}
	set := func(v syncd.Availability) {
		a.do("f-"+f.ID, func() error { return a.client.SetAvailability(f.ID, v) }, nil)
	}
	switch {
	case f.Mode == syncd.FolderOnDemand:
		m.Separator()
		for _, o := range []struct {
			v          syncd.Availability
			word, hint string
		}{
			{syncd.AvailabilityAlwaysLocal, L.AvailAlwaysLocal(), L.AvailAlwaysLocalHint()},
			{syncd.AvailabilityOnDemand, L.ModeOnDemand(), L.AvailOnDemandHint()},
			{syncd.AvailabilityOnlineOnly, L.AvailOnlineOnly(), L.AvailOnlineOnlyHint()},
		} {
			if !m.Item(o.word).Subtitle(o.hint).Radio(f.Availability == o.v).Disabled(busy).Chosen() || f.Availability == o.v {
				continue
			}
			if o.v == syncd.AvailabilityOnlineOnly {
				a.ask(L.ConfirmFreeUpTitle(f.Name), L.ConfirmFreeUpBody(), L.AvailOnlineOnly(), func() { set(o.v) })
			} else {
				set(o.v)
			}
		}
	case a.snap.Daemon.Capabilities["virtualFiles"] && f.Conversion != syncd.ConversionPending:
		m.Separator()
		if m.Item(L.MenuUseOnDemand()).Subtitle(L.AvailOnDemandHint()).Disabled(busy).Chosen() {
			a.ask(L.ConfirmUseOnDemandTitle(f.Name), L.ConfirmUseOnDemandBody(), L.MenuUseOnDemand(), func() { set(syncd.AvailabilityOnDemand) })
		}
	}
}

// confirmState is a question waiting for the user's answer.
type confirmState struct {
	open                 bool
	title, body, confirm string
	then                 func()
}

// ask shows a question with a button that does then, and Cancel.
func (a *App) ask(title, body, confirm string, then func()) {
	a.confirm = confirmState{open: true, title: title, body: body, confirm: confirm, then: then}
}

// confirmDialog is the question ask shows.
func (a *App) confirmDialog(c *ui.Context) {
	if !a.confirm.open {
		return
	}
	if ui.AlertDialog(c, &a.confirm.open, a.confirm.title, a.confirm.body, L.ActionCancel(), a.confirm.confirm) == 1 {
		then := a.confirm.then
		a.confirm = confirmState{}
		then()
	}
}
