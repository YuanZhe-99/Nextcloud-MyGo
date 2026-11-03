// Package syncd is the interface's side of the contract with SyncD, the
// background sync process (PLAN.md §13): the state SyncD reports and the
// commands it takes. Mock stands in for SyncD until the Named Pipe client
// exists; both satisfy Client.
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
	AddFolder(accountID, remotePath, localPath string) error

	SetPolicy(accountID string, policy Policy, fixedEndpointID string) error
	// StageEndpoint looks at an address without sending any credential
	// (endpoints.stage).
	StageEndpoint(accountID, name, url string) (Staging, error)
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
}

// Errors the interface tells apart.
var (
	ErrStale    = errors.New("syncd: the decision changed; review it again")
	ErrBusy     = errors.New("syncd: not at a safe point yet")
	ErrNotFound = errors.New("syncd: no such item")
	ErrRefused  = errors.New("syncd: refused")
)

// Snapshot is SyncD's whole state as the interface shows it.
type Snapshot struct {
	Daemon    Daemon
	Accounts  []Account
	Folders   []Folder
	Endpoints []Endpoint
	Networks  []Network
	Switches  []Switch
	Decisions []Decision
	Activity  []Activity
}

// Daemon is SyncD itself.
type Daemon struct {
	Version  string
	Protocol int
	// Demo is true for Mock: the interface says it shows sample data.
	Demo bool
	// AllPaused is the user's pause of every folder.
	AllPaused bool
	// CheckedAt is when the last full check of every folder ended.
	CheckedAt time.Time
}

// Account is one Nextcloud account: its identity never depends on the
// address used to reach it (INV-02).
type Account struct {
	ID          string
	User        string
	DisplayName string
	// Server is the canonical address's host, as people know it.
	Server   string
	SignedIn bool
}

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
	// UserPaused is kept apart from every other blocking reason, which
	// never overrides it (INV-13).
	UserPaused bool
	Phase      Phase
	// FilesDone counts the files of the current cycle so far; SyncD gives
	// no total while discovery runs, so there is none to show.
	FilesDone int
}

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
	Blocked   bool
	Reachable bool
	LatencyMs int
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
	ActiveID        string
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
	// DecisionLocalDeletions: many files were deleted on this PC; their
	// deletion on the server waits for the user (deletion protection).
	DecisionLocalDeletions DecisionKind = "localDeletions"
	// DecisionConflict: a file changed in both places; both were kept.
	DecisionConflict DecisionKind = "conflict"
)

// Action answers a decision.
type Action string

const (
	ActionRestore      Action = "restore"
	ActionDeleteRemote Action = "deleteRemote"
	ActionLater        Action = "later"
	ActionResolved     Action = "resolved"
)

// Decision waits for the user.
type Decision struct {
	ID        string
	Kind      DecisionKind
	AccountID string
	FolderID  string
	// Digest binds an answer to exactly this proposal.
	Digest string
	// Count is how many files a deletion proposal covers.
	Count  int
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
	At       time.Time
	Kind     ActivityKind
	FolderID string
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
