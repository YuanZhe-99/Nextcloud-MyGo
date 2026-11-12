package gui

import (
	"cmp"
	"slices"
	"strconv"
	"time"

	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// networkPage is an account's addresses with their two trust steps, why
// the one in use was chosen, and the recent switches
// (design/Network.html).
func (a *App) networkPage(c *ui.Context) {
	s := a.snap
	n, _ := s.Network(a.account)
	auto := s.Daemon.Capabilities["endpointAutoSwitch"]
	live := s.Daemon.Connected
	pageHeader(c, L.NavNetwork(), func() {
		// Automatic choice is offered only when SyncD supports it; an
		// account already on it can still leave it (never presented as
		// supported otherwise).
		if auto || n.Policy == syncd.PolicyAuto {
			mode := 0
			if n.Policy != syncd.PolicyAuto {
				mode = 1
			}
			seg := ui.Segmented(c.Key("net-mode"), &mode, L.NetworkModeAuto(), L.NetworkModeFixed()).Label(L.NetworkHowChosen()).Disabled(!live || a.busy["policy"] || a.account == "")
			if seg.Changed() && (mode == 1 || auto) {
				pol, fixed := syncd.PolicyAuto, ""
				if mode == 1 {
					pol, fixed = syncd.PolicyFixed, n.ActiveID
					if fixed == "" {
						fixed = mainEndpoint(s, a.account)
					}
				}
				a.requestPolicy(a.account, pol, fixed)
			}
		}
		if primary(c, "net-add", L.ActionAddAddress(), icPlus).Disabled(a.account == "" || !live).Clicked() {
			a.addr = addrForm{open: true, accountID: a.account}
		}
	})
	if len(s.Accounts) > 1 {
		a.accountTabs(c)
	}
	pageBody(c, "net-body", func() {
		if !auto && s.Daemon.Connected {
			pl := pal(c)
			ui.Row(c).FillWidth().Gap(10).Padding(10, 14).Radius(radiusCard).Background(pl.subtle).Children(func() {
				ui.Icon(c, icInfo).FontSize(16).TextColor(pl.muted)
				muted(c, L.AutoUnavailable(), 13).Wrap().Grow(1).MinWidth(0)
			})
		}
		ui.Grid(c).ColumnTracks(ui.Fr(1.65), ui.Fr(1)).Gap(20).FillWidth().AlignItems(ui.Start).Children(func() {
			ui.Column(c).Gap(10).MinWidth(0).Children(func() {
				sectionTitle(c, L.AddressesOf())
				for _, e := range sortedEndpoints(s, a.account) {
					a.endpointCard(c, e, n)
				}
			})
			ui.Column(c).Gap(20).MinWidth(0).Children(func() {
				a.whyCard(c, n)
				a.switchList(c)
			})
		})
	})
}

// mainEndpoint is the ID of an account's main address, or "".
func mainEndpoint(s syncd.Snapshot, accountID string) string {
	for _, e := range s.Endpoints {
		if e.AccountID == accountID && e.Main {
			return e.ID
		}
	}
	return ""
}

// requestPolicy asks SyncD for a policy; accepted is not done: the
// address changes at a safe point, shown as pending meanwhile.
func (a *App) requestPolicy(accountID string, pol syncd.Policy, fixed string) {
	a.do("policy", func() error {
		_, err := a.client.RequestPolicy(accountID, pol, fixed)
		return err
	}, nil)
}

// accountTabs chooses the account whose addresses the page shows.
func (a *App) accountTabs(c *ui.Context) {
	p := pal(c)
	ui.Row(c).FillWidth().Gap(20).Padding(0, 28).BorderWidth(0, 0, 1, 0).BorderColor(p.hover).Role(ui.RoleTabList).Children(func() {
		for _, acc := range a.snap.Accounts {
			on := acc.ID == a.account
			b := ui.ButtonBase(c.Key("net-tab-"+acc.ID)).Height(36).Padding(0, 2).Role(ui.RoleTab).Label(accountName(acc))
			if on {
				b.BorderWidth(0, 0, 2, 0).BorderColor(p.accent)
			}
			b.Children(func() {
				t := ui.Text(c, accountName(acc))
				if on {
					t.FontWeight(600)
				} else {
					t.TextColor(p.muted)
				}
			})
			if b.Clicked() {
				a.account = acc.ID
			}
		}
	})
}

// sortedEndpoints puts the address in use first, then the main one, then
// the others, and those that failed verification last.
func sortedEndpoints(s syncd.Snapshot, accountID string) []syncd.Endpoint {
	var out []syncd.Endpoint
	for _, e := range s.Endpoints {
		if e.AccountID == accountID {
			out = append(out, e)
		}
	}
	rank := func(e syncd.Endpoint) int {
		switch {
		case e.InUse:
			return 0
		case e.Verified == syncd.VerifyFailed:
			return 3
		case e.Main:
			return 1
		}
		return 2
	}
	slices.SortStableFunc(out, func(x, y syncd.Endpoint) int { return cmp.Compare(rank(x), rank(y)) })
	return out
}

// endpointCard is an address: its name, where it leads, its URL, the two
// trust steps, why it is blocked, and how fast it answers. A security
// block is a hard one no score overrides (INV-04); an address not usable
// for now (unreachable, waiting for a sign-in) only says so.
func (a *App) endpointCard(c *ui.Context, e syncd.Endpoint, n syncd.Network) {
	p := pal(c)
	live := a.snap.Daemon.Connected
	failed := quarantined(e)
	blocked := blockedWords(e)
	usable := e.Verified == syncd.VerifyVerified && e.Reachable && !e.Blocked && live
	pending := n.RequestedEndpointID == e.ID
	border, bg, iconCol := p.border, p.pane, p.muted
	switch {
	case e.InUse && live:
		border, bg, iconCol = p.tintBorder, p.tint.Alpha(0.35), p.success
	case failed:
		border, bg, iconCol = p.dangerBorder, p.dangerBg, p.danger
	}
	icon := kindIcon(e.Kind)
	if failed {
		icon = icBlock
	}
	ui.Row(c.Key("ep-"+e.ID)).FillWidth().Gap(14).Padding(14, 16).Radius(radiusCard).Border(1, border).Background(bg).Children(func() {
		ui.Icon(c, icon).FontSize(22).TextColor(iconCol)
		ui.Column(c).Grow(1).MinWidth(0).Gap(4).Children(func() {
			ui.Row(c).Gap(8).Wrap().Children(func() {
				ui.Text(c, e.Name).FontWeight(600)
				switch {
				case e.InUse && live:
					pill(c, L.EndpointInUse(), p.accent, p.onAccent)
				case pending:
					pill(c, L.EndpointSwitchingTo(), p.tint, p.accentText)
				}
				if e.Main {
					pill(c, L.EndpointMain(), p.hover, p.muted)
				}
			})
			where := e.URL
			if k := kindWord(e.Kind); k != e.Name {
				where = k + " · " + e.URL
			}
			muted(c, where, 13).Ellipsis("…").MaxLines(1)
			if failed {
				ui.Text(c, blocked).FontSize(fontSmall).FontWeight(600).TextColor(p.danger).Wrap()
				return
			}
			ui.Row(c).Gap(14).Wrap().Children(func() {
				if e.Approved {
					trustStep(c, icCheck, L.TrustApproved(), p.success)
				}
				switch e.Verified {
				case syncd.VerifyVerified:
					trustStep(c, icCheck, L.TrustVerified(), p.success)
				default:
					trustStep(c, icClock, L.TrustPending(), p.warn)
				}
			})
			if blocked != "" {
				muted(c, blocked, fontSmall).Wrap()
			}
		})
		ui.Row(c).Gap(8).Shrink(0).AlignSelf(ui.Center).Children(func() {
			// How it answers: the probe's round trip once one succeeded.
			answers := func() {
				if !usable && blocked != "" {
					return // its card says why already
				}
				ui.Column(c).AlignItems(ui.End).Children(func() {
					if !usable {
						muted(c, L.Unreachable(), 13)
						return
					}
					word := L.LatencyReachable()
					if e.LatencyMs > 0 && fastest(a.snap, e) {
						word = L.LatencyFastest()
					}
					ui.Text(c, word).FontSize(13)
					if e.LatencyMs > 0 {
						muted(c, L.LatencyMs(strconv.Itoa(e.LatencyMs)), fontSmall)
					}
				})
			}
			switch {
			case failed:
			case e.Verified != syncd.VerifyVerified:
				if button(c, "ep-verify-"+e.ID, L.ActionVerifyNow(), nil).Disabled(a.busy["ep-"+e.ID] || !live).Clicked() {
					a.do("ep-"+e.ID, func() error { return a.client.VerifyEndpoint(e.ID) }, nil)
				}
			case n.Policy != syncd.PolicyAuto && !e.InUse && usable && !pending:
				answers()
				if button(c, "ep-use-"+e.ID, L.ActionUseThis(), nil).Disabled(a.busy["policy"] || n.RequestedEndpointID != "").Clicked() {
					a.requestPolicy(e.AccountID, syncd.PolicyFixed, e.ID)
				}
			default:
				answers()
			}
			if e.Main || e.InUse {
				return // SyncD keeps the main address and the one in use
			}
			button(c, "ep-more-"+e.ID, "", icMore).Label(L.ActionMore()).Tooltip(L.ActionMore()).PopoverMenuButton(func(m *ui.Menu) {
				if m.Item(L.MenuRemoveAddress()).Danger().Disabled(a.busy["ep-"+e.ID] || !live || pending).Chosen() {
					a.ask(L.ConfirmRemoveAddressTitle(e.Name), L.ConfirmRemoveAddressBody(), L.ActionRemove(), func() {
						a.do("ep-"+e.ID, func() error { return a.client.RemoveEndpoint(e.ID) }, nil)
					})
				}
			})
		})
	})
}

// fastest tells whether e answers fastest of its account's reachable,
// trusted addresses whose answer time is known.
func fastest(s syncd.Snapshot, e syncd.Endpoint) bool {
	for _, o := range s.Endpoints {
		if o.AccountID == e.AccountID && o.Reachable && !o.Blocked && o.Verified == syncd.VerifyVerified && o.LatencyMs > 0 && o.LatencyMs < e.LatencyMs {
			return false
		}
	}
	return true
}

// trustStep is one of an address's trust steps.
func trustStep(c *ui.Context, icon *ui.SVG, word string, col ui.Color) {
	ui.Row(c).Gap(4).Children(func() {
		ui.Icon(c, icon).FontSize(12).TextColor(col)
		ui.Text(c, word).FontSize(fontSmall).FontWeight(600).TextColor(col)
	})
}

// whyCard says why the address in use was chosen, in words: SyncD's
// reasons when it gives them, else what the snapshot itself shows (the
// address verified, chosen by the user), never a guess.
func (a *App) whyCard(c *ui.Context, n syncd.Network) {
	p := pal(c)
	e, ok := a.snap.Endpoint(n.ActiveID)
	if !ok || !a.snap.Daemon.Connected {
		return
	}
	why := n.Why
	if len(why) == 0 {
		if e.Verified == syncd.VerifyVerified {
			why = append(why, syncd.ReasonVerified)
		}
		if n.Policy == syncd.PolicyFixed && !e.Main {
			why = append(why, syncd.ReasonUserChoice)
		}
	}
	ui.Column(c).Gap(10).Padding(16).Radius(radiusCard).Background(p.subtle).Children(func() {
		sectionTitle(c, L.WhyTitle(e.Name))
		if n.Switching || n.RequestedEndpointID != "" {
			ui.Text(c, L.Switching()).FontSize(13).TextColor(p.accentText).Wrap()
		}
		for _, r := range why {
			words := whyWords(a.snap, e, n, r)
			if words == "" {
				continue
			}
			icon, col := icCheck, p.success
			if r == syncd.ReasonUnreachable || r == syncd.ReasonPostponed {
				icon, col = icInfo, p.muted
			}
			ui.Row(c).Gap(8).AlignItems(ui.Start).Children(func() {
				ui.Icon(c, icon).FontSize(16).TextColor(col)
				ui.Text(c, words).FontSize(13).Wrap().Grow(1).MinWidth(0)
			})
		}
		if at, reason := lastSwitch(a.snap, n); !at.IsZero() {
			ui.Row(c).Gap(8).AlignItems(ui.Start).Children(func() {
				ui.Icon(c, icClock).FontSize(16).TextColor(p.muted)
				ui.Text(c, L.SwitchedAgo(ago(at, time.Now()), reasonSentence(reason))).FontSize(13).Wrap().Grow(1).MinWidth(0)
			})
		}
		muted(c, L.WhyNote(), fontSmall).Wrap()
	})
}

// whyWords is one of SyncD's reasons for the address in use, in words, or
// "" for one with nothing to add. SyncD sends no speedup factor (1.9): a
// faster address says its answer time beside the main one's when both
// are known.
func whyWords(s syncd.Snapshot, e syncd.Endpoint, n syncd.Network, r syncd.Reason) string {
	switch r {
	case syncd.ReasonVerified:
		if e.Main {
			return L.WhyMain()
		}
		return L.WhyVerified()
	case syncd.ReasonFaster:
		if n.Speedup >= 2 {
			return L.WhyFaster(n.Speedup)
		}
		if m, ok := s.Endpoint(mainEndpoint(s, e.AccountID)); ok && !e.Main && e.LatencyMs > 0 && m.LatencyMs > e.LatencyMs {
			return L.WhyFasterMs(strconv.Itoa(e.LatencyMs), strconv.Itoa(m.LatencyMs))
		}
		return L.WhyFasterPlain()
	case syncd.ReasonStable:
		return L.WhyStable()
	case syncd.ReasonUserChoice:
		return L.WhyUser()
	case syncd.ReasonJoinedNetwork:
		return L.WhyJoined()
	case syncd.ReasonUnreachable:
		return L.WhyUnreachable()
	case syncd.ReasonPostponed:
		if n.Switching || n.RequestedEndpointID != "" {
			return "" // the pending switch says it already
		}
		return L.WhyPostponed()
	}
	return ""
}

// switchList is the account's recent switches, newest first.
func (a *App) switchList(c *ui.Context) {
	p := pal(c)
	s := a.snap
	var list []syncd.Switch
	for _, sw := range s.Switches {
		if sw.AccountID == a.account {
			list = append(list, sw)
		}
	}
	name := func(id string) string {
		e, _ := s.Endpoint(id)
		return e.Name
	}
	ui.Column(c).Children(func() {
		ui.Box(c).Margin(0, 0, 8, 0).Children(func() { sectionTitle(c, L.RecentSwitches()) })
		if len(list) == 0 && stateKnown(s) {
			muted(c, L.NoSwitches(), 13)
		}
		now := time.Now()
		for i, sw := range list {
			title, body := name(sw.FromID)+" → "+name(sw.ToID), ""
			switch sw.Reason {
			case syncd.ReasonJoinedNetwork:
				body = L.SwitchJoined()
			case syncd.ReasonUnreachable:
				body = L.SwitchUnreachable()
			case syncd.ReasonUserChoice:
				body = L.SwitchUser()
			case syncd.ReasonFaster:
				body = L.SwitchFaster()
			case syncd.ReasonPostponed:
				title, body = L.SwitchPostponedTitle(int(sw.Postponed/time.Minute)), L.SwitchPostponedBody()
			}
			r := ui.Row(c).Gap(12).Padding(10, 0).AlignItems(ui.Start)
			if i < len(list)-1 {
				r.BorderWidth(0, 0, 1, 0).BorderColor(p.hover)
			}
			r.Children(func() {
				muted(c, clock(sw.At, now), fontSmall).Width(72).Shrink(0)
				ui.Column(c).Gap(2).Grow(1).MinWidth(0).Children(func() {
					ui.Text(c, title).FontSize(13).FontWeight(600)
					if body != "" {
						muted(c, body, 13).Wrap()
					}
				})
			})
		}
	})
}

// addrForm is the Add an address dialog while open: the address typed,
// then what the address showed before any credential went to it.
type addrForm struct {
	open      bool
	accountID string
	name, url string
	// kind is where the address leads, an index of addrKinds; it never
	// affects trust.
	kind int
	// failed says why the address could not be looked at, or "".
	failed string
	staged *syncd.Staging
	agree  bool
}

// addrKinds are the kinds Add an address offers, in its order.
var addrKinds = []syncd.EndpointKind{syncd.KindInternet, syncd.KindHome, syncd.KindVPN}

// addrDialog adds an address in two trust steps: the user approves the
// origin after seeing its certificate, then SyncD verifies it is the same
// server (design/AddAddress.html; PLAN.md §7.3).
func (a *App) addrDialog(c *ui.Context) {
	if !a.addr.open {
		return
	}
	p := pal(c)
	f := &a.addr
	acc, _ := a.snap.Account(f.accountID)
	ui.Modal(c.Key("addr"), &f.open, func() {
		ui.Column(c).Width(560).Gap(16).Children(func() {
			ui.Column(c).Gap(4).Children(func() {
				ui.Text(c, L.AddAddressTitle()).FontSize(20).FontWeight(600).Level(1)
				muted(c, L.AddAddressSubtitle(accountName(acc)), fontBody)
			})
			step := 1
			if f.staged != nil {
				step = 2
			}
			stepper(c, step, L.StepAddress(), L.StepApprove(), L.StepVerify())
			ui.Row(c).Gap(12).FillWidth().Children(func() {
				ui.Column(c).Grow(1).Gap(6).Children(func() {
					ui.Text(c, L.FieldName()).FontSize(13).FontWeight(600)
					ui.TextInput(c.Key("addr-name"), &f.name).Placeholder(L.FieldNamePlaceholder()).ReadOnly(f.staged != nil).Label(L.FieldName())
				})
				ui.Column(c).Grow(2).Gap(6).Children(func() {
					ui.Text(c, L.FieldAddress()).FontSize(13).FontWeight(600)
					in := ui.TextInput(c.Key("addr-url"), &f.url).Placeholder("https://cloud.vpn.example").ReadOnly(f.staged != nil).Label(L.FieldAddress())
					if f.failed != "" {
						in.Error(f.failed)
					}
				})
			})
			ui.Column(c).Gap(6).Children(func() {
				ui.Text(c, L.FieldKind()).FontSize(13).FontWeight(600)
				ui.Segmented(c.Key("addr-kind"), &f.kind, kindWord(addrKinds[0]), kindWord(addrKinds[1]), kindWord(addrKinds[2])).Label(L.FieldKind()).Disabled(f.staged != nil).AlignSelf(ui.Start)
			})
			if f.staged == nil {
				muted(c, L.AddressChecking(), fontSmall).Wrap()
			} else {
				a.approveSection(c, acc)
			}
			ui.Column(c).Gap(6).Padding(16).Radius(radiusCard).Border(1, p.border).BorderStyle(ui.BorderDashed).Children(func() {
				ui.Text(c, L.VerifyTitle()).FontSize(fontSection).FontWeight(600)
				muted(c, L.VerifyBody(), 13).Wrap()
			})
			ui.Row(c).Gap(10).Justify(ui.End).FillWidth().Children(func() {
				if button(c, "addr-cancel", L.ActionCancel(), nil).Clicked() {
					a.addr = addrForm{}
				}
				if f.staged == nil {
					if primary(c, "addr-check", L.ActionCheckAddress(), nil).Disabled(a.busy["stage"] || f.url == "").Clicked() {
						a.stageAddress()
					}
				} else if primary(c, "addr-approve", L.ActionApproveVerify(), nil).Disabled(!f.agree || !f.staged.SecureTLS || a.busy["approve"]).Clicked() {
					id := f.staged.ID
					a.do("approve", func() error { _, err := a.client.ApproveEndpoint(id); return err }, func(err error) {
						if err != nil {
							a.setToast(errText(err))
							return
						}
						a.addr = addrForm{}
						a.setToast(L.AddressAdded())
					})
				}
			})
		})
	})
}

// stageAddress asks SyncD to look at the address typed, sending no
// credential (endpoints.stage).
func (a *App) stageAddress() {
	f := a.addr
	name := f.name
	if name == "" {
		name = hostOf(f.url)
	}
	a.do("stage", func() error {
		st, err := a.client.StageEndpoint(f.accountID, name, f.url, addrKinds[f.kind])
		a.later(func() {
			if err != nil {
				a.addr.failed = L.AddressInvalid()
				if t := reasonText(syncd.ReasonOf(err)); t != "" {
					a.addr.failed = t
				}
				return
			}
			a.addr.failed, a.addr.staged, a.addr.name = "", &st, name
		})
		return nil
	}, nil)
}

// approveSection is step 2: the certificate and server seen, and the
// user's explicit permission to send the sign-in there.
func (a *App) approveSection(c *ui.Context, acc syncd.Account) {
	p := pal(c)
	st := a.addr.staged
	ui.Column(c).Gap(10).Padding(16).Radius(radiusCard).Border(1, p.border).Children(func() {
		ui.Text(c, L.ApproveTitle()).FontSize(fontSection).FontWeight(600)
		if !st.SecureTLS {
			ui.Text(c, L.ApproveInsecure()).FontSize(13).TextColor(p.danger).Wrap()
		} else {
			muted(c, L.ApproveBody(), 13).Wrap()
		}
		ui.Grid(c).ColumnTracks(ui.Fixed(130), ui.Fr(1)).GapY(6).Padding(10, 12).Radius(radiusCtl).Background(p.subtle).Children(func() {
			conn, connCol := L.CertSecure(), p.success
			if !st.SecureTLS {
				conn, connCol = L.CertInsecure(), p.danger
			}
			server := L.ServerSameVersion()
			if !st.SameVersion {
				server = L.ServerOtherVersion()
			}
			muted(c, L.CertConnection(), 13)
			ui.Text(c, conn).FontSize(13).FontWeight(600).TextColor(connCol)
			muted(c, L.CertFor(), 13)
			ui.Text(c, st.CertSubject).FontSize(13)
			muted(c, L.CertIssuer(), 13)
			ui.Text(c, st.CertIssuer).FontSize(13)
			muted(c, L.ServerSays(), 13)
			ui.Text(c, server).FontSize(13)
		})
		if st.SecureTLS {
			ui.Checkbox(c.Key("addr-agree"), &a.addr.agree, L.ApproveCheck(accountName(acc)))
		}
	})
}

// stepper shows numbered steps, those before current done.
func stepper(c *ui.Context, current int, steps ...string) {
	p := pal(c)
	ui.Row(c).FillWidth().Gap(8).Children(func() {
		for i, s := range steps {
			n := i + 1
			if i > 0 {
				ui.Box(c).Grow(1).Height(1).Background(p.border)
			}
			ui.Row(c).Gap(8).Shrink(0).Children(func() {
				switch {
				case n < current:
					ui.Box(c).Size(22, 22).Radius(11).Background(p.success).Center().Children(func() {
						ui.Icon(c, icCheck).FontSize(12).TextColor(p.pane)
					})
					ui.Text(c, s).FontSize(13).FontWeight(600).TextColor(p.success)
				case n == current:
					ui.Box(c).Size(22, 22).Radius(11).Background(p.accent).Center().Children(func() {
						ui.Text(c, strconv.Itoa(n)).FontSize(fontSmall).TextColor(p.onAccent)
					})
					ui.Text(c, s).FontSize(13).FontWeight(600)
				default:
					ui.Box(c).Size(22, 22).Radius(11).Border(1.5, p.muted).Center().Children(func() {
						ui.Text(c, strconv.Itoa(n)).FontSize(fontSmall).TextColor(p.muted)
					})
					muted(c, s, 13)
				}
			})
		}
	})
}
