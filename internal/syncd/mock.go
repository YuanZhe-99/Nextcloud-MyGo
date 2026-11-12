package syncd

import (
	"context"
	"fmt"
	"slices"
	"strings"
	"sync"
	"time"
)

// Mock is an in-memory SyncD for building and testing the interface before
// the real one exists: sample accounts, folders and addresses, commands
// that change them, and a sync cycle that moves on each Step. It says so
// (Daemon.Demo), and nothing it does touches files or the network.
type Mock struct {
	mu   sync.Mutex
	s    Snapshot
	now  func() time.Time
	subs map[int]func()
	next int
	// logins are the open Login Flow transactions: the server's host, and
	// the account a re-authentication is for.
	logins map[string]mockLogin
	staged map[string]mockStage
	seq    int
	// done counts the cycle each syncing folder ends after.
	cycle map[string]int
}

type mockLogin struct {
	server, accountID string
}

// NewMock returns a Mock. With sample true it holds the sample state of the
// design (design/README.md): two accounts, four folders, three addresses
// and two decisions; without, nothing, as on a first run.
func NewMock(sample bool, now func() time.Time) *Mock {
	if now == nil {
		now = time.Now
	}
	m := &Mock{now: now, subs: map[int]func(){}, logins: map[string]mockLogin{}, staged: map[string]mockStage{}, cycle: map[string]int{}}
	m.s = Snapshot{
		Daemon:   mockDaemon(now()),
		Accounts: []Account{}, Folders: []Folder{}, Endpoints: []Endpoint{}, Networks: []Network{}, Switches: []Switch{}, Decisions: []Decision{}, Activity: []Activity{},
	}
	if sample {
		m.s = sampleState(now())
		m.cycle["f-projects"] = 12
	}
	return m
}

// mockDaemon is the Mock's own description: connected, with every
// capability it simulates, and labelled as sample data.
func mockDaemon(now time.Time) Daemon {
	return Daemon{
		Connected: true, StateAvailable: true, StartupState: StartupNormal,
		Capabilities: map[string]bool{"login": true, "fullSync": true, "endpointAutoSwitch": true},
		Version:      "0.1.0", Protocol: 1, ProtocolMinor: 8, Demo: true, CheckedAt: now.Add(-2 * time.Minute),
	}
}

// sampleState is the design's sample: what the screens in design/ show.
func sampleState(now time.Time) Snapshot {
	return Snapshot{
		Daemon: mockDaemon(now),
		Accounts: []Account{
			{ID: "a-home", User: "alex", DisplayName: "Alex", Server: "cloud.example.com", SignedIn: true},
			{ID: "a-work", User: "alex", DisplayName: "Alex", Server: "work.example", SignedIn: false},
		},
		Folders: []Folder{
			{ID: "f-docs", AccountID: "a-home", Name: "Documents", LocalPath: `C:\Users\Alex\Documents`, RemotePath: "/Documents", State: FolderUpToDate},
			{ID: "f-projects", AccountID: "a-home", Name: "Projects", LocalPath: `D:\Projects`, RemotePath: "/Work/Projects", State: FolderSyncing, Phase: PhaseUploading, FilesDone: 3},
			{ID: "f-photos", AccountID: "a-home", Name: "Photos", LocalPath: `C:\Users\Alex\Pictures`, RemotePath: "/Photos", State: FolderNeedsReview},
			{ID: "f-team", AccountID: "a-work", Name: "Team share", LocalPath: `C:\Users\Alex\Team`, RemotePath: "/Shared/Team", State: FolderWaitingSignIn},
			{ID: "f-reports", AccountID: "a-work", Name: "Reports", LocalPath: `C:\Users\Alex\Reports`, RemotePath: "/Reports", State: FolderPaused, UserPaused: true},
		},
		Endpoints: []Endpoint{
			{ID: "e-lan", AccountID: "a-home", Name: "Home network", Kind: KindHome, URL: "https://cloud.lan.example", Approved: true, Verified: VerifyVerified, Reachable: true, LatencyMs: 8, InUse: true},
			{ID: "e-wan", AccountID: "a-home", Name: "Internet", Kind: KindInternet, URL: "https://cloud.example.com", Main: true, Approved: true, Verified: VerifyVerified, Reachable: true, LatencyMs: 64},
			{ID: "e-vpn", AccountID: "a-home", Name: "VPN", Kind: KindVPN, URL: "https://cloud.vpn.example", Approved: true, Verified: VerifyPending},
			{ID: "e-nas", AccountID: "a-home", Name: "Old NAS address", Kind: KindHome, URL: "https://nas.example.net", Approved: true, Verified: VerifyFailed, Blocked: true},
			{ID: "e-work", AccountID: "a-work", Name: "Internet", Kind: KindInternet, URL: "https://work.example", Main: true, Approved: true, Verified: VerifyVerified, Reachable: true, LatencyMs: 41, InUse: true},
		},
		Networks: []Network{
			{AccountID: "a-home", Policy: PolicyAuto, ActiveID: "e-lan", Why: []Reason{ReasonVerified, ReasonFaster, ReasonStable}, Speedup: 8, Switched: now.Add(-4 * time.Minute), SwitchReason: ReasonJoinedNetwork},
			{AccountID: "a-work", Policy: PolicyAuto, ActiveID: "e-work", Why: []Reason{ReasonVerified}},
		},
		Switches: []Switch{
			{AccountID: "a-home", At: now.Add(-4 * time.Minute), FromID: "e-wan", ToID: "e-lan", Reason: ReasonJoinedNetwork},
			{AccountID: "a-home", At: now.Add(-5*time.Hour - 19*time.Minute), FromID: "e-lan", ToID: "e-wan", Reason: ReasonUnreachable},
			{AccountID: "a-home", At: now.Add(-20 * time.Hour), FromID: "e-wan", ToID: "e-lan", Reason: ReasonPostponed, Postponed: 6 * time.Minute},
		},
		Decisions: []Decision{
			{ID: "d-photos", Kind: DecisionBulkDeletions, AccountID: "a-home", FolderID: "f-photos", Digest: "sha256:photos-142", Count: 142, RemoteDeletes: 142, Expires: now.Add(10 * time.Minute), Groups: []FileGroup{
				{Name: "Vacation 2025", Files: 96, Bytes: 1_800_000_000},
				{Name: "Screenshots", Files: 44, Bytes: 120_000_000},
				{Name: "Photos", Files: 2, Bytes: 8_000_000, Loose: true},
			}},
			{ID: "d-budget", Kind: DecisionConflict, AccountID: "a-home", FolderID: "f-docs", Digest: "sha256:budget", File: "Budget.xlsx", Copy: "Budget (conflicted copy 2026-11-03).xlsx"},
		},
		Activity: []Activity{
			{At: now.Add(-1 * time.Minute), Kind: ActUploaded, FolderID: "f-projects", Name: "site-plan.dwg"},
			{At: now.Add(-13 * time.Minute), Kind: ActDownloaded, FolderID: "f-docs", Count: 6},
			{At: now.Add(-20 * time.Minute), Kind: ActSwitched, Name: "Home network"},
			{At: now.Add(-27 * time.Minute), Kind: ActDeleted, FolderID: "f-docs", Name: "old-draft.docx"},
		},
	}
}

// Snapshot returns a copy of the state.
func (m *Mock) Snapshot() Snapshot {
	m.mu.Lock()
	defer m.mu.Unlock()
	s := m.s
	s.Accounts = slices.Clone(s.Accounts)
	s.Folders = slices.Clone(s.Folders)
	s.Endpoints = slices.Clone(s.Endpoints)
	s.Networks = slices.Clone(s.Networks)
	for i := range s.Networks {
		s.Networks[i].Why = slices.Clone(s.Networks[i].Why)
	}
	s.Switches = slices.Clone(s.Switches)
	s.Decisions = slices.Clone(s.Decisions)
	s.Activity = slices.Clone(s.Activity)
	return s
}

// Subscribe calls fn after every change.
func (m *Mock) Subscribe(fn func()) func() {
	m.mu.Lock()
	id := m.next
	m.next++
	m.subs[id] = fn
	m.mu.Unlock()
	return func() {
		m.mu.Lock()
		delete(m.subs, id)
		m.mu.Unlock()
	}
}

// Run moves the sync cycle on every interval until ctx ends.
func (m *Mock) Run(ctx context.Context, interval time.Duration) {
	t := time.NewTicker(interval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			m.Step()
		}
	}
}

// Step moves every syncing folder on by one file; a folder whose cycle is
// done is up to date, with its last file in the activity.
func (m *Mock) Step() {
	m.change(func() {
		for i := range m.s.Folders {
			f := &m.s.Folders[i]
			if f.State != FolderSyncing {
				continue
			}
			f.FilesDone++
			end := m.cycle[f.ID]
			if end == 0 {
				end = 4
			}
			if f.FilesDone < end {
				continue
			}
			kind := ActUploaded
			if f.Phase == PhaseDownloading {
				kind = ActDownloaded
			}
			m.s.Activity = slices.Insert(m.s.Activity, 0, Activity{At: m.now(), Kind: kind, FolderID: f.ID, Count: f.FilesDone})
			f.State, f.Phase, f.FilesDone = FolderUpToDate, "", 0
			delete(m.cycle, f.ID)
			m.s.Daemon.CheckedAt = m.now()
		}
	})
}

// change applies fn under the lock, then tells the subscribers.
func (m *Mock) change(fn func()) {
	m.mu.Lock()
	fn()
	subs := make([]func(), 0, len(m.subs))
	for _, s := range m.subs {
		subs = append(subs, s)
	}
	m.mu.Unlock()
	for _, s := range subs {
		s()
	}
}

// folder returns the folder with id (under the lock).
func (m *Mock) folder(id string) *Folder {
	for i := range m.s.Folders {
		if m.s.Folders[i].ID == id {
			return &m.s.Folders[i]
		}
	}
	return nil
}

// settle sets a folder's state from what blocks it, the user's pause first
// (INV-13), then a decision, then the account's sign-in.
func (m *Mock) settle(f *Folder) {
	acc, _ := m.s.Account(f.AccountID)
	switch {
	case f.UserPaused || m.s.Daemon.AllPaused:
		f.State = FolderPaused
	case m.pendingDeletion(f.ID):
		f.State = FolderNeedsReview
	case !acc.SignedIn:
		f.State = FolderWaitingSignIn
	case f.State != FolderSyncing:
		f.State, f.Phase, f.FilesDone = FolderSyncing, PhaseChecking, 0
	}
}

func (m *Mock) pendingDeletion(folderID string) bool {
	for _, d := range m.s.Decisions {
		if d.FolderID == folderID && d.Kind == DecisionBulkDeletions {
			return true
		}
	}
	return false
}

func (m *Mock) PauseAll() error {
	m.change(func() {
		m.s.Daemon.AllPaused = true
		for i := range m.s.Folders {
			m.settle(&m.s.Folders[i])
		}
	})
	return nil
}

func (m *Mock) ResumeAll() error {
	m.change(func() {
		m.s.Daemon.AllPaused = false
		for i := range m.s.Folders {
			m.settle(&m.s.Folders[i])
		}
	})
	return nil
}

func (m *Mock) PauseFolder(id string) error  { return m.setPaused(id, true) }
func (m *Mock) ResumeFolder(id string) error { return m.setPaused(id, false) }

func (m *Mock) setPaused(id string, paused bool) error {
	var err error
	m.change(func() {
		f := m.folder(id)
		if f == nil {
			err = ErrNotFound
			return
		}
		f.UserPaused = paused
		m.settle(f)
	})
	return err
}

// SyncNow starts a cycle unless something blocks the folder, which it
// then reports as ErrRefused.
func (m *Mock) SyncNow(id string) error {
	var err error
	m.change(func() {
		f := m.folder(id)
		switch {
		case f == nil:
			err = ErrNotFound
		case f.State == FolderSyncing:
		case f.State != FolderUpToDate && f.State != FolderOffline && f.State != FolderError:
			err = ErrRefused
		default:
			f.State, f.Phase, f.FilesDone = FolderSyncing, PhaseChecking, 0
		}
	})
	return err
}

func (m *Mock) RemoveFolder(id string) error {
	var err error
	m.change(func() {
		i := slices.IndexFunc(m.s.Folders, func(f Folder) bool { return f.ID == id })
		if i < 0 {
			err = ErrNotFound
			return
		}
		m.s.Folders = slices.Delete(m.s.Folders, i, i+1)
		m.s.Decisions = slices.DeleteFunc(m.s.Decisions, func(d Decision) bool { return d.FolderID == id })
	})
	return err
}

// ResolveDecision answers a decision once, as SyncD does: Later keeps it
// pending, an approval consumes it and the folder syncs the deletions; a
// second answer to the same proposal is refused.
func (m *Mock) ResolveDecision(id, digest string, action Action) error {
	var err error
	m.change(func() {
		i := slices.IndexFunc(m.s.Decisions, func(d Decision) bool { return d.ID == id })
		if i < 0 {
			err = ErrNotFound
			return
		}
		d := m.s.Decisions[i]
		switch {
		case d.Digest != digest || m.now().After(d.Expires) && !d.Expires.IsZero():
			err = ErrStale
			return
		case action == ActionLater:
			return
		case d.Kind == DecisionBulkDeletions && action != ActionApproveDeletion,
			d.Kind == DecisionConflict && action != ActionResolved:
			err = ErrRefused
			return
		}
		m.s.Decisions = slices.Delete(m.s.Decisions, i, i+1)
		if d.Kind != DecisionBulkDeletions {
			return
		}
		m.s.Activity = slices.Insert(m.s.Activity, 0, Activity{At: m.now(), Kind: ActDeleted, FolderID: d.FolderID, Count: d.Count})
		if f := m.folder(d.FolderID); f != nil {
			f.State = FolderUpToDate
			m.settle(f)
		}
	})
	return err
}

func (m *Mock) id(prefix string) string {
	m.seq++
	return fmt.Sprintf("%s-%d", prefix, m.seq)
}

func (m *Mock) StartLogin(serverURL string) (Login, error) {
	host := hostOf(serverURL)
	if host == "" {
		return Login{}, ErrRefused
	}
	var l Login
	m.change(func() {
		l = Login{ID: m.id("login"), LoginURL: "https://" + host + "/index.php/login/v2/flow/sample", Expires: m.now().Add(20 * time.Minute)}
		m.logins[l.ID] = mockLogin{server: host}
	})
	return l, nil
}

// WaitLogin finishes a login three seconds after it began, as if the user
// granted access in the browser.
func (m *Mock) WaitLogin(ctx context.Context, loginID string) (string, error) {
	select {
	case <-ctx.Done():
		return "", ctx.Err()
	case <-time.After(3 * time.Second):
	}
	var accountID string
	var err error
	m.change(func() {
		l, ok := m.logins[loginID]
		if !ok {
			err = ErrNotFound
			return
		}
		delete(m.logins, loginID)
		if l.accountID != "" {
			accountID = l.accountID
			for i := range m.s.Accounts {
				if m.s.Accounts[i].ID == accountID {
					m.s.Accounts[i].SignedIn = true
				}
			}
			for i := range m.s.Folders {
				if m.s.Folders[i].AccountID == accountID {
					m.settle(&m.s.Folders[i])
				}
			}
			return
		}
		accountID = m.id("a")
		epID := m.id("e")
		m.s.Accounts = append(m.s.Accounts, Account{ID: accountID, User: "alex", DisplayName: "Alex", Server: l.server, SignedIn: true})
		m.s.Endpoints = append(m.s.Endpoints, Endpoint{ID: epID, AccountID: accountID, Name: "Internet", Kind: KindInternet, URL: "https://" + l.server, Main: true, Approved: true, Verified: VerifyVerified, Reachable: true, LatencyMs: 50, InUse: true})
		m.s.Networks = append(m.s.Networks, Network{AccountID: accountID, Policy: PolicyAuto, ActiveID: epID, Why: []Reason{ReasonVerified}})
	})
	return accountID, err
}

func (m *Mock) CancelLogin(id string) error {
	m.change(func() { delete(m.logins, id) })
	return nil
}

func (m *Mock) Reauthenticate(accountID string) (Login, error) {
	acc, ok := m.Snapshot().Account(accountID)
	if !ok {
		return Login{}, ErrNotFound
	}
	l, err := m.StartLogin(acc.Server)
	if err == nil {
		m.change(func() { m.logins[l.ID] = mockLogin{server: acc.Server, accountID: accountID} })
	}
	return l, err
}

func (m *Mock) RemoveAccount(id string) error {
	m.change(func() {
		m.s.Accounts = slices.DeleteFunc(m.s.Accounts, func(a Account) bool { return a.ID == id })
		m.s.Folders = slices.DeleteFunc(m.s.Folders, func(f Folder) bool { return f.AccountID == id })
		m.s.Endpoints = slices.DeleteFunc(m.s.Endpoints, func(e Endpoint) bool { return e.AccountID == id })
		m.s.Networks = slices.DeleteFunc(m.s.Networks, func(n Network) bool { return n.AccountID == id })
		m.s.Decisions = slices.DeleteFunc(m.s.Decisions, func(d Decision) bool { return d.AccountID == id })
	})
	return nil
}

func (m *Mock) RemoteFolders(string) ([]RemoteFolder, error) {
	return []RemoteFolder{{Path: "/Documents"}, {Path: "/Photos"}, {Path: "/Work"}, {Path: "/Vault", Encrypted: true}}, nil
}

func (m *Mock) AddFolder(accountID, remotePath, localPath string, mode ...FolderMode) error {
	m.change(func() {
		name := strings.TrimPrefix(remotePath, "/")
		if i := strings.LastIndex(name, "/"); i >= 0 {
			name = name[i+1:]
		}
		f := Folder{ID: m.id("f"), AccountID: accountID, Name: name, LocalPath: localPath, RemotePath: remotePath, Mode: FolderFull}
		if len(mode) > 0 {
			f.Mode = mode[0]
		}
		m.settle(&f)
		f.Phase = PhaseDownloading
		m.s.Folders = append(m.s.Folders, f)
	})
	return nil
}

// RequestPolicy changes the policy at once: the Mock has no transfer to
// wait for.
func (m *Mock) RequestPolicy(accountID string, p Policy, fixed string) (string, error) {
	var err error
	var op string
	m.change(func() {
		for i := range m.s.Networks {
			n := &m.s.Networks[i]
			if n.AccountID != accountID {
				continue
			}
			n.Policy, n.FixedEndpointID = p, ""
			if p == PolicyFixed {
				if fixed == "" {
					fixed = n.ActiveID
				}
				n.FixedEndpointID = fixed
				if fixed != n.ActiveID {
					m.s.Switches = slices.Insert(m.s.Switches, 0, Switch{AccountID: accountID, At: m.now(), FromID: n.ActiveID, ToID: fixed, Reason: ReasonUserChoice})
					for j := range m.s.Endpoints {
						e := &m.s.Endpoints[j]
						if e.AccountID == accountID {
							e.InUse = e.ID == fixed
						}
					}
					n.ActiveID, n.Why, n.Speedup, n.Switched, n.SwitchReason = fixed, []Reason{ReasonUserChoice, ReasonVerified}, 0, m.now(), ReasonUserChoice
				}
			}
			op = m.id("op")
			return
		}
		err = ErrNotFound
	})
	return op, err
}

func (m *Mock) StageEndpoint(accountID, name, url string, kind ...EndpointKind) (Staging, error) {
	host := hostOf(url)
	if host == "" {
		return Staging{}, ErrRefused
	}
	var st Staging
	m.change(func() {
		st = Staging{ID: m.id("stage"), URL: "https://" + host, SecureTLS: true, CertSubject: host, CertIssuer: "Sample CA", SameVersion: true}
		stage := mockStage{st: st, accountID: accountID, name: name, kind: KindInternet}
		if len(kind) > 0 {
			stage.kind = kind[0]
		}
		m.staged[st.ID] = stage
	})
	return st, nil
}

// ApproveEndpoint adds the staged address, approved and not verified yet;
// the verification ends two seconds later.
func (m *Mock) ApproveEndpoint(stagingID string) (string, error) {
	var id string
	var err error
	m.change(func() {
		sg, ok := m.staged[stagingID]
		if !ok {
			err = ErrNotFound
			return
		}
		delete(m.staged, stagingID)
		id = m.id("e")
		// Without an explicit kind the sample data guesses one from the name.
		kind := sg.kind
		if l := strings.ToLower(sg.name); kind == KindInternet && strings.Contains(l, "vpn") {
			kind = KindVPN
		} else if kind == KindInternet && (strings.Contains(l, "home") || strings.Contains(l, "lan")) {
			kind = KindHome
		}
		m.s.Endpoints = append(m.s.Endpoints, Endpoint{ID: id, AccountID: sg.accountID, Name: sg.name, Kind: kind, URL: sg.st.URL, Approved: true, Verified: VerifyPending})
	})
	if err == nil {
		go m.finishVerify(id)
	}
	return id, err
}

func (m *Mock) VerifyEndpoint(id string) error {
	var err error
	m.change(func() {
		err = ErrNotFound
		for i := range m.s.Endpoints {
			if m.s.Endpoints[i].ID == id {
				m.s.Endpoints[i].Verified, err = VerifyPending, nil
			}
		}
	})
	if err == nil {
		go m.finishVerify(id)
	}
	return err
}

func (m *Mock) finishVerify(id string) {
	time.Sleep(2 * time.Second)
	m.change(func() {
		for i := range m.s.Endpoints {
			e := &m.s.Endpoints[i]
			if e.ID == id && e.Verified == VerifyPending {
				e.Verified, e.Reachable, e.LatencyMs = VerifyVerified, true, 30
			}
		}
	})
}

func (m *Mock) RemoveEndpoint(id string) error {
	var err error
	m.change(func() {
		i := slices.IndexFunc(m.s.Endpoints, func(e Endpoint) bool { return e.ID == id })
		switch {
		case i < 0:
			err = ErrNotFound
		case m.s.Endpoints[i].InUse || m.s.Endpoints[i].Main:
			err = ErrRefused
		default:
			m.s.Endpoints = slices.Delete(m.s.Endpoints, i, i+1)
		}
	})
	return err
}

// PrepareUpdate refuses while files move or a deletion waits for the
// user, as SyncD defers an update then.
func (m *Mock) PrepareUpdate(string) error {
	s := m.Snapshot()
	for _, f := range s.Folders {
		if f.State == FolderSyncing || f.State == FolderNeedsReview {
			return ErrBusy
		}
	}
	return nil
}

func (m *Mock) Shutdown() error { return nil }

// Diagnostics reports the sample folders with no transfers.
func (m *Mock) Diagnostics(context.Context) (RuntimeDiagnostics, error) {
	s := m.Snapshot()
	d := RuntimeDiagnostics{Version: s.Daemon.Version, Protocol: "1.8", NetworkGeneration: "1", AllPaused: s.Daemon.AllPaused, StateAvailable: true, StartupState: StartupNormal}
	for _, f := range s.Folders {
		d.Folders = append(d.Folders, FolderDiagnostics{FolderID: f.ID, AccountID: f.AccountID, Epoch: "1", State: string(f.State), ReasonCode: f.ReasonCode, UserPaused: f.UserPaused})
	}
	return d, nil
}

// ExportDiagnostics writes nothing: sample data has no bundle.
func (m *Mock) ExportDiagnostics() (string, error) { return "", ErrRefused }

var _ Client = (*Mock)(nil)

// hostOf is the host of an address typed with or without its scheme.
func hostOf(s string) string {
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(strings.TrimPrefix(s, "https://"), "http://")
	if i := strings.IndexAny(s, "/?#"); i >= 0 {
		s = s[:i]
	}
	if strings.ContainsAny(s, " \\") {
		return ""
	}
	return s
}

// mockStage is a staged address and whose it is to become.
type mockStage struct {
	st              Staging
	accountID, name string
	kind            EndpointKind
}
