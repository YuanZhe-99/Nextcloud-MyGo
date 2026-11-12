# SyncD IPC

## Contract

Protocol 1.10 uses JSON-RPC 2.0 over a local Named Pipe: a four-byte little-endian length followed
by UTF-8 JSON, at most 1 MiB and 64 nested containers. Duplicate object keys are invalid.
`protocol/contract.json` records methods and capability gates. `protocol/fixtures.json` is shared
by Go and C++ contract tests. The wire projection uses lower-camel-case field names.
Fixture `valid` means JSON-boundary acceptance; RPC envelope/method validation is tested separately.

## Sessions and state

The client negotiates `session.hello` before `state.bootstrap`. Bootstrap atomically returns
`bootId`, decimal `seq`, decimal configuration `revision`, and `snapshot`, and subscribes the
connection. Persistent state changes publish an `event` notification carrying the new projection.
Reconnect always bootstraps. A disconnected UI marks folders offline and never reports stale
green status. Closing the client does not shut down SyncD.

## Commands

Mutations carry a UUID `operationKey`; configuration edits also carry `expectedRevision`.
The daemon stores the request digest, result and state event in one SQLite transaction before
replying. Reusing a key with different input is a conflict. A lost reply leaves the operation
uncertain; reconnect retries the same key. The client polls `operations.get` for accepted work.
Unsupported capabilities return `UNSUPPORTED_CAPABILITY`, never simulated success.

## Implementation scope

Protocol 1.10, with Control schema 11 unchanged, completes the history (schema `$defs/activity` in
`protocol/message.schema.json`, examples in `protocol/state.examples.json`). Activity entries carry
`cursor`, `accountId`, `folderId`, `at`, `kind`, `count`, `fromId`, `toId` and `reason`, and `name`
for a switch. Four kinds are new: `deletedLocal` (entries removed on this PC because the server
removed them), `renamedRemote` and `renamedLocal` (renames or moves applied on the server and on
this PC; each counts upstream's completed rename items, a directory as one), and `switchFailed`: a
switch the user asked for (`endpoints.requestSwitch` or a fixed `endpoints.setPolicy`) that did not
happen, with `fromId` still in use, `toId` the address asked for, its `name`, and the `reason` code.
A user's request therefore always ends in the history, as `switched` or `switchFailed`; while it
waits for a running cycle the network shows `switching`, `why` with `postponed` and, for a policy,
`requestedEndpointId`. When the cycle outlasts the 30-second drain window the request ends as
`switchFailed` with `QUIESCE_STALLED`, the address and policy unchanged (the window, the request
and its receipt were as before; only the record is new). Automatic switches record no failure entry.
Diagnostics, the startup diagnostics and `session.hello` all report the one `Control::protocolMinor`
(`testProtocolVersion` compares them with `protocol/contract.json`).

Protocol 1.9, with Control schema 11, adds what the interface needs and what the installer needs
(examples in `protocol/state.examples.json`, schema in `protocol/message.schema.json`):

- Several accounts. A second account on the same or another server is accepted; the same user on
  the same server (same UID and DAV root identity, whatever address was used) is refused as
  `ACCOUNT_EXISTS` once its login proved who it is, and the password that login acquired is
  retired. The `multiAccount` capability reports this ([accounts](accounts.md)).
- Account removal is never refused for a busy account: `accounts.remove` stops the account's
  sign-ins, engines and checks, removes its mappings (files and journals stay) and then revokes as
  before.
- `account.auth` is `signedIn`, `needsSignIn` (the credential cannot be used on this computer, or a
  renewal waits for the browser) or `revoked` (the server refused the app password); `signedIn`
  stays as `auth == signedIn`.
- `folder.phase` is `checking`, `uploading` or `downloading` while a cycle runs and empty
  otherwise; `folder.filesDone` counts the cycle's completed files. A running cycle publishes a
  snapshot event at most once a second when either changes.
- `endpoint.latencyMs` (smoothed probe round trip, 0 before a probe succeeded), `endpoint.kind`
  (`internet`, `home`, `vpn`, chosen at `endpoints.stage` with the optional `kind`; it never
  affects trust) and `endpoint.blockedReason`, the one reason `blocked` is true, strongest first:
  a security quarantine, `AUTH_REQUIRED`, `VERIFICATION_PENDING` or `NETWORK_UNAVAILABLE`.
- `network.why` (why the address in use was chosen), `network.switchReason` and
  `network.switched` (the last committed switch).
- Bulk deletions carry `groups` (at most eight top-level entries with file counts, loose files
  as one group). Conflict copies upstream kept are decisions of
  kind `conflict` with `file` and `copy`; `resolved` closes one and both copies stay.
- `daemon.prepareUpdate(targetVersion)` is dispatched. It refuses `DECISION_PENDING` while a
  deletion waits for the user and `QUIESCE_STALLED` while a switch, renewal, address check,
  sign-in or removal runs; otherwise no new cycle starts, the running one finishes on its own, a
  consistent Control copy `control.pre-update.<id>.sqlite` is written (the newest only) and the
  receipt succeeds with `{"ready": true}`. The hold lasts ten minutes or until shutdown.
  `daemon.startupReport` then says what a start migrated from (`previousVersion`,
  `previousSchema`, `schema`) and whether its self-check passed (`selfCheck`: pending, passed or
  failed, with `reasonCodes`). It checks only this start: the database, every ready account's credential and every mapping that did not attach again; a reason the previous process's last cycle left is not a failure of it, and a cycle stopped at shutdown (`CANCELLED`) no longer leaves its folder in error.
- The `engine` capability separates a build linked to the upstream engine from a foundation build.
- `folders.inspect` accepts a local folder that does not exist yet when its parent does
  (`localExists` false); `folders.add` then creates it empty.

The Go client sends `minor: 10`, maps `ACCOUNT_EXISTS` to `ErrAccountExists` and `DECISION_PENDING`
to `ErrBusy`, and offers `ConnectRemote`, which never starts SyncD, and `Remote.Disconnect`, a
stable phrase for Diagnostics saying why the last connection ended. A call made while no
connection is ready returns `ErrDisconnected`: it still matches `ErrBusy` for the interface, but a
daemon that is gone is never reported as one at an unsafe point. `nextcloud-mygo-syncd.exe
--version` prints one JSON line with the version, protocol and Control schema and starts nothing
([packaging](packaging.md)).

Protocol 1.8 adds the folder file mode ([on-demand](on-demand.md)). `folders.add` takes an optional
`mode`, `full` (the default) or `onDemand`; any other value, or a non-string, is `INVALID_PARAMS`, and
`onDemand` without the CFAPI plugin is `UNSUPPORTED_CAPABILITY`. The mode is fixed for the folder's
lifetime, stored by Control schema 10 (`folders.mode`, existing rows `full`), and every folder
projection carries it. Removing an on-demand folder first ends its Cloud Files registration and refuses
`QUIESCE_STALLED` while one of its hydrations runs. The `virtualFiles` capability stays false: the
on-demand path has run on native ARM64 against a real server through SyncD's engine runtime, not yet
through the paired daemon and interface. See `protocol/folders.schema.json` (`addParams`) and
`protocol/folders.examples.json` (`addOnDemand`). The Go client decodes `Folder.Mode` and `AddFolder`
takes an optional mode; a schema-9 mapping migrates as `full` (`testFolderModeMigration`). The interface
offers the choice in setup only with `virtualFiles` and sends no mode without it ([interface](gui.md)).

Protocol 1.7 adds explicit startup availability and stable folder `reasonCode` projections. Control
schema 8 is being integrated for durable account-level server throttles (the preceding startup
acceptance used schema 7). Control schema 9 adds only the `held_revocations` table
([accounts](accounts.md)); no method, field, event or error of protocol 1.7 changed with it.
`snapshot.daemon.stateAvailable=false` means the account/file state is unknown:
the snapshot omits all account/folder/endpoint/network/switch/decision/activity arrays instead of
reporting empty success. `startupState` is `normal`, `backoff`, `diagnostic`, `stopped` or
`databaseUnavailable`. Authenticated diagnostic bootstrap and `diagnostics.snapshot` remain readable;
state-dependent calls refuse `STATE_UNAVAILABLE`. Normal transport readiness is separate from healthy
application state. No diagnostic or backoff state may produce a green sync status. The client also
refuses contradictory `stateAvailable=true` with any non-`normal` startup state. The connected
startup contracts and authenticated pipe transition tests have passed on both architectures; the
broader recovery matrix remains in progress, not a support claim. See
`protocol/startup.examples.json` and the projection schema.
RPC readiness is independent of healthy UI connectivity: an unavailable bootstrap keeps diagnostics
and explicit shutdown callable, marks the UI disconnected and clears cached application collections
to unknown. Legacy availability inference requires explicit pre-1.7 negotiation.
Database-unavailable shutdown retains at most 32 durable receipts in one atomically written file,
without eviction. Fresh keys at capacity refuse `QUIESCE_STALLED`; old-key retries remain truthful
and never stop a different boot. All receipts import transactionally into recovered Control storage
before file removal. No account/file state is inferred from this narrow diagnostic receipt store.

The same unreleased 1.7 contract distinguishes `PERMISSION_DENIED`, `REMOTE_LOCKED` and
`SERVER_THROTTLED` from transport/authentication failure. Retry deadlines remain internal, not IPC
fields. These constraints must not trigger endpoint fallback or quarantine; a server throttle applies
to the entire account, not just an origin. Consumer implementation and acceptance are in progress.
See `protocol/constraints.examples.json`.

Mutation retries retain the original operation key, parameters and expected revision across
reconnect/bootstrap. Cancelling an operation wait reports the context error and never cancels
accepted daemon work; observation can resume using the same operation ID. A transport EOF caused
by cancellation must not hide that cancellation or imply rollback (INV-12).

`diagnostics.export` creates a user-requested local JSON support bundle containing only the sanitized
runtime snapshot (versions, IDs, epochs, states, blockers and counters) and creation time. It never
copies logs, journals, databases, file contents or credential stores. Empty `options` is the only
accepted option set. The operation returns the generated local `path` for Diagnostics, not the main
interface. Bundles are atomic files in the profile's `diagnostics` directory; only the latest five
owned bundle files are retained. Retention requires a matching recorded export operation; unrelated
UUID-named files are never treated as owned bundles. Operation-key retries return the same receipt/path. An interrupted
export is failed on restart, never silently replayed. This minimal bundle does not claim a full logging
or telemetry implementation; nothing is uploaded automatically.

The bounded activity history stores aggregate confirmed upstream item completions and committed
switches, not filenames, paths or request bodies. `activity.list` accepts an optional decimal exclusive
`cursor`, `accountId`/`folderId` filters and `limit` (default 100, maximum 500), returning newest first.
Entries carry a decimal cursor for pagination. At most 1,000 records are retained. Snapshot activity
contains the most recent 100 entries. Partial-cycle counts describe only items upstream actually
reported successful, not the outcome of an interrupted request; they never authorize propagation.

Protocol 1.6 adds durable `endpoints.setPolicy` and a separate pending `requestedEndpointId` network
field. Fixed configuration commits only after all-root new-epoch reconciliation. Nonstring
`fixedEndpointId` values are rejected even for automatic policy; null is not an omitted/empty target.
Accepted operations remain observable after a caller stops waiting. `Remote.RequestPolicy` returns acceptance independently
of `WaitOperation`. Schema/examples live in `protocol/policy.*`. Control schema 7 persists policy,
pending policy, security quarantine and frozen switch-evidence context. Existing implicit fixed profiles
are not silently opted into automatic selection. This connected path is under real fault acceptance;
the automatic-switching capability remains false until the full agreed scope is verified.

`diagnostics.snapshot` returns only sanitized IDs, immutable epochs and aggregate request/drainage
counters. It never returns request URLs, headers, passwords, paths or file content, and is not a
replacement for a user-triggered support-package export. Active upload/download counters count actual
unfinished content replies, not elapsed UI status; uncertainty is not proof a remote write failed.

Control schema 6 adds account-preserving reauthentication reservations and native-vault replacement
recovery. `accounts.reauthenticate` takes the account ID, expected revision and operation key; its
browser session is polled through `accounts.login.status`. A cleanup warning never conceals an
installed replacement: status includes the existing account ID and a stable warning reason. See
[accounts](accounts.md) and `protocol/reauthentication.schema.json` for phases and warning semantics.

Control schema 5 adds durable endpoint bindings and switch intents. Startup blocks interrupted
intents and fails their receipts in the same Control transaction, preserving any committed epoch.
Folder attachment resolves the committed endpoint through current origin consent and restores the
independent switch gate. Engine builds expose `endpoints.requestSwitch` with `accountId`, `endpointId`,
`expectedRevision` and `operationKey` (see `protocol/switch.schema.json`). It returns a durable running
receipt without waiting for user-paused roots. Poll `operations.get`; success requires all frozen roots
to reconcile at the new epoch. The Go `Remote.RequestSwitch`/`WaitOperation` methods separate acceptance
from completion. Closing a wait does not cancel the operation. Full sync and automatic switching stay
capability-disabled pending complete recovery acceptance.
After interrupted-switch startup blocking, original root leases are reacquired before an internal
replacement operation freshly verifies the committed origin at a new epoch. The old receipt remains
failed; no recorded HTTP write is replayed. Previously failed hard verification is not auto-retried.
Endpoint removal also refuses a committed binding or pending switch target; deleting its origin
permission cannot bypass an existing runtime/recovery gate.

Endpoint registration uses durable staged records in Control schema 4. `endpoints.stage` performs
an unauthenticated HTTPS status probe; its immutable staging ID binds the displayed URL for ten
minutes. `endpoints.approveOrigin` durably records explicit credential permission before starting
read-only UID/DAV-root verification; `endpoints.verify` retries an already approved endpoint.
All mapped roots, including paused folders, must match. Registration never changes the active
endpoint. Restart invalidates unfinished operations and staged/verified results; approved origins
remain approved but require fresh verification. `endpoints.remove` refuses active probes and the
canonical endpoint. These operations do not enable automatic switching.
Failed or expired unapproved staging records are discarded when staging again; approved origins are
never discarded this way. Reusing the old operation key still returns its original outcome.

Engine-linked builds implement `folders.inspect`, `folders.add`, `folders.pause`, `folders.resume`,
`folders.syncNow` and `folders.remove`. Inspection accepts an existing local directory or, since 1.9,
one that does not exist yet under an existing parent; the returned five-minute token binds both root
identities and is rechecked when adding. Removing a mapping preserves files and its journal; a running
mapping must first quiesce. Since 1.9 account removal removes the account's mappings itself.
Foundation-only builds reject these engine operations.

`decisions.list` and snapshots expose `bulkDeletions` with `localDeletes`, `remoteDeletes`, `digest`
and `expires` (and `groups` since 1.9). `decisions.resolve` takes `decisionId`, `proposalDigest` and an
`action` of `approveDeletion` or `later`; approval consumes the pending
decision and authorizes one matching rediscovery within ten minutes of proposal creation. Restart
invalidates old proposals. Approval is not a promise that propagation succeeded: observe folder state
and any replacement decision. The current UI still needs controls for this decision kind.

The first connected implementation delivers the daemon lifecycle, durable global pause,
snapshot subscription, operation deduplication and the Go client adapter. The endpoint-switch
coordinator checks trust, identity, epoch and quiescence as a separate control-thread component;
the engine adapter must supply the actual quiescence proof before it can activate an endpoint.
Login now uses an in-memory Login Flow v2 session and Windows Credential Manager; see
[accounts](accounts.md). `accounts.login.status` polls the session ID independently of its start
operation. Full sync and automatic switching remain capability-disabled until
their complete paths are implemented. Mock remains an explicit development mode.

The Windows daemon owns a profile-hashed Control DB under the current user's application-data
directory. Its SQLite connection stays on the Qt thread; settings, operation receipts and outbox
rows commit together with full synchronous durability. Terminal receipts are currently retained
indefinitely. `events.resume` requests a fresh bootstrap. Shutdown receipts never restart a shutdown
after a new daemon boot. The standalone switch guard is not yet connected to libsync.

Overlapped Win32 pipes reject remote clients, restrict access to the logon SID, omit client
instance-creation rights and reserve the first instance. Both peers check user and session identity;
the Go client additionally checks the server executable path. One UI connection is served at a time;
the sole pipe instance remains reserved across reconnects, with bounded output queues and stalled-I/O
deadlines. The frontend starts its sibling
`nextcloud-mygo-syncd.exe` once when absent. `NEXTCLOUD_MYGO_MOCK=1` explicitly selects demo mode.

## Verification

The endpoint-selection policy is a deterministic control-thread component with caller-supplied
monotonic time. It filters trust/network/freshness before applying PLAN §8 weights, preserves independent
quarantine, tracks generation/revision-bound probe tickets and limits half-open probes. Its output is
only a proposal; it never publishes an active endpoint or overrides engine quiescence. Runtime network
notifications, probe scheduling and switch activation must be connected before automatic switching is enabled.

The connected functional unit passed shared framing fixtures, persistence/idempotency and switch
guards on native ARM64 and emulated x64. Real Go-to-C++ pipes passed on both architectures,
including restart persistence, reconnect and refusal of an unexpected server executable path.
The native frontend opened its paired daemon; its screenshot showed the unavailable-login message
and disabled Continue control. Windows cross-user/session and impersonation acceptance are separate
from these checks and remain outstanding. Full sync and engine-driven switching are not verified.
