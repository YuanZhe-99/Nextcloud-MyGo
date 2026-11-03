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
		if button(c, "fo-add-account", L.ActionAddAccount(), icPlus).Clicked() {
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
		if e.AccountID == acc.ID && e.Approved && !e.Blocked {
			approved++
		}
	}
	card(c.Key("acc-" + acc.ID)).FillWidth().Clip().Children(func() {
		ui.Row(c).FillWidth().Gap(12).Padding(14, 16).Background(p.subtle).BorderWidth(0, 0, 1, 0).BorderColor(p.border).Children(func() {
			avatar(c, acc, 36)
			ui.Column(c).Grow(1).MinWidth(0).Children(func() {
				ui.Text(c, accountName(acc)).FontSize(fontSection).FontWeight(600).Level(2)
				if acc.SignedIn {
					muted(c, L.AccountSignedIn(approved), fontSmall)
				} else {
					ui.Text(c, L.AccountSignedOut()).FontSize(fontSmall).FontWeight(600).TextColor(p.warn)
				}
			})
			if acc.SignedIn {
				if button(c, "acc-add-"+acc.ID, L.ActionAddFolder(), icPlus).Clicked() {
					a.startAddFolders(acc.ID)
				}
			} else if primary(c, "acc-signin-"+acc.ID, L.ActionSignInBrowser(), nil).Disabled(a.busy["login-"+acc.ID]).Clicked() {
				a.reauthenticate(acc.ID)
			}
			more := button(c, "acc-more-"+acc.ID, "", icMore).Label(L.ActionMore()).Tooltip(L.ActionMore())
			more.PopoverMenuButton(func(m *ui.Menu) {
				if m.Item(L.MenuRemoveAccount()).Chosen() {
					a.ask(L.ConfirmRemoveAccountTitle(accountName(acc)), L.ConfirmRemoveAccountBody(), L.ConfirmRemoveAccount(), func() {
						a.do("acc-rm-"+acc.ID, func() error { return a.client.RemoveAccount(acc.ID) }, nil)
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
		if n == 0 {
			ui.Box(c).Padding(14, 16).Children(func() { muted(c, L.NoFolders(), 13) })
		}
	})
}

// folderRow is a folder: where it syncs, its state, and its buttons.
func (a *App) folderRow(c *ui.Context, f syncd.Folder, rule bool) {
	p := pal(c)
	word, col, hollow := folderWord(f, p)
	detail := folderDetail(f, a.snap)
	row := ui.Grid(c.Key("fr-"+f.ID)).ColumnTracks(folderTracks...).FillWidth().Padding(12, 16).GapX(12).AlignItems(ui.Center)
	if rule {
		row.BorderWidth(1, 0, 0, 0).BorderColor(p.hover)
	}
	row.Children(func() {
		ui.Text(c, f.Name).FontWeight(600).Ellipsis("…").MaxLines(1)
		muted(c, f.LocalPath+"  ⇄  "+f.RemotePath, 13).Ellipsis("…").MaxLines(1)
		ui.Column(c).Gap(2).Children(func() {
			ui.Row(c).Gap(6).Children(func() {
				dot(c, col, hollow)
				ui.Text(c, word).FontSize(13).FontWeight(600).TextColor(col)
			})
			if detail != "" {
				muted(c, detail, fontSmall)
			}
		})
		ui.Row(c).Gap(8).Justify(ui.End).Children(func() {
			if button(c, "fr-open-"+f.ID, L.ActionOpen(), nil).Clicked() {
				a.openPath(f.LocalPath)
			}
			switch {
			case f.State == syncd.FolderNeedsReview:
				if filled(c, "fr-review-"+f.ID, L.ActionReview(), nil, p.warn, p.onWarn).Clicked() {
					a.page = pageActivity
				}
			case f.UserPaused:
				if button(c, "fr-resume-"+f.ID, L.ActionResume(), nil).Disabled(a.busy["f-"+f.ID]).Clicked() {
					a.do("f-"+f.ID, func() error { return a.client.ResumeFolder(f.ID) }, nil)
				}
			case f.State != syncd.FolderWaitingSignIn:
				if button(c, "fr-pause-"+f.ID, L.ActionPause(), nil).Disabled(a.busy["f-"+f.ID]).Clicked() {
					a.do("f-"+f.ID, func() error { return a.client.PauseFolder(f.ID) }, nil)
				}
			}
			button(c, "fr-more-"+f.ID, "", icMore).Label(L.ActionMore()).Tooltip(L.ActionMore()).PopoverMenuButton(func(m *ui.Menu) {
				if m.Item(L.MenuStopFolder(f.Name)).Chosen() {
					a.ask(L.ConfirmStopFolderTitle(f.Name), L.ConfirmStopFolderBody(), L.ConfirmStopFolder(), func() {
						a.do("f-"+f.ID, func() error { return a.client.RemoveFolder(f.ID) }, nil)
					})
				}
			})
		})
	})
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
