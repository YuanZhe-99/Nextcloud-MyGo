// Package syncd is the interface's side of the contract with SyncD, the
// background sync process (PLAN.md §13): the state SyncD reports and the
// commands it takes. Remote connects to SyncD; Mock is the explicit demo adapter.
//
// Enumerations cross as strings, as the protocol carries them. Reason and
// state codes are for the interface to put into words: they are never
// shown as they are.
package syncd

import (
	"context"
	"errors"
	"time"
)

// Client is what the interface asks of SyncD. Every method may block on
// the pipe: call it off the main thread.
type Client interface {
	// Snapshot returns the latest consistent state (state.bootstrap and
	// the events since).
	Snapshot() Snapshot
	// Subscribe calls fn, on any goroutine, after the snapshot changed.
	// Returns: a function that stops the calls.
	Subscribe(fn func()) (cancel func())

	PauseAll() error
	ResumeAll() error
	PauseFolder(folderID string) error
	ResumeFolder(folderID string) error
	SyncNow(folderID string) error
	// RemoveFolder stops syncing a folder; its files stay on both sides.
	RemoveFolder(folderID string) error

	// ResolveDecision answers a pending decision once (decisions.resolve).
	// Inputs: digest is the Decision's, so an answer to a changed
	// proposal is refused with ErrStale.
	ResolveDecision(decisionID, digest string, action Action) error

	// StartLogin begins Login Flow v2 for a server: SyncD opens nothing
	// itself; the interface opens LoginURL in the browser.
	StartLogin(serverURL string) (Login, error)
	// WaitLogin returns the new or re-authenticated account once the
	// browser step is done, or the context's error.
	WaitLogin(ctx context.Context, loginID string) (accountID string, err error)
	CancelLogin(loginID string) error
	// Reauthenticate starts a new login for an account whose app
	// password stopped working, keeping its identity.
	Reauthenticate(accountID string) (Login, error)
	RemoveAccount(accountID string) error

	// RemoteFolders lists the folders at the top of an account's files.
	RemoteFolders(accountID string) ([]RemoteFolder, error)
	// AddFolder maps a remote folder to a local one (folders.add); mode, at
	// most one, is fixed for the folder's lifetime and defaults to FolderFull.
	AddFolder(accountID, remotePath, localPath string, mode ...FolderMode) error

	// RequestPolicy asks SyncD to choose addresses automatically or to
	// always use fixedEndpointID (endpoints.setPolicy).
	// Returns: once SyncD accepted it; the change completes at a safe point,
	// and Network.RequestedEndpointID shows a fixed target still pending.
	RequestPolicy(accountID string, policy Policy, fixedEndpointID string) (operationID string, err error)
	// StageEndpoint looks at an address without sending any credential
	// (endpoints.stage).
	// kind, at most one, says how the address is reached (protocol 1.9);
	// it never affects trust and defaults to KindInternet.
	StageEndpoint(accountID, name, url string, kind ...EndpointKind) (Staging, error)
	// ApproveEndpoint records the user's permission to send the account's
	// credential to the staged origin, then verifies it is the same server.
	ApproveEndpoint(stagingID string) (endpointID string, err error)
	VerifyEndpoint(endpointID string) error
	RemoveEndpoint(endpointID string) error

	// PrepareUpdate brings SyncD to a safe point for installing an update
	// (daemon.prepareUpdate); ErrBusy names why not now.
	PrepareUpdate(targetVersion string) error
	// Shutdown stops background sync (daemon.shutdown).
	Shutdown() error

	// Diagnostics reads SyncD's sanitized runtime counters
	// (diagnostics.snapshot); it works while application state is unavailable.
	Diagnostics(ctx context.Context) (RuntimeDiagnostics, error)
	// ExportDiagnostics writes a local sanitized support bundle
	// (diagnostics.export). Returns: the bundle's path, for Diagnostics only.
	ExportDiagnostics() (path string, err error)
}

// Errors the interface tells apart.
var (
	ErrStale                 = errors.New("syncd: the decision changed; review it again")
	ErrBusy                  = errors.New("syncd: not at a safe point yet")
	ErrNotFound              = errors.New("syncd: no such item")
	ErrRefused               = errors.New("syncd: refused")
	ErrRevocationUnconfirmed = errors.New("syncd: account removed; server credential revocation unconfirmed")
	ErrCredentialCleanup     = errors.New("syncd: app-password revocation unconfirmed")
	ErrStateUnavailable      = errors.New("syncd: application state is unavailable")
	// ErrAccountExists: this user on this server is already an account.
	ErrAccountExists = errors.New("syncd: this account is already added")
)

// ReasonOf returns the stable reason code SyncD gave for err
// (PLAN.md §13.8), or "" when err carries none.
// Notes: for the interface to put into words; never shown as it is.
func ReasonOf(err error) string {
	if e, ok := errors.AsType[*rpcError](err); ok {
		return e.Data.Reason
	}
	return ""
}

// Snapshot is SyncD's whole state as the interface shows it.
// Notes: nil collections mean unknown, not a confirmed empty account or folder list.
type Snapshot struct {
	Daemon    Daemon     `json:"daemon"`
	Accounts  []Account  `json:"accounts,omitzero"`
	Folders   []Folder   `json:"folders,omitzero"`
	Endpoints []Endpoint `json:"endpoints,omitzero"`
	Networks  []Network  `json:"networks,omitzero"`
	Switches  []Switch   `json:"switches,omitzero"`
	Decisions []Decision `json:"decisions,omitzero"`
	Activity  []Activity `json:"activity,omitzero"`
}

// StartupState describes daemon recovery independently of transport authentication.
type StartupState string

const (
	StartupNormal              StartupState = "normal"
	StartupBackoff             StartupState = "backoff"
	StartupDiagnostic          StartupState = "diagnostic"
	StartupStopped             StartupState = "stopped"
	StartupDatabaseUnavailable StartupState = "databaseUnavailable"
)

// Daemon is SyncD itself.
type Daemon struct {
	// Connected requires a live negotiated bootstrap with available application state.
	// An authenticated diagnostic-only transport remains callable while this is false.
	Connected bool `json:"-"`
	// StateAvailable distinguishes known application state from diagnostic-only startup.
	StateAvailable bool         `json:"stateAvailable"`
	StartupState   StartupState `json:"startupState"`
	// Capabilities gate unfinished backend workflows without assuming version support.
	Capabilities map[string]bool `json:"capabilities"`
	Version      string          `json:"version"`
	Protocol     int             `json:"protocol"`
	// ProtocolMinor is the minor version session.hello negotiated.
	ProtocolMinor int `json:"-"`
	// Demo is true for Mock: the interface says it shows sample data.
	Demo bool `json:"demo"`
	// AllPaused is the user's pause of every folder.
	AllPaused bool `json:"allPaused"`
	// CheckedAt is when the last full check of every folder ended.
	CheckedAt time.Time `json:"checkedAt"`
	// StartupReport says what this start migrated from and whether its
	// self-check passed (protocol 1.9), for the installer after an update.
	StartupReport StartupReport `json:"startupReport"`
}

// StartupReport is what a SyncD start found after an update.
type StartupReport struct {
	// PreviousVersion is the SyncD version that used this profile before;
	// empty on a new profile or one last used before protocol 1.9.
	PreviousVersion string `json:"previousVersion"`
	// PreviousSchema and Schema are the Control schemas before and after.
	PreviousSchema int `json:"previousSchema"`
	Schema         int `json:"schema"`
	// SelfCheck is pending, passed or failed; ReasonCodes say why it failed.
	SelfCheck   string   `json:"selfCheck"`
	ReasonCodes []string `json:"reasonCodes"`
}

// Account is one Nextcloud account: its identity never depends on the
// address used to reach it (INV-02).
type Account struct {
	ID          string
	User        string
	DisplayName string
	// Server is the canonical address's host, as people know it.
	Server string
	// Auth is whether the account's credential still works (protocol 1.9);
	// empty means a daemon before 1.9, which reported SignedIn only.
	Auth     AuthState `json:"auth"`
	SignedIn bool
}

// AuthState is whether an account's credential still works.
type AuthState string

const (
	// AuthSignedIn: nothing is known to be wrong with the credential.
	AuthSignedIn AuthState = "signedIn"
	// AuthNeedsSignIn: the credential cannot be used on this computer (its
	// store refused it or it is missing), or a renewal waits for the browser.
	AuthNeedsSignIn AuthState = "needsSignIn"
	// AuthRevoked: the server refused the app password; it was revoked or
	// expired there, and signing in again replaces it.
	AuthRevoked AuthState = "revoked"
)

// FolderState is what a folder is doing, for the interface to say.
type FolderState string

const (
	FolderUpToDate      FolderState = "upToDate"
	FolderSyncing       FolderState = "syncing"
	FolderPaused        FolderState = "paused"
	FolderNeedsReview   FolderState = "needsReview"
	FolderWaitingSignIn FolderState = "waitingSignIn"
	FolderOffline       FolderState = "offline"
	FolderError         FolderState = "error"
)

// Phase is what a syncing folder transfers now.
type Phase string

const (
	PhaseChecking    Phase = "checking"
	PhaseUploading   Phase = "uploading"
	PhaseDownloading Phase = "downloading"
)

// Folder is one sync mapping.
type Folder struct {
	ID         string
	AccountID  string
	Name       string
	LocalPath  string
	RemotePath string
	State      FolderState
	// ReasonCode explains a blocker independently of the user's pause state.
	ReasonCode string `json:"reasonCode"`
	// UserPaused is kept apart from every other blocking reason, which
	// never overrides it (INV-13).
	UserPaused bool
	// Mode is fixed when the folder is added; empty means a daemon before
	// protocol 1.8, which knew only full-file folders.
	Mode FolderMode `json:"mode"`
	// Phase is what a syncing folder does now; empty while it does not sync
	// (protocol 1.9; earlier daemons always said checking).
	Phase Phase `json:"phase"`
	// FilesDone counts the files of the current cycle so far; SyncD gives
	// no total while discovery runs, so there is none to show.
	FilesDone int `json:"filesDone"`
}

// FolderMode is how a folder keeps its files on this computer.
type FolderMode string

const (
	// FolderFull downloads every file.
	FolderFull FolderMode = "full"
	// FolderOnDemand keeps Windows Cloud Files placeholders and downloads a
	// file when it is opened (protocol 1.8).
	FolderOnDemand FolderMode = "onDemand"
)

// Policy is how an account's address is chosen.
type Policy string

const (
	PolicyAuto  Policy = "auto"
	PolicyFixed Policy = "fixed"
)

// EndpointKind is where an address leads, for its icon and name.
type EndpointKind string

const (
	KindInternet EndpointKind = "internet"
	KindHome     EndpointKind = "home"
	KindVPN      EndpointKind = "vpn"
)

// Verification is the second trust step: the address answers as the same
// server with the same files.
type Verification string

const (
	VerifyPending  Verification = "pending"
	VerifyVerified Verification = "verified"
	VerifyFailed   Verification = "failed"
)

// Endpoint is one address of an account's server.
type Endpoint struct {
	ID        string
	AccountID string
	Name      string
	Kind      EndpointKind
	URL       string
	Main      bool
	// Approved is the first trust step: the user allowed sending the
	// credential to this origin (INV-03).
	Approved bool
	Verified Verification
	// Blocked is a hard block no score overrides (INV-04).
	Blocked bool
	// BlockedReason is the one reason code Blocked has, strongest first:
	// a security quarantine (TLS_ERROR, ENDPOINT_IDENTITY_MISMATCH,
	// ENDPOINT_UNTRUSTED), AUTH_REQUIRED, VERIFICATION_PENDING or
	// NETWORK_UNAVAILABLE; empty when not blocked (protocol 1.9).
	BlockedReason string `json:"blockedReason"`
	Reachable     bool
	// LatencyMs is the smoothed probe round trip; 0 before a probe succeeded.
	LatencyMs int `json:"latencyMs"`
	InUse     bool
}

// Reason is a code for why an address was chosen or a switch happened.
type Reason string

const (
	ReasonVerified      Reason = "verified"
	ReasonFaster        Reason = "faster"
	ReasonStable        Reason = "stable"
	ReasonJoinedNetwork Reason = "joinedNetwork"
	ReasonUnreachable   Reason = "unreachable"
	ReasonPostponed     Reason = "postponed"
	ReasonUserChoice    Reason = "userChoice"
)

// Network is how an account reaches its server now.
type Network struct {
	AccountID string
	Policy    Policy
	// FixedEndpointID is the address PolicyFixed always uses.
	FixedEndpointID string
	// RequestedEndpointID is a pending fixed target, not the actual or committed policy.
	RequestedEndpointID string
	ActiveID            string
	// Why lists the reasons for the active address, strongest first.
	Why []Reason
	// Speedup is how many times faster the active address answers than
	// the main one (0 when not faster).
	Speedup  int
	Switched time.Time
	// SwitchReason is why the last switch happened.
	SwitchReason Reason
	// Switching is a switch waiting for a safe point.
	Switching bool
}

// Switch is one change of address in an account's history.
type Switch struct {
	AccountID string
	At        time.Time
	FromID    string
	ToID      string
	Reason    Reason
	// Postponed is how long a switch waited for a transfer to finish.
	Postponed time.Duration
}

// DecisionKind is what a decision is about.
type DecisionKind string

const (
	// DecisionBulkDeletions: a cycle would delete many files; it waits for
	// the user (deletion protection, PLAN.md §11.2–§11.3).
	DecisionBulkDeletions DecisionKind = "bulkDeletions"
	// DecisionConflict: a file changed in both places; both were kept
	// (protocol 1.9: SyncD reports each conflict copy upstream kept).
	DecisionConflict DecisionKind = "conflict"
)

// Action answers a decision.
type Action string

const (
	// ActionLater keeps a proposal pending; nothing is deleted.
	ActionLater Action = "later"
	// ActionApproveDeletion authorizes one matching, unexpired recomputed proposal.
	ActionApproveDeletion Action = "approveDeletion"
	// ActionResolved closes a conflict notice; both copies stay.
	ActionResolved Action = "resolved"
)

// Decision waits for the user.
type Decision struct {
	ID        string
	Kind      DecisionKind
	AccountID string
	FolderID  string
	// Digest binds an answer to exactly this proposal.
	Digest string
	// Count is how many files a deletion proposal covers: LocalDeletes
	// would go on this PC, RemoteDeletes on the server.
	Count         int
	LocalDeletes  int
	RemoteDeletes int
	// Expires is when an approval stops being accepted; the folder is then
	// checked again and a new proposal made.
	Expires time.Time
	// Groups are what the proposal covers, when SyncD names it.
	Groups []FileGroup
	// File and Copy name a conflict's file and the copy kept beside it.
	File string
	Copy string
}

// FileGroup is a part of a proposal: a folder's files, or loose files.
type FileGroup struct {
	Name  string
	Files int
	Bytes int64
	// Loose is the files directly in the folder, not in a subfolder.
	Loose bool
}

// ActivityKind is what happened.
type ActivityKind string

const (
	ActUploaded   ActivityKind = "uploaded"
	ActDownloaded ActivityKind = "downloaded"
	ActDeleted    ActivityKind = "deletedRemote"
	ActSwitched   ActivityKind = "switched"
)

// Activity is one entry of the recent history.
type Activity struct {
	Cursor, AccountID, FromID, ToID, Reason string
	At                                      time.Time
	Kind                                    ActivityKind
	FolderID                                string
	// Name is a file's name, or an address's name for a switch.
	Name  string
	Count int
}

// Login is a Login Flow v2 transaction.
type Login struct {
	ID       string
	LoginURL string
	Expires  time.Time
}

// RemoteFolder is a folder at the top of an account's files.
type RemoteFolder struct {
	Path string
	// Encrypted folders (E2EE) cannot be synced yet.
	Encrypted bool
}

// Staging is what an address showed before any credential went to it.
type Staging struct {
	ID          string
	URL         string
	SecureTLS   bool
	CertSubject string
	CertIssuer  string
	// SameVersion: the server reports the same version as the main
	// address (a hint, never proof of identity).
	SameVersion bool
}

// Account returns the account with id.
func (s Snapshot) Account(id string) (Account, bool) {
	for _, a := range s.Accounts {
		if a.ID == id {
			return a, true
		}
	}
	return Account{}, false
}

// Folder returns the folder with id.
func (s Snapshot) Folder(id string) (Folder, bool) {
	for _, f := range s.Folders {
		if f.ID == id {
			return f, true
		}
	}
	return Folder{}, false
}

// Endpoint returns the address with id.
func (s Snapshot) Endpoint(id string) (Endpoint, bool) {
	for _, e := range s.Endpoints {
		if e.ID == id {
			return e, true
		}
	}
	return Endpoint{}, false
}

// Network returns how an account reaches its server.
func (s Snapshot) Network(accountID string) (Network, bool) {
	for _, n := range s.Networks {
		if n.AccountID == accountID {
			return n, true
		}
	}
	return Network{}, false
}
