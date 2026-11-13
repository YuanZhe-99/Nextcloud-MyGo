# Engine integration

## Ownership

Capability installation work now audits the upstream setter as well as the response format. A
headless `Account::create(false)` must remain headless when capabilities are installed: upstream
`setCapabilities` otherwise creates user-status services again and can enable push/status reporting.
The new narrow upstream regression failed `!hasUserStatus` before remediation. Patch 0015 preserves
the headless lifecycle across capability updates; its builds and consuming integration are in progress.
Runtime capability installation must use verified approved-origin metadata, keep unsupported background
features disabled, and retain the existing request-origin and journal gates. No modern chunked-upload
or portable cross-origin resume acceptance is claimed at this checkpoint.
The adapter loads approved-origin capability metadata once per runtime epoch before starting libsync,
validates the OCS envelope/version and modern DAV chunking declaration, and supplies only sync-related
capability groups. User-status, push, telemetry, theming and server-managed desktop configuration are
not imported. Cached metadata belongs to that immutable binding; rebinding requires a new load.

The B09–B12 network integration work connects Windows interface/address/route notifications and
resume events to generation-bound read-only health checks. DNS configuration is also polled without
recording interface names or addresses. Events invalidate evidence immediately and evaluation is
debounced for 2–10 seconds. A health sequence gives reaching the server (`status.php`) four seconds and the authenticated user check after it fifteen (`metadataDeadline`, also used by every metadata read), at most two sequences per account
and four globally; full UID/capability/all-root checks remain separate and mandatory before activation.
Ordinary health checks never read or write user content. Unknown route cost/preferences remain neutral;
strict VPN egress and metered-network enforcement are not advertised.
Interface/address/route callbacks only set an atomic notification bit and are drained with
`CancelMibChangeNotify2`. Suspend/resume uses a process-local message-only window on the control
thread, destroyed synchronously after unregistering. It creates no visible window, Shell entry or
system setting; no power callback can retain a freed Qt/control object. Every `WM_POWERBROADCAST`
(suspend and `PBT_APMRESUMEAUTOMATIC` alike) is a network change: earlier health is no longer
fresh and a probe begun before the sleep is not counted after it, whether or not the monotonic
clock counted the sleep (`resume` in `syncd_endpoint_policy_tests`, injected clock).
Probe evaluation retains owned endpoint identifiers across nested Qt metadata requests and reacquires
selection entries afterward. Removing an address cannot leave a dangling map key or authorize another
credentialed request: consent and frozen roots are rechecked between full-verification steps and before
the authenticated lightweight step. The real read-only removal regression reproduced ARM64 access
violation `0xC0000005` before remediation; its parent still cleaned the borrowed native credential.

Protocol 1.6 / Control schema 7 work adds durable endpoint policy. `auto` enables daemon proposals;
`fixed` reserves a switch and does not replace the old policy until successful reconciliation, including
paused roots after resume. Pending target and actual binding are separate projections. Existing profiles
without a policy retain their actual endpoint as fixed until explicitly changed. Failure-driven switches
request upstream cancellation with a 120-second drainage budget; healthy optimization still defers after
30 seconds rather than interrupting a healthy transfer. Verification and publication recheck network,
policy and binding revisions. Failed preparation may recover the previous binding only through fresh
verification and a newly allocated epoch. No old receipt becomes successful because recovery succeeded.
Repeated failed targets wait for fresh independent circuit-recovery samples rather than alternating
between bad addresses. Security quarantine survives restart and only explicit verification can clear it.
Implementation and acceptance of this connected path are in progress, not a capability claim.

Durable switching work in progress: Control schema 5 records switch preparation separately from the
committed endpoint binding. A target epoch is allocated monotonically inside the Control transaction;
committing the binding retains a reconciliation gate until every registered folder has a successful
new-epoch result. Restart changes unfinished intents to `blocked` without rolling back the binding or
replaying a file request. This storage layer does not itself authorize runtime activation.
Engine completion carries its immutable binding epoch, including preflight failure and cancellation.
Control ignores results from older bindings or restart-blocked intents before publishing status or
deletion decisions. A late successful completion therefore cannot turn a blocked folder green.

The connected manual-switch path (B11/B12, under acceptance) reserves the intent and receipt before
closing account admission. It waits up to 30 seconds for natural cycle completion, retires all old
accounts/managers/replies, freshly verifies the target UID and every mapped root, then commits the
binding with admission still closed. Runtime rebinding retains journals and root leases. Only matching
successful new-epoch cycles consume reconciliation membership; paused folders remain pending until
the user resumes them. Failure after retirement leaves a durable block, never an implicit resume.
`endpoints.requestSwitch` returns a running receipt; completion is observed through `operations.get`. Every user request also ends in Activity: `switched`, or `switchFailed` with its reason, such as `QUIESCE_STALLED` when the running cycle outlasts the 30 seconds and the address and policy stay ([ipc](ipc.md), protocol 1.10).
Automatic policy activation and complete fault recovery are still unfinished.

Interrupted-switch startup recovery uses the last committed endpoint, not an uncommitted target.
After reacquiring original root leases it reserves a new internal operation/epoch and runs the same
fresh verification and reconciliation path. The interrupted receipt stays failed. A previous hard
verification failure is not automatically retried. A process kill at each durable stage of a real
switch, followed by a restart on the same profile, confirmed this ([development](development.md),
`switch-kill`): killed with the intent stored, with a transfer still running on the old address, or
with the target's verification about to be sent, the daemon came back on the previous binding;
killed after the binding was published, before or after the first transfer of the new epoch, it came
back on the committed one. Every restart failed the interrupted receipt as `OPERATION_INTERRUPTED`,
allocated an epoch above both the old and the abandoned one, kept the credential reference, the
folder rows and the native journal file, and transferred only the file the kill had interrupted.
Same-origin recovery admits existing transfer rows to upstream recovery. Cross-origin rebinding admits only audited relative download state with a
nonempty ETag, single-PUT uncertainty state with a nonempty checksum, and chunking-v2 sessions with a
nonempty checksum; poll URLs and legacy v1 chunk indexes remain blocked. Admission never edits those rows.
Before publication, verification also requires the OCS files capability map and readable (`G`) DAV
home/mapped roots. It does not infer write permission: upstream discovery still enforces each item's
actual permissions. Every retired local root and read-only resume context is checked before the
Control commit and again by rebinding. TLS/identity failures mark alternative evidence rejected.

Connected-folder work in progress: Control schema 3 persists folder IDs, local volume/file identity,
remote root identity, endpoint-independent journal paths and per-folder pause. Inspection tokens are
short-lived and revalidated at add; removing a mapping preserves user files and its journal. Authenticated
DAV metadata reads require the approved HTTPS installation and do not follow redirects. Runtime recovery
must revalidate roots before scheduling. These changes require grouped daemon acceptance before support
is advertised.

B05 introduces an in-process adapter around the upstream `SyncEngine`, not a CLI subprocess.
The adapter owns one immutable account runtime, explicit journal path and one cycle. All Qt calls
remain on the control thread. Credentials are read from the already reserved native vault reference;
upstream URL-derived keychain persistence is not used. The network boundary forces manual redirects
and checks the approved installation before attaching credentials.

## Completion and safety

A failed health probe invalidates prior successful selection evidence immediately; circuit opening
still requires three independent transport failures. Business TLS/identity/authentication failures do
not depend on acquiring a health-probe ticket: they invalidate outstanding evidence even with a busy
probe or an open circuit. Security quarantine is persisted independently, and late successful probes
cannot clear it. Connected regressions reproduced lost quarantine before this fix (INV-04/06/14).

Only a failed probe blocks the active endpoint. While its circuit is still recovering (half-open), a
successful probe is recovery evidence and never blocks the account or fails the switch that just
activated it; a failed probe there still does. Before this rule a manual switch onto a recovering
address passed full verification and was then failed as `NETWORK_UNAVAILABLE` by its own successful
probe. The real active-transfer fixture found it, and a connected regression reproduces it (INV-06/14).

Real rebinding acceptance found a successful plaintext download leaving a resume row behind:
upstream `PropagateDownloadFile::updateMetadata` removed the empty encrypted-name key instead of
the plaintext path used to store that row. Patch 0014 uses the same file key as `setDownloadInfo` at
transfer start. ARM64 download regressions proved successful cleanup and retention of incomplete
transfer state, followed by real alternate-address rebinding with the original journal. The combined
fourteen-patch upstream suites passed on ARM64 and emulated x64; the switch gate remains fail-closed.

The standalone switch guard separates old-runtime drainage from new-epoch reconciliation (§9.3).
Unknown remote writes do not have to be recovered before committing the verified target; each frozen
folder instead remains pending reconciliation after commit. Only a matching new-epoch completion can
clear its recovery entry. Cancelling a preparation cannot erase committed recovery work.

Before rebinding a retired runtime, a read-only journal check rejects unreadable/corrupt schemas.
Cross-origin recovery admits relative download paths/temporary paths with nonempty ETags and
single-PUT upload rows (`transferid=0`, `chunk=0`, nonnegative size, positive mtime and a nonempty
checksum). Desktop v35.0.3's download path compares discovery ETags, reply ETags, ranges and content
length; its single-PUT discovery compares the persisted checksum before interpreting an uncertain
write. The adapter forces upstream remote rediscovery with the original journal before propagation.
A chunking-v2 session row (`transferid` nonzero, `chunk=0`, same size, mtime and checksum rules) is
admitted too. It stores no address: upstream addresses the session relative to the current account,
lists it on the target before sending anything, resumes after the last complete chunk, and adopts a
final MOVE the server already committed by checksum during discovery. Patch 0016 adds the two
upstream regressions for exactly that after an origin change. In a real run against server 35.0.2 a
112 MiB upload interrupted in its second chunk resumed on the other origin without resending the
first 100 MiB chunk; with the previous gate the same run could not leave the failed address.
Any pending poll URL, legacy v1 chunk index or unsupported/malformed transfer context is reported as
`RESUME_CONTEXT_UNSUPPORTED`; it is never removed or rewritten to force a switch. An absent journal
has no existing resume context. Same-origin recovery remains upstream-owned.

A lost reply is never answered by a second request from SyncD (`PLAN.md` §12.3). The adapter counts
the unanswered mutating request as an unconfirmed write, the cycle ends unsuccessfully with its
journal unchanged, and the next cycle on whichever origin is then bound starts with full remote
discovery. Real runs against server 35.0.2 lost the reply of three committed writes and let
another client create the affected path again before recovery. After a MOVE of an existing entry,
upstream found the renamed entry by its file ID, left it alone and downloaded the new object at the
old path. After a DELETE, it downloaded the re-created object instead of deleting it. After the
final MOVE of a 112 MiB chunk session, it adopted the assembled file by checksum and sent neither a
chunk nor a MOVE again. In each case the server file ID and ETag of the committed result were the
same after recovery, and the native journal file was the original one.
Relative resume paths also reject trailing-dot/space components and Windows wildcard/control
characters, rather than allowing Win32 normalization to reinterpret a purported child path. The
read-only regression rejected neither `.. /outside` nor its nested form before this boundary fix;
inspection still never modifies the journal or a temporary file.

Recovery must preserve a journal that fails SQLite integrity checks. Patch 0013 makes automatic
database recreation optional in the upstream SQL wrapper and disables it specifically for journals.
A malformed-journal regression requires repeated open failures to preserve the exact original bytes.
This does not repair the journal or authorize a full resync. The actual corrupt-byte preservation and
commit-failure tests passed; see [platform notes](platform-notes.md) for architecture-specific evidence.

Switch preparation requires an observable journal commit result. Patch 0011 adds a sticky
`hasCommitError()` observation to the upstream journal: a failed SQLite commit remains visible
through close/reopen of that object, and is never cleared by a later successful operation. The adapter
must report `JOURNAL_ERROR` instead of success when this flag is set. Its regression uses SQLite's
commit hook to reject a real transaction, verifies rollback and checks that close cannot hide failure.
The recorded thirteen-patch builds passed both upstream suites; see the platform notes.

`RequestLifetime` now observes active versus retained replies and counts unconfirmed mutating
outcomes, including destruction without `finished`. A later successful request cannot erase that
uncertainty. The engine adapter also counts live network managers and classifies denied origins,
TLS failures and HTTP 401 separately from generic cycle errors. Idle retirement permanently closes
the binding, checks journal commit state and drains deferred account/network destruction while
retaining the root lease. Shutdown waits for these observed lifetimes, with a bounded fatal-stop path
if old network resources cannot drain. These observations do not themselves prove remote reconciliation.
The retirement audit exposed an upstream ownership cycle: `Account::create()` assigns its
own strong pointer to `ClientSideEncryption::_account`. The adapter never initializes E2EE and now
clears that helper's account reference only after engine teardown, before releasing the account.
The real engine fixture checks zero live managers and replies at the shutdown callback; unit reply
tests alone cannot verify this account ownership behavior.
Further tracing found that `setSharedThis()` also starts `OcsUserStatusConnector`, which retains a
second strong account reference. Patch 0012 makes this interactive service optional at account
creation; SyncD requests the headless form. The default upstream client behavior stays enabled.
Its focused regression observes actual account destruction after the unused E2EE reference is cleared;
the real adapter fixture separately requires account, manager and reply counts to reach zero.

Successful cycle persistence converts a null Qt reason string to an empty SQL text value, preserving
the schema's non-null constraint. The two-folder daemon fixture checks terminal status as well as
transferred bytes, so a completed transfer with a stalled scheduler fails acceptance.
If the completion transaction itself fails, SyncD closes admission and exits after engine teardown
instead of silently leaving a live, permanently `syncing` projection. The UI disconnects and marks
state unavailable. Restart revalidates roots and rediscovers using the existing journal; it never
manufactures a successful receipt or repairs the journal to work around a Control DB failure.

An engine `finished` signal only starts teardown. The adapter destroys the engine and drains its
deferred discovery destruction before releasing the journal or reporting completion. It does not
claim that this alone proves endpoint-switch quiescence. Patch 0010 permits aborting before propagator
creation. Deletion proposals count descendants against the previous journal baseline separately for
each side; expiring one-use approval requires a matching rediscovery digest. Full sync stays capability-disabled until
folder ownership, propagation safety and real daemon acceptance are connected and verified.

## Progress, decisions and the update hold

Protocol 1.9 reads, without changing any decision, what the adapter already observes. While a
cycle runs, `EngineRuntime::progress` names its phase from the live request counters (an
unfinished upload reply is `uploading`, a download `downloading`, otherwise `checking`) and the
files upstream reported completed in this cycle; the runtime tick publishes a snapshot event at
most once a second when either changes. There is no total: discovery does not know one.

A held deletion proposal keeps, beside its digest, a summary by top-level entry (`groups`: a
directory counts the files the journal knows below it). There is no restore answer: bringing the
entries back would mean forgetting their journal records, which the behavior contract forbids;
the user decided on 2026-11-12 to keep it that way (files come back from the server's trash).

After each finished cycle the adapter reads the journal's conflict records (copy and original,
folder-relative) and Control keeps them as `conflict` decisions; a copy that is gone is forgotten,
and `resolved` closes a notice while its copy exists. Both files always stay.

`daemon.prepareUpdate` uses `EngineRuntime::hold`: admission freezes like a pause, but a running
cycle is not cancelled and finishes on its own; the user's own pause is kept apart and restored
when the hold ends.

## Scheduling

Folder attachment preserves stable credential, journal and root blockers. Unexpected internal
failures or failure to persist attachment errors stop engine admission without rewriting the journal.
The normal folder projection exposes the sanitized `reasonCode` independently of a user's pause;
the interface must not display that technical code as user-facing copy.

Preflight network and unavailable-root errors and server constraints (a full quota, a lock, a
throttle) retain pending work and retry after backoff; they do not permanently block a folder. A
full quota once blocked its folder for good, which also held every switch waiting for that folder's
reconciliation open. TLS, identity, credential, journal and unsupported-mode failures
remain independent safety blockers. A user's pause and a deletion decision survive retries.
Hard blockers now retain their individual reason instead of a shared safety bit. Verified credential
renewal clears only `AUTH_REQUIRED`/`CREDENTIALS_LOCKED` under closed admission, then reloads login and
the stable vault reference at a fresh same-origin epoch. It cannot clear a TLS, root or journal failure.

Metadata responses distinguish permission denial (403), remote locking (423) and server throttling
(429). Unavailable required DAV properties are not evidence of a different namespace. Internal
`MetadataError.retryAfterMs` is zero when unspecified, a bounded relative deadline with a 60-second
floor, or `-1` for a valid deadline beyond seven days/overflow; the last case must not become an early
automatic retry. Deadline interpretation uses UTC only to parse HTTP-date, then monotonic scheduling.
Account-wide consumer enforcement is under integration and is not yet an acceptance claim.

The follow-up adds durable account throttle storage in Control schema 8, restored before runtime
activation. Neither process restart, user wakeups nor a different approved origin may shorten a
server-imposed delay. Finite deadlines must be re-established conservatively on the monotonic clock;
indefinite deadlines remain blocked across restart. Persistence failure must close admission instead
of acknowledging a throttle that will be lost. Live-engine and reauthentication consumers remain
under integration; no complete N18 acceptance is claimed yet.

The serial queue uses caller-supplied monotonic time. Each folder has independent blocking reasons,
dirty and captured generations, a merged pending reason set and bounded local-change debounce.
Successful completion acknowledges only the captured generation; failed or cancelled work remains
pending. Account and folder rotation prevent immediate follow-up work from monopolizing the engine.
Resuming one pause never clears another blocker. `EngineRuntime` connects the queue to adapters,
polls at 60 seconds with ±20% jitter, requests daily full checks and retains failed generations with
bounded exponential backoff. Rebinding an account to another address drops its folders' retry
delay and failure count: the delay was earned on the old address, and it held reconciliation on
the new one for 47 s after a simulated sleep, and could for up to the 300-second backoff cap
(`sleep-resume` found it; `rebound` in `syncd_scheduler_tests`). Account
throttles and blockers stay. A shutdown freezes admission and waits for active teardown. Recursive
watching uses an overlapped Windows subtree subscription, including newly created directories.
Notifications only mark dirty work; discovery remains the authority. Overflow or notification loss
requests a full audit, with polling retained as a fallback. Watch handles are cancelled and drained
before destruction; callbacks remain on the control thread and cannot override pauses or blockers.
Durable Control/IPC folder registration is connected.

## Local root lease

`RootLease` admits only ordinary local fixed NTFS directories and rejects reparse ancestors and
existing upstream journals in the root or its ancestors. Native volume/file identities, a per-user
global admission mutex, exact-root mutexes and shared ancestor-presence events exclude equal or
nested SyncD mappings across processes and profiles of the same Windows user. The admission mutex
covers registration only; unrelated sibling roots remain concurrent. Root and ancestor directory
handles deny delete sharing, and identity is rechecked before each cycle. Drive aliases hiding
filesystem ancestors are refused. Ownership uses kernel handles rather than persistent records or
PIDs; release and process termination drop ownership, and kernel-object failures fail closed. This
does not detect future official-client registration. Recursive preflight and pre-propagation scans
continue rejecting reparse points, offline/recall objects, unreadable directories and upstream journals
inside the tree.

An on-demand folder's lease (`RootLease(path, true)`) also admits the Cloud Files reparse tag on the
root itself and Cloud Files placeholders in the tree, and does not list unpopulated directories;
every other reparse point still fails. Its adapter owns an `OnDemandRoot` between the journal and the
engine, gives its VFS to every cycle, and counts a running hydration as activity: `quiesceAccount`
closes the account's roots and reports quiescence only when no cycle and no on-demand request runs;
`retire` destroys an idle root before it releases the account; `rebind` connects a new one; a cycle
after a quiesce without rebinding reconnects a closed root ([on-demand](on-demand.md)).

## Build and acceptance

Startup helpers acquire the profile lock before reading an atomic crash ledger. Six observed unclean
exits within ten minutes suppress engine admission; damaged ledger data also fails closed. Before
Control schema migration, a read-only integrity check and independently validated SQLite snapshot
preserve committed WAL content without touching journals. These helpers have standalone ARM64/x64
regression coverage; connected startup and authenticated availability transitions have also passed
isolated dual-architecture regressions. Final shared rebuilds remain required after receipt refinement. Monotonic
2/5/15/60-second backoff never clears a user's stopped intent or pause.

Storage initialization is separate from runtime activation; authenticated IPC starts before recovery.
Unavailable snapshots omit application-state arrays. Deferred activation transactionally publishes a
strictly newer normal projection even for an empty database. Explicit initial launches may clear
stopped intent, never user pause. When Control storage is unavailable, one atomic JSON file retains
up to 32 diagnostic-shutdown receipts without eviction (32 KiB, depth 5). Historical retries cannot
stop a new boot; conflicting keys fail. Fresh keys at capacity refuse `QUIESCE_STALLED`. Healthy
Control storage imports every receipt in one transaction before removing the file; failed removal
allows idempotent re-import. Corrupt files remain untouched and failed appends never acknowledge work.

Pass `-UpstreamBuild` to `tools/build-syncd.ps1` to build the adapter against an already built,
patched upstream engine. `syncd_engine_acceptance.exe` is a dedicated fixture executable, not the
shipping daemon. Run `tools/test-upstream.ps1 -Executable <absolute-fixture-path> -EngineAdapter`
with the approved test environment and matching DLL paths. Its counters contain no URLs or secrets.
Native ARM64 adapter and subsequent runtime/root-lease runs against server 35.0.2 passed uploads,
downloads, modifications, conflict preservation, stable journal reuse and unchanged-cycle request
observation, with owned-resource cleanup. Four core/login/scheduler/root CTest targets passed on
ARM64 (0.84 seconds) and emulated x64 (0.91 seconds). The x64 adapter itself remains unverified.
Changed C++ files were clang-formatted; full clang-tidy could not run because its Python launcher
was unavailable. Full daemon sync is not yet claimed.

The subsequent `tools/test-login.ps1 -SyncFolders` run used the actual ARM64 daemon, protocol 1.2
and Desktop v35.0.3 with ten patches against server 35.0.2. Two-folder upload/download, independent
pause, deletion veto, matching one-use approval and cleanup passed. It also verified account restart
persistence and confirmed app-password revocation. Five connected CTest targets passed in 1.26 s.
This bounded scenario does not establish full recovery, switching or all §20 acceptance cases.
