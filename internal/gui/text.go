package gui

import (
	"fmt"
	"time"

	"github.com/egoist/mygo/ui"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
	L "github.com/YuanZhe-99/Nextcloud-MyGo/locales"
)

// accountName is how people know an account: user@server.
func accountName(a syncd.Account) string { return a.User + "@" + a.Server }

// ago says how long ago t was, relative to now.
func ago(t, now time.Time) string {
	d := now.Sub(t)
	switch {
	case d < time.Minute:
		return L.AgoJustNow()
	case d < time.Hour:
		return L.AgoMinutes(int(d / time.Minute))
	case d < 24*time.Hour:
		return L.AgoHours(int(d / time.Hour))
	}
	return L.AgoDays(int(d / (24 * time.Hour)))
}

// clock is a time of day for a list, or Yesterday and older as a date.
func clock(t, now time.Time) string {
	y1, m1, d1 := t.Date()
	y2, m2, d2 := now.Date()
	switch {
	case y1 == y2 && m1 == m2 && d1 == d2:
		return t.Format("15:04")
	case now.Sub(t) < 48*time.Hour:
		return L.Yesterday()
	}
	return t.Format("2006-01-02")
}

// size is a byte count as people read it.
func size(b int64) string {
	const unit = 1000
	if b < unit {
		return fmt.Sprintf("%d B", b)
	}
	div, exp := int64(unit), 0
	for n := b / unit; n >= unit; n /= unit {
		div *= unit
		exp++
	}
	v := float64(b) / float64(div)
	if v >= 100 {
		return fmt.Sprintf("%.0f %cB", v, "kMGTPE"[exp])
	}
	return fmt.Sprintf("%.1f %cB", v, "kMGTPE"[exp])
}

// overall is the whole app's state, for the sidebar, the hero and the
// tray: what needs the user first, then what moves.
type overall int

const (
	overallUpToDate overall = iota
	overallSyncing
	overallPaused
	overallNeedsYou
	overallOffline
)

func overallOf(s syncd.Snapshot) overall {
	if s.Daemon.AllPaused {
		return overallPaused
	}
	for _, d := range s.Decisions {
		if d.Kind == syncd.DecisionLocalDeletions {
			return overallNeedsYou
		}
	}
	syncing, offline := false, len(s.Folders) > 0
	for _, f := range s.Folders {
		switch f.State {
		case syncd.FolderNeedsReview, syncd.FolderWaitingSignIn, syncd.FolderError:
			return overallNeedsYou
		case syncd.FolderSyncing:
			syncing = true
		}
		if f.State != syncd.FolderOffline {
			offline = false
		}
	}
	switch {
	case syncing:
		return overallSyncing
	case offline:
		return overallOffline
	}
	return overallUpToDate
}

// word and colour of the overall state.
func (o overall) word() string {
	switch o {
	case overallSyncing:
		return L.StateSyncing()
	case overallPaused:
		return L.StatePaused()
	case overallNeedsYou:
		return L.StateNeedsYou()
	case overallOffline:
		return L.StateOffline()
	}
	return L.StateUpToDate()
}

func (o overall) color(p *palette) ui.Color {
	switch o {
	case overallSyncing:
		return p.accentText
	case overallPaused, overallOffline:
		return p.muted
	case overallNeedsYou:
		return p.warn
	}
	return p.success
}

// folderWord is a folder's state in words, its colour, and whether its dot
// is hollow (waiting on something outside the app).
func folderWord(f syncd.Folder, p *palette) (string, ui.Color, bool) {
	switch f.State {
	case syncd.FolderSyncing:
		return L.FolderSyncing(), p.accentText, false
	case syncd.FolderPaused:
		if f.UserPaused {
			return L.FolderPausedByYou(), p.muted, false
		}
		return L.FolderPaused(), p.muted, false
	case syncd.FolderNeedsReview:
		return L.FolderNeedsReview(), p.warn, false
	case syncd.FolderWaitingSignIn:
		return L.FolderWaitingSignIn(), p.muted, true
	case syncd.FolderOffline:
		return L.FolderOffline(), p.muted, true
	case syncd.FolderError:
		return L.FolderError(), p.danger, false
	}
	return L.FolderUpToDate(), p.success, false
}

// folderDetail is the second line under a folder's state, or "".
func folderDetail(f syncd.Folder, s syncd.Snapshot) string {
	switch f.State {
	case syncd.FolderSyncing:
		switch f.Phase {
		case syncd.PhaseUploading:
			return L.FolderUploading(f.FilesDone)
		case syncd.PhaseDownloading:
			return L.FolderDownloading(f.FilesDone)
		}
		return L.FolderChecking()
	case syncd.FolderNeedsReview:
		for _, d := range s.Decisions {
			if d.FolderID == f.ID && d.Kind == syncd.DecisionLocalDeletions {
				return L.FolderDeletions(d.Count)
			}
		}
	case syncd.FolderPaused:
		if f.UserPaused {
			return L.FolderStaysPaused()
		}
	}
	return ""
}

// reasonSentence ends "Switched … ago: " with why.
func reasonSentence(r syncd.Reason) string {
	switch r {
	case syncd.ReasonJoinedNetwork:
		return L.ReasonSentenceJoined()
	case syncd.ReasonUnreachable:
		return L.ReasonSentenceUnreachable()
	case syncd.ReasonUserChoice:
		return L.ReasonSentenceUser()
	case syncd.ReasonPostponed:
		return L.ReasonSentencePostponed()
	}
	return L.ReasonSentenceFaster()
}

// kindIcon is an address's icon.
func kindIcon(k syncd.EndpointKind) *ui.SVG {
	switch k {
	case syncd.KindHome:
		return icHouse
	case syncd.KindVPN:
		return icShield
	}
	return icGlobe
}

// activeEndpoint is the address an account uses now.
func activeEndpoint(s syncd.Snapshot, accountID string) (syncd.Endpoint, syncd.Network, bool) {
	n, ok := s.Network(accountID)
	if !ok {
		return syncd.Endpoint{}, n, false
	}
	e, ok := s.Endpoint(n.ActiveID)
	return e, n, ok
}

// hostOf is a URL without its scheme.
func hostOf(url string) string {
	for _, p := range []string{"https://", "http://"} {
		if len(url) > len(p) && url[:len(p)] == p {
			return url[len(p):]
		}
	}
	return url
}
