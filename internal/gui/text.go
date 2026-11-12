package gui

import (
	"fmt"
	"slices"
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
	// overallUnknown: SyncD is not connected, or cannot read its own state;
	// nothing is known, so nothing is shown as healthy (INV-15).
	overallUnknown
	// overallFinished: every folder finished its last cycle, in a build
	// whose file sync is not yet verified (fullSync is false): not green.
	overallFinished
)

// overallOf never presents stale, unknown or unsupported backend state as
// synchronized (PLAN.md §13.6, §13.7).
func overallOf(s syncd.Snapshot) overall {
	if !s.Daemon.Connected || !s.Daemon.StateAvailable {
		return overallUnknown
	}
	if s.Daemon.AllPaused {
		return overallPaused
	}
	for _, d := range s.Decisions {
		if d.Kind == syncd.DecisionBulkDeletions {
			return overallNeedsYou
		}
	}
	for _, acc := range s.Accounts {
		if needsSignIn(s, acc) {
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
		if quotaFull(f) {
			// Finished, but the server refuses its uploads: the user acts.
			return overallNeedsYou
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
	case !s.Daemon.Capabilities["fullSync"]:
		return overallFinished
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
	case overallUnknown:
		return L.StateUnknown()
	case overallFinished:
		return L.StateFinished()
	}
	return L.StateUpToDate()
}

func (o overall) color(p *palette) ui.Color {
	switch o {
	case overallSyncing:
		return p.accentText
	case overallPaused, overallOffline, overallUnknown, overallFinished:
		return p.muted
	case overallNeedsYou:
		return p.warn
	}
	return p.success
}

// stateKnown tells whether the snapshot holds SyncD's actual lists: a
// missing collection is unknown, never an empty account (PLAN.md §13.6).
func stateKnown(s syncd.Snapshot) bool {
	return s.Daemon.StateAvailable && s.Accounts != nil
}

// needsSignIn tells whether an account waits for the user to sign in
// again, as its Auth says (protocol 1.9).
// Notes: a daemon before 1.9 sends no Auth: then signed out, or a folder
// its server refuses.
func needsSignIn(s syncd.Snapshot, acc syncd.Account) bool {
	if acc.Auth != "" {
		return acc.Auth != syncd.AuthSignedIn
	}
	if !acc.SignedIn {
		return true
	}
	for _, f := range s.Folders {
		if f.AccountID == acc.ID && (f.State == syncd.FolderWaitingSignIn || f.ReasonCode == "AUTH_REQUIRED") {
			return true
		}
	}
	return false
}

// authWords is why an account needs signing in again, in words: the
// credential cannot be used on this PC, or the server refused it.
func authWords(acc syncd.Account) (state, body string) {
	if acc.Auth == syncd.AuthNeedsSignIn {
		return L.AccountNeedsSignIn(), L.AttnNeedsSignInBody()
	}
	return L.AccountSignedOut(), L.AttnSignInBody()
}

// quotaFull tells whether the server refused a folder's uploads because
// the account's storage there is full; its cycle still finishes.
func quotaFull(f syncd.Folder) bool { return f.ReasonCode == "REMOTE_QUOTA_EXCEEDED" }

// accountsFull are the accounts with a folder whose uploads the server
// refuses for want of storage, in the snapshot's order.
func accountsFull(s syncd.Snapshot) []syncd.Account {
	var out []syncd.Account
	for _, acc := range s.Accounts {
		if slices.ContainsFunc(s.Folders, func(f syncd.Folder) bool { return f.AccountID == acc.ID && quotaFull(f) }) {
			out = append(out, acc)
		}
	}
	return out
}

// modeShown tells whether a folder's mode is worth saying: on-demand
// files exist (the virtualFiles capability) or the folder uses them.
func modeShown(s syncd.Snapshot, f syncd.Folder) bool {
	return s.Daemon.Capabilities["virtualFiles"] || f.Mode == syncd.FolderOnDemand
}

// modeWord is how a folder keeps its files on this PC; empty is a daemon
// before 1.8, which knew only full files.
func modeWord(m syncd.FolderMode) string {
	if m == syncd.FolderOnDemand {
		return L.ModeOnDemand()
	}
	return L.ModeFull()
}

// kindWord is where an address leads, in words.
func kindWord(k syncd.EndpointKind) string {
	switch k {
	case syncd.KindHome:
		return L.KindHome()
	case syncd.KindVPN:
		return L.KindVPN()
	}
	return L.KindInternet()
}

// quarantined tells whether an address is blocked for its security: its
// certificate, a different server, or an origin never allowed (INV-04).
func quarantined(e syncd.Endpoint) bool {
	switch e.BlockedReason {
	case "TLS_ERROR", "ENDPOINT_IDENTITY_MISMATCH", "ENDPOINT_UNTRUSTED":
		return e.Blocked
	}
	return e.Verified == syncd.VerifyFailed
}

// blockedWords is why an address is not used, in words, or "" when its
// trust steps already say it (a verification under way) or it is not
// blocked.
func blockedWords(e syncd.Endpoint) string {
	if !e.Blocked {
		return ""
	}
	switch e.BlockedReason {
	case "TLS_ERROR":
		return L.BlockedTLS()
	case "ENDPOINT_UNTRUSTED":
		return L.BlockedUntrusted()
	case "AUTH_REQUIRED":
		return L.BlockedSignIn()
	case "NETWORK_UNAVAILABLE":
		return L.BlockedNetwork()
	case "VERIFICATION_PENDING":
		return ""
	}
	if quarantined(e) {
		return L.TrustBlocked()
	}
	return ""
}

// pendingReview is the deletion proposal waiting for a folder, if any.
func pendingReview(s syncd.Snapshot, folderID string) (syncd.Decision, bool) {
	for _, d := range s.Decisions {
		if d.FolderID == folderID && d.Kind == syncd.DecisionBulkDeletions {
			return d, true
		}
	}
	return syncd.Decision{}, false
}

// folderWord is a folder's state in words, its colour, and whether its dot
// is hollow (waiting on something outside the app). Only a supported full
// sync is ever green.
func folderWord(f syncd.Folder, s syncd.Snapshot, p *palette) (string, ui.Color, bool) {
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
	case syncd.FolderUpToDate:
		if quotaFull(f) {
			// The cycle finished; its uploads were refused.
			return L.FolderStorageFull(), p.warn, false
		}
		if !s.Daemon.Capabilities["fullSync"] {
			// Not green while file sync is not a supported capability.
			return L.StateFinished(), p.muted, false
		}
		return L.FolderUpToDate(), p.success, false
	}
	// A state this version does not know (PLAN.md §13.3): never green.
	return L.FolderUnknown(), p.muted, true
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
		if d, ok := pendingReview(s, f.ID); ok {
			return L.FolderDeletions(d.Count)
		}
	case syncd.FolderPaused:
		if f.UserPaused {
			return L.FolderStaysPaused()
		}
	case syncd.FolderUpToDate:
		if quotaFull(f) {
			// Short: the account's notice says what to do.
			return L.FolderQuotaDetail()
		}
	}
	return reasonText(f.ReasonCode)
}

// reasonText puts a stable reason code (PLAN.md §13.8) into words for the
// person using the app, or "" for a code with nothing more to say than
// its state; the code itself is never shown.
func reasonText(code string) string {
	switch code {
	case "AUTH_REQUIRED":
		return L.ReasonAuthRequired()
	case "CREDENTIALS_LOCKED":
		return L.ReasonCredentialsLocked()
	case "NETWORK_UNAVAILABLE":
		return L.ReasonNetwork()
	case "TLS_ERROR":
		return L.ReasonTLS()
	case "ENDPOINT_UNTRUSTED", "ENDPOINT_IDENTITY_MISMATCH":
		return L.ReasonIdentity()
	case "LOCAL_ROOT_UNAVAILABLE":
		return L.ReasonLocalRoot()
	case "REMOTE_ROOT_UNAVAILABLE":
		return L.ReasonRemoteRoot()
	case "FOLDER_ROOT_CHANGED":
		return L.ReasonRootChanged()
	case "FOLDER_OWNED":
		return L.ReasonFolderOwned()
	case "ACCOUNT_EXISTS":
		return L.ReasonAccountExists()
	case "REMOTE_QUOTA_EXCEEDED":
		return L.ReasonQuota()
	case "DECISION_PENDING":
		return L.ReasonDeletions()
	case "VERIFICATION_PENDING":
		return L.TrustPending()
	case "UNSUPPORTED_FOLDER_MODE":
		return L.ReasonFolderMode()
	case "JOURNAL_ERROR":
		return L.ReasonJournal()
	case "DISCOVERY_INCOMPLETE":
		return L.ReasonDiscovery()
	case "DELETION_CONFIRMATION_REQUIRED":
		return L.ReasonDeletions()
	case "SERVER_THROTTLED", "RATE_LIMITED":
		return L.ReasonThrottled()
	case "REMOTE_LOCKED":
		return L.ReasonLocked()
	case "PERMISSION_DENIED":
		return L.ReasonPermission()
	case "QUIESCE_STALLED", "PAUSED":
		return L.ReasonBusy()
	case "STATE_UNAVAILABLE":
		return L.ReasonStateUnavailable()
	case "UNSUPPORTED_CAPABILITY":
		return L.ReasonUnsupported()
	case "REVISION_CONFLICT", "OPERATION_KEY_CONFLICT", "NOT_FOUND":
		return L.ToastStale()
	case "LOGIN_SESSION_EXPIRED":
		return L.ReasonLoginExpired()
	case "LOGIN_CANCELLED", "CANCELLED":
		return L.ReasonCancelled()
	case "APP_PASSWORD_REVOCATION_UNCONFIRMED":
		return L.CredentialCleanupUnconfirmed()
	case "INVALID_SERVER_RESPONSE":
		return L.ReasonServerResponse()
	case "OPERATION_INTERRUPTED":
		return L.ReasonInterrupted()
	case "SYNC_FAILED", "RESUME_CONTEXT_UNSUPPORTED", "RESYNC_REQUIRED", "INTERNAL_ERROR":
		return L.ReasonSyncFailed()
	}
	return ""
}

// startupWord is why SyncD runs without its state, in words.
func startupWord(st syncd.StartupState) string {
	switch st {
	case syncd.StartupBackoff:
		return L.StartupBackoff()
	case syncd.StartupDatabaseUnavailable:
		return L.StartupDatabase()
	case syncd.StartupStopped:
		return L.StartupStopped()
	}
	return L.StartupDiagnostic()
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
	case syncd.ReasonFaster:
		return L.ReasonSentenceFaster()
	}
	return L.ReasonSentenceVerified()
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

// lastSwitch is when an account last changed address and why: SyncD's
// own summary when it gives one, else the newest switch in the history.
func lastSwitch(s syncd.Snapshot, n syncd.Network) (time.Time, syncd.Reason) {
	if !n.Switched.IsZero() {
		return n.Switched, n.SwitchReason
	}
	var at time.Time
	var why syncd.Reason
	for _, sw := range s.Switches {
		if sw.AccountID == n.AccountID && sw.At.After(at) {
			at, why = sw.At, sw.Reason
		}
	}
	return at, why
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
