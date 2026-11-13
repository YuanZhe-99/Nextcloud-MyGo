# Development

## Requirements

- The latest Go release, named in `go.mod` (an older Go downloads it automatically). MyGo is a Go
  tool dependency (`go tool mygo …`) at its latest tag.
- Windows: Visual Studio 2026 (or its Build Tools) with the C++ tools for ARM64 and x64 and the
  Windows SDK; CMake and Ninja. M0 chooses their versions, together with Qt and the other native
  dependencies, and `build-lock.json` records what each build used.

## Commands

Initialize the recorded upstream snapshot with `git submodule update --init third_party/nextcloud`.
Keep that checkout unmodified; build trees and dependency downloads belong in ignored `.dev/`
or `build/`. The current M0 status is in [backend-progress.md](backend-progress.md).

`tools/bootstrap-deps.ps1 -Architecture ARM64` (then `x64`) downloads Qt and builds the other native
dependencies at exactly what `build-lock.json` records into `.dev/deps`, and prints the path of
`.dev/deps/env-<arch>.json`: its `QtPrefix`, `QtHostPrefix` and `DependencyPrefix` are the arguments
below, and its `PkgConfig` goes into `PKG_CONFIG` for `tools/build-upstream.ps1`. CI runs the same
script ([ci](ci.md)). `tools/build-upstream.ps1 -BuildRoot`, `tools/build-syncd.ps1 -Build` and
`tools/test-all.ps1 -Build` put a build tree somewhere else, for example to build against a second
dependency prefix beside the usual trees.

```bash
gofmt -l cmd internal tools
go vet ./...
go test ./...
CGO_ENABLED=0 go test ./...
go tool mygo vet .
go tool mygo i18n check
go run ./tools/checkdocs           # doc/en-us and doc/zh-cn mirror each other
```

Build the connected daemon with `tools/build-syncd.ps1 -Architecture ARM64 -QtPrefix <target Qt>
-QtHostPrefix <host Qt>`; repeat with `-Architecture x64`. This builds the executable and runs
the shared framing, persistence and switch-guard CTest target. Put target Qt `bin` on PATH and
set `QT_PLUGIN_PATH` to its `plugins`; set `SYNCD_TEST_EXE` to the absolute built daemon path,
then run `go test ./internal/syncd -count=1 -v` for the real Go-to-C++ pipe contract. Tests use
a unique profile and remove it afterward. These commands neither install nor register the app.

Package a built SyncD with `tools/package.ps1 -Arch ARM64` (or `x64`); `go run ./tools/pkg check -arch
ARM64 <package dir>` re-runs the architecture check. `tools/package-smoke.ps1 -Setup <installer>`
really installs, reinstalls and uninstalls the package for the current user under a throwaway
profile and removes everything again; it refuses to run over an existing install. For an upgrade
between two versions, build a second package with `tools/package.ps1 -Arch ARM64 -TestVersion 0.1.1`
(its own SyncD tree; committed version files stay unchanged) and pass it as `-UpgradeTo`. See
[packaging](packaging.md).

M0's upstream-only build entry point is `tools/build-upstream.ps1`. From PowerShell, pass
`-Architecture ARM64` (or `x64`), `-Mode engine` (or `tests`), `-QtPrefix`, `-QtHostPrefix` and
`-DependencyPrefix`. Prefix arguments are absolute SDK/dependency directories; DependencyPrefix
may contain several directories. It discovers Visual Studio's CMake, checks the recorded clean
upstream commit, and builds into `build/<a|x><e|t>-<source fingerprint>` (architecture and mode
initials keep generated MSBuild paths below Windows limits).
Recorded patches are hash-checked and applied only to a generated source clone. Engine mode builds
`nextcloudcmd` without GUI or Shell integration; tests mode enables upstream GUI test dependencies
and runs CTest with `--no-tests=error`. It never installs or launches the official application.
The complete dependency combination is still being assembled; this is not a successful-build claim.

The real-server M0 fixture is `tools/test-upstream.ps1 -Executable <absolute CLI path>`.
Approved local test credentials may be loaded with `. ./.dev/nextcloud-test.local.ps1` in
PowerShell. This local-only file is covered by `/.dev/` in `.gitignore`; never stage it forcibly.
Set `NEXTCLOUD_TEST_URL`, `NEXTCLOUD_TEST_USER` and `NEXTCLOUD_TEST_PASSWORD` in your terminal and
put matching runtime DLL directories on PATH. It creates a unique remote collection and temporary
local profile, checks upload/download, modifications, conflict contents and stable journal location,
then cleans its resources. Credentials pass through the child environment, never arguments or files;
raw client output stays in memory and is never printed. The fixture observes upstream request
creation records, requires upload/download requests in the initial cycle as a positive control,
and requires discovery but no DAV content GET or write request in the unchanged cycle.
This check passed on native ARM64; it does not measure transport-level byte counts. On Windows,
`QT_FORCE_STDERR_LOGGING=1` sends the selected Qt category to the captured pipe rather than the
debugger. Remote cleanup failures
are explicit. No installer, cloud-root or Shell registration is invoked.

Use `tools/test-login.ps1 -Executable <absolute-daemon-path> -SyncFolders -Reauthenticate` for the
real paired-daemon account/switch/renewal fixture. It issues the original and replacement app passwords,
automates two explicit consent grants in memory, checks cancellation, mapped identities, independent
pauses, old-password revocation, resumed uploads and restart, then removes owned remote collections,
credentials and profiles. The one-time renewal browser handoff is consumed through an in-memory child
stdout pipe, never a file. This does not establish interactive browser, SSO/MFA or wrong-account acceptance.
With `-SyncFolders` it runs four lanes in parallel, each with its own sign-in (consent, persistence
across a restart, removal with confirmed revocation), profile and two owned folders: `switching`
(switches with a graceful restart and a kill, alternate uploads, published activity), `crash` (kills
during real unfinished transfers), `cross` (cancelled transfers recovered on the other origin) and
`renewal` (recursive watcher, independent pause, deletion review, fixed-policy reconciliation and, with
`-Reauthenticate`, the renewal). Every lane prints its `PASS` lines, a `STEP <ms> <name>` timeline and
its wall time. Profile names carry a random part: lanes start within one Windows clock tick, and two
lanes on one profile would share one daemon and remove each other's account. The shared server spends
over two seconds on every request that authenticates afresh, for the fixture and the daemon alike.
Positive DAV readback retries HTTP 423 only for GET, with a bounded budget; mutating requests are never
replayed by this fixture to hide a write outcome.

`tools/test-reauth-lifetime.ps1 -Executable <absolute-syncd_reauth_tests-path>` exercises cancel/new
session replacement during real nested DAV verification. It uses approved credentials only as borrowed
read-only test credentials, never grants or revokes an app password, and waits for the exact child.
Its parent reserves and validates a native-key cleanup manifest under one unique ignored profile;
cleanup also runs after a crashing child. The fixture reproduced access violation `0xC0000005` before
the local login-session lease fix. This is a lifecycle regression, not active-transfer crash acceptance.
The same parent wrapper accepts `-Scenario network` for the isolated real-network acceptance mode.
That child borrows credentials read-only, approves only the two supplied HTTPS origins, and uses a
process-local Qt proxy fault to refuse selected connections. It changes no system proxy, DNS, route,
firewall, registry or Explorer setting. It verifies automatic failure selection, all-bad waiting and
fixed-target refusal using actual HTTPS checks; it does not substitute an HTTP mock for DAV acceptance.
The paired-daemon fixture also creates 4 MiB random owned upload/download objects within the approved
quota. It kills only its own child after diagnostics observe an unfinished content reply and positive
content bytes, then checks recovered hashes and native journal-file identities after restart. A run
that never observes an active transfer fails rather than claiming crash acceptance from UI status.
All objects remain under the fixture's reserved collections and are removed by its existing cleanup.
The extension also observes positive-byte unfinished 4 MiB transfers, explicitly pauses/cancels them,
switches the whole paused account to the other approved origin and reconciles after resume. It checks
recovered hashes and native journal identities in both directions. This is cancellation plus manual
cross-origin recovery, not proof of automatic failure selection during a mapped active transfer.
The separate `-Scenario network-active` acceptance mode uses an owned local CONNECT relay to hold
a real transfer unfinished at a byte offset and then break it. TLS still terminates at the approved real
server, not at the relay. Normal daemon probes and policy must cancel, drain, fail over and reconcile
the mapped root using its original journal. Only one uniquely reserved remote collection is created;
the parent cleanup validates its manifest and removes it even after a child crash. It does not change
system networking or replay an unconfirmed user write. This mode is under implementation/acceptance.
Its lost-acknowledgement case holds only the large upstream-bound tunnel's encrypted response, proves
the PUT is visible through the other approved origin, and then drops the old connection before the
daemon receives success. Recovery must preserve uncertainty and rediscover; the fixture never tells
the daemon that the hidden PUT succeeded. This is separate from an early interrupted upload.
`-Scenario wrong-instance` is a separate read-only N02 fixture. It requires two genuinely isolated
Nextcloud instances with the same freshly created test UID/credential but different DAV namespace
identities, both exact HTTPS origins explicitly approved. It must fail namespace verification—not
merely authentication—persist quarantine, refuse activation and retain the canonical binding. Local
certificate trust requires explicit user permission. The wrong-instance test executable accepts only
the acknowledged public-root fingerprint and installs it in its own Qt process configuration, never
in the Windows trust store; normal chain and hostname checks remain enabled. TLS validation is never
disabled. Isolated-instance credentials and cleanup
manifests stay in ignored local configuration, never in committed fixtures.

`-Scenario network-chunked` cuts one chunk session inside its second chunk and requires it to resume
on the other origin without resending the first chunk. Upstream chunks only above 100 MiB, so it
uploads 112 MiB and needs an isolated server without a small quota, with the same fixture-local root
(`SYNCD_TEST_CA_CERT`, `SYNCD_TEST_CA_SHA1`). Qt never sends requests for `127.0.0.1` or `localhost`
through a proxy, which would bypass the fault relay: address a loopback server through two reserved
`.test` names present in its certificate and its `trusted_domains`, and set
`NEXTCLOUD_TEST_CLEANUP_URL` to the direct loopback origin for the cleanup step, which runs without
the relay. On failure the script prints the `PASS` lines already reached and one sanitized reason.
An isolated fixture server is recognised by `NEXTCLOUD_TEST_CLEANUP_URL` (the relay connects to that
host) and `SYNCD_TEST_CA_CERT` (the acknowledged root); with both set, every transfer scenario
below and `network-active` can run against it.

`-Scenario network-uncertain` loses the reply of writes the real server has committed, and needs
the same isolated server. The fixture sends the one request whose path it names through a relay
that delivers the request and never its reply, waits until the other origin shows the effect while
the daemon still waits, then breaks the primary. It covers a MOVE and a DELETE of existing entries
whose path another client creates again before recovery, and the final MOVE of a 112 MiB chunk
session. Recovery must leave the re-created object and the committed target untouched (same server
file ID and ETag) and send no chunk again; the fixture never tells the daemon what the server did.

`-Scenario switch-kill` ends its own child at a durable stage of a real switch and restarts it on
the same profile: `prepared` (intent stored), `quiescing` (an upload still running on the old
address), `verifying` (the target's credentialed check about to be sent), `committed` (binding
published, no request of the new epoch sent) and `reconciling` (part of a file already sent to the
new address). `-Stage` selects stages; each one uses a fresh profile. The restarted child gets no
hint beyond its own storage and must end on the previous binding for the first three stages and on
the committed one for the last two, with a fresh epoch, the same credential reference, folder
binding and native journal file, matching hashes, and no reconciled file transferred again. Its
content is 8 MiB, so it also runs against the shared test server.

`-Scenario multi-account` runs two accounts with two folders each on one daemon against the
disposable server (`PLAN.md` §2.1, R05). It signs in the fixture user again and requires
`ACCOUNT_EXISTS`, creates a second server user with a random password (recorded first in the
profile's `owned-user.json`, removed by the run and by the parent's `--cleanup`), adds both
accounts through the production login path, maps two folders each (one of them a local folder that
does not exist yet), moves content both ways in all four, switches the first account to the other
address while the second account's folders keep uploading (none of them may show the
`switchInProgress` blocker or a pause, and its binding and epoch must not change), gives the second user a 1 KB quota so its uploads are refused (`REMOTE_QUOTA_EXCEEDED` on the folder),
commits a fixed policy for that account while its quota is full, requires the first account to keep
syncing and the daemon to keep serving, and removes the second account while the first keeps syncing.
It needs a server where the fixture user may create users, so it runs only in the server tier.

`-Scenario switch-quota` reproduces the switch that vanished in B14 step 6 against the disposable
server. It creates an owned server user (recorded in `owned-user.json` like `multi-account`), signs
it in, approves the alternate address and maps one folder, holds a 64 MiB upload unfinished on the
primary with `NetworkTunnel::setStallAfter`, and asks for the alternate as "Use this one" does (a
fixed `endpoints.setPolicy`). While the upload is held the network must show the pending request
(`switching`, `requestedEndpointId`, `postponed`); once the 30-second drain window passes, the
request must end as a `switchFailed` activity with `QUIESCE_STALLED`, its addresses and name, the
binding and policy unchanged and the receipt failed. It then releases the upload, gives the user a
1 MB quota through the provisioning API so the next upload is `REMOTE_QUOTA_EXCEEDED`, asks again
and requires the switch to happen and be recorded, then removes the account and the user. Before the
fix the request disappeared without any record and the run stopped at its outcome step.

`-Scenario slow-server` makes the relay answer every client flight 1.5 s late
(`NetworkTunnel::setReplyDelay`; a health probe on a fresh connection then takes about 4.5 s) and
requires both addresses to stay healthy: a reachable server that is slow to answer is not
`NETWORK_UNAVAILABLE`. Before `metadataDeadline` it failed within 15 s.

`-Scenario sleep-resume` simulates a sleep; the user decided this machine is not put to sleep for
a test. Every tunnel runs in a child relay (`--sleep-relay=<pid>`, the same executable), because a
suspended process cannot drop its own connections. The case holds a 6 MiB upload on the primary
with the relay's stall hook, then the relay suspends the whole acceptance process (the daemon with
it) with `NtSuspendProcess` for 35 s, past the 15-second freshness and the 30-second failure,
success and drain windows. Meanwhile it resets every connection and refuses the primary from
then on, leaving only the alternate: the machine woke on another network. A control-thread timer
and an independent thread must both have stood still. After `NtResumeProcess` the daemon gets no
hint, not even a power notification. It must fail the dead connection on its own as a transport
failure (a reset while the body is still being written arrives as Qt's unknown network error),
keep every local file and propose no deletion while the primary is unreachable, never call the
folder up to date there, and go idle before the switch. The switch must be recorded with reason
`unreachable`. The upload must complete once on the alternate (one `uploaded` entry, at most one
body's worth of bytes for the interrupted attempt and one for each attempt on the alternate: the
completed one and any the server refused, such as the fixture's SQLite answering 503 under the
tier's load; no conflict copy) with matching hashes,
the original native journal file, folder rows and credential reference. The relay resumes its
parent on every path, and the wrapper's 120-second budget and Job Object end both otherwise.

`-Scenario file-safety` runs fault-free cases on the real server: renames and directory moves in
both directions with server file IDs kept, deletion review for one directory with twelve
descendants (direction and count), a stale approval after a later change, a leased local root and
a remote root answering 404. `-Scenario file-safety-bulk` runs, on a profile of its own, 100 single
files deleted on the server under a fifth of a 510-file baseline (the absolute threshold alone,
held as local deletions); it is a case of its own so that both stay within the 150 s budget.
`-Scenario network-large` cuts the chunk session of a file above
4 GiB after its 4 GiB boundary and requires it to resume on the other origin; it writes, sends and
verifies the content as a stream and needs an isolated server with about 9 GiB free.
`-Scenario held-revocation` needs an isolated server too: it issues one app password with the
fixture's own password, leaves it recorded as the held revocation of an interrupted login, restarts
Control and requires one DELETE after the 60-second floor, the server's confirmation, and the
password to be rejected afterwards. The fixture's own password is never revoked.

`-Scenario switching` runs the automatic-switching cases against the disposable server
(`backend/switching_acceptance.cpp`), each in its own process and profile; `-Case` selects them:
`auto-switch` (N08, N09, N11), `auto-fallback` (N14), `tls-trust` (N03, N04), `tls-auth` (N16,
N18), `tls-circuit` (N10, N15, N22), `redirect` (N05), `subpath` (N06), `revoke-switch` (N17),
`new-folder` (N19), `root-change` (N20) and `late-epoch` (N13). The cases that need a server answer
the real one does not give run a loopback TLS-terminating proxy inside the test process: it issues a
throwaway CA and certificates with `openssl` under the profile (expired, wrong host, unknown CA),
trusts only that CA and only in that process, forwards HTTP/1.1 to the real server over verified TLS,
and can strip an installation path, answer 401, 429 or a redirect, refuse connections or answer
late. The 120 s dwell is aged through the test's access to Control, because the switching
contracts prove it on a caller clock; the margin, stability, freshness and every trust check stay
the daemon's own. Each case takes 40–110 s on ARM64.

`-Scenario data-safety` runs the data-safety and recovery cases against the disposable server
(`backend/data_safety_acceptance.cpp`), each in its own process and profile; `-Case` selects them.
`interrupted-change` (S05) holds a local and a remote modification mid-transfer and then refuses both
addresses, and no transfer temporary file may reach the server; `watcher` (S09, S10) writes a change after discovery while an upload is held, and
overflows the watch buffer while the control thread is busy; `root-denied` (S13) denies listing of
the mapped root through its ACL and restores it; `maintenance` (S14) puts the instance into
maintenance mode with `occ`; `partial-listing` (S14) takes away a `files_external` storage mounted
inside the folder, so its listing answers 207 with one failing member; `names` (S17) creates a case
clash, names Windows cannot hold, NFD and non-BMP Unicode, paths above 300 characters and a locked
local file; `excluded` (S18, S21) adds a junction and an offline file to a new and to a synced
folder; `revocation` (R07, R22) signs in an owned user with an app password, revokes it on the
server, then scans the profile, the support bundle and the daemon's log. `journal` (S16),
`root-replaced` (S15) and `discovery-kill` (R02) end their first process (exit 75) and check the
restarted one on the same profile: a damaged journal header, a root replaced by an empty folder (and
a `DefineDosDevice` drive alias, removed again), a kill during remote discovery. `maintenance` and
`partial-listing` find the container through the `state.json` beside `SYNCD_TEST_CA_CERT`;
`maintenance` puts the whole instance into maintenance for a few seconds, so it must not share the
server with parallel lanes.

`tools/build-syncd.ps1` puts only Qt's `bin` on PATH for CTest. The consuming tests also need the
matching upstream build's `bin/Release` and the native dependency `bin` directories for the same
architecture; add them before calling it, and set `QT_PLUGIN_PATH` when running a built executable
directly. Never mix headers or DLLs from a different upstream build.

For the M0 discovery lifetime regression, run `./tools/test-discovery-lifetime.ps1 -Executable <build>/bin/Release/RemoteDiscoveryTest.exe` with the target Qt and dependency DLL directories on
PATH. It runs the upstream timeout/next-fixture pair in 20 bounded subprocesses and fails on any
nonzero exit or timeout. Diagnostic output remains under ignored `build/`; this is a mock-server
crash regression, not real-server acceptance. It reproduced the pre-patch-0005 crash; post-patch
native ARM64 execution passed 20/20 processes.

M0 CTest runs exclude `UtilityTest` because its startup test creates a real startup registration.
Report this exclusion explicitly; this configuration does not verify startup integration.
They also exclude `SyncCfApiTest`, which registers temporary sync roots and has one case that opens
Explorer windows. Run it with `tools/test-cfapi-upstream.ps1 -Build <upstream tests tree>` (target Qt
and dependency `bin` directories on PATH): it leaves that case out and fails if an `OC-TEST`
registration or one of upstream's shell-extension class keys is left behind.

`tools/test-ondemand.ps1 -Architecture ARM64|x64 [-Build <SyncD tree>]` is the real on-demand run
([on-demand](on-demand.md)). It starts a disposable server with `tools/test-server.ps1`, runs
`syncd_ondemand_acceptance` within 150 s (every step inside has its own deadline), removes the server,
and fails if a `Nextcloud-MyGo!` sync root registered during the run is still present. `-State` reuses
a server that is already running. The CTest target `SyncDOnDemandContracts` needs no server.

Reused build sources are checked in a temporary Git index: reverse the recorded patches in reverse
order, then compare the result to the recorded upstream commit. This handles overlapping patches
without modifying the source working tree or its real index.

## Test tiers

`tools/test-all.ps1 -Tier fast|server -Architecture ARM64|x64` is the one entry point. It runs
the built SyncD of `build/syncd-<arch>` with the Qt and upstream DLLs its CMake cache recorded,
prints one `PASS`/`FAIL` line per scenario with its duration and a `TOTAL` line, stops at the first
failure, and fails when the total exceeds the tier's budget. A failed scenario is never rerun, on
the same or on the other architecture.

| Tier | Budget | ARM64 (native) | x64 (emulated on ARM64 hosts) |
|---|---|---|---|
| `fast` | 2 min, no server | `gofmt`, `go vet`, `go test ./...` with the real Go↔C++ pipe tests, all SyncD CTest targets | `go test ./...` built for amd64, all SyncD CTest targets |
| `server` | 10 min including the container | the release-critical real-server scenarios below in six parallel lanes started 3 s apart, then `maintenance` alone | one smoke scenario (`switch-kill` at `committed`) |

The server tier starts its own disposable Nextcloud with `tools/test-server.ps1 -Start` (`wslc` by
default, `-Runtime docker` where Docker runs Linux containers; GitHub's Windows runners cannot, so
[CI](ci.md) runs the fast tier only) and removes it with `-Stop` afterwards: one labelled
container with an anonymous volume on a loopback port, the image only if this run pulled it, and a
fixture-local root and admin password kept in ignored `build/test-server-<id>/` until then. The
root is trusted only inside each acceptance process (`SYNCD_TEST_CA_*`); no trust store, hosts
file or proxy setting changes, and TLS checks stay on. Clients address the server as
`primary.syncd.test` and `alternate.syncd.test` through the fixture relay, the cleanup step and the
`lifetime` case use the direct loopback origin. The tier needs no private server.
`-ServerVersion` picks the server release (default `35.0.2`, the latest); run it with the newest
release of the previous supported major too (`-ServerVersion 34.0.5` on 2026-11-13: passed on ARM64
in 498.2 s). On 2026-11-13 the default ARM64 tier passed three times in a row in 483.6, 502.2 and
539.0 s (container start 66.1–105.9 s, `maintenance` alone 67.5–68.1 s), `-Extended` in 566.3 s
plus 115.2 s for its three lanes, and the emulated x64 smoke in 156.6 s.

Its cases each run in a process and profile of their own, so every case starts on healthy
endpoints instead of waiting out the circuit history of the one before it: `network-active` runs
`upload`, `download` and `unacknowledged-upload` as separate cases, `network-uncertain` runs `move`,
`delete` and `final-move` (`-Case` selects them in `tools/test-reauth-lifetime.ps1`), and
`switch-kill` runs each `-Stage`, and `switching` and `data-safety` run each `-Case`. Independent
cases share the container and run in parallel lanes. The default tier runs every scenario except
eight switching and data-safety cases: `tls-circuit`, `subpath`, `new-folder`, `root-change`,
`names`, `watcher`, `root-denied` and `excluded` run only with `-Extended`, in three more lanes
outside the budget. `maintenance` puts the whole instance into maintenance mode, so it runs last and
alone. Lanes start 3 s apart: started together, their first unauthenticated probes outran the 4 s
public-probe deadline on a fresh server. A lane restores its relayed address after `lifetime`, which
addresses the server directly; a case that followed it once ran without its relay. More than six
lanes slowed this host enough for timing-bound cases to miss their windows (an overflow audit
within 30 s, a lost final MOVE's failover). The disposable server has brute-force protection off
(`auth.bruteforce.protection.enabled=false`, checked: failed sign-ins leave no attempts and no
delay), so the failed sign-ins of the revocation cases never slow another lane's sign-in.
The relay is no longer throttled: a transfer is held unfinished at a byte offset
(`NetworkTunnel::setStallAfter`, primary or alternate relays only, requests below 1 MiB unaffected)
and broken by the fault the case names. A faulted origin resets the relay's established tunnels and
answers new ones as a proxy reports a destination that refuses connections (503); before the soak,
it closed a new tunnel right after its CONNECT, and Qt then left a request with a body (upstream's
PROPFIND) and every later request of that manager waiting for the transfer timeout, which no real
refusal does ([upstream](upstream.md)). Most of a case's remaining time is the daemon's own
failover, circuit and reconciliation timing, which the tests do not shorten.

Every step has its own deadline and fails with its name: C++ waits default to 60 s and print a
`STEP <ms> <name>` line that the wrapper reports when a child exceeds its 150 s budget (240 s for
`network`, which waits out two circuit-open windows); `SYNCD_TEST_STEPS=1` prints all step times.
The Go probe gives each wait 90 s (a sign-in 150 s), each command retry 30 s and each lane's phase
5 min, names the step (a command by its source position) and the daemon's sanitized state on
failure, and a watchdog prints its goroutine positions and exits shortly after the phase budget even
if a step ignores its context. A call made without a ready connection fails as "not connected", so a
daemon that is gone is never mistaken for a busy one. All wrappers relaunch themselves in one child
`pwsh` that owns a kill-on-close Job Object (`tools/test-job.ps1`), and the probe puts itself in
one too, so no daemon, probe or test executable outlives a failed, killed or interrupted run.
Cleanup is direct: the probe's `abort` removes the owned account through the daemon (waiting out
at most its 60 s crash backoff), then the owned collections and roots; it never replays the flow.
Its client lives for the whole abort; it once lived only for the 75 s connection wait and closed its
own pipe in the middle of a slow account removal.
When the daemon refuses to remove the account, the abort keeps the profile, roots and state file
and says so, because only an existing root lets an interrupted switch finish.

Outside the budget, by explicit switch: `-Extended` adds the eight cases named above; `-Large` adds `network-large` (a chunk session above 4 GiB
cut after its 4 GiB boundary, about 30 min, about 9 GiB free on the server), and `-SharedServer`
adds the paired-daemon fixture `tools/test-login.ps1` against the approved shared server in
`NEXTCLOUD_TEST_*`: the paired daemon trusts only a publicly trusted chain, so it cannot use the
disposable server. `wrong-instance` needs a second instance and is run on its own.

`tools/test-soak.ps1 -Minutes 60` is the accelerated soak (acceptance R21, `PLAN.md` §23.3), opt-in
and outside the tiers and [CI](ci.md); `-State` reuses a running disposable server. One daemon
(`syncd_reauth_tests --soak=first|second`, `backend/soak_acceptance.cpp`) signs in the fixture user
and an owned second user, approves both addresses for each (the first account's alternate is a
home address, so preference switches happen as well as failovers), maps two folders each and repeats
a cycle: in every folder one or two local adds, a local modify, rename and delete, a remote add,
modify, move and delete on distinct names; every fourth cycle a 24–32 MiB file (a chunked upload)
or its deletion; one network event, rotating through a 20 s outage of either address, a 12 s loss
of both with network-change notifications, three 2 s flaps of one address, or the cut of the
address a large upload is running on once 4 MiB of it are sent. Each cycle must converge: both
sides hold exactly the expected names and SHA-256 hashes, then, with polls paused, every folder is
idle and still matches (polled 24 times as often, some folder is nearly always in a cycle); a
deletion proposal or review state fails the run. A local rename the daemon undoes is counted
(`renamesReverted`, a failed check) and followed: upstream renames the file back when its MOVE
fails, a transport failure included ([upstream](upstream.md#nextcloud-desktop)). The daemon's timers run 24 times faster through the
test's access to Control: pending probes, circuit cooldowns, the switch cooldown and the 120 s
dwell are pulled forward, and each folder's poll and the daily audit are queued as their own timers
would; stability, freshness and debounce windows, server throttles and every trust check stay the
daemon's own. Each half renews one account's credential through `accounts.reauthenticate` with the
user's own password. After a fifth of the run the daemon shuts down and a new process opens the
same profile, which must keep every journal, folder identity and credential reference; the slope
window then holds only the second process, past its warm-up. Meanwhile
`tools/test-soak` connects the interface's pipe client (`syncd.ConnectRemote`) and disconnects
again every few seconds, staying 30 s every fourth time. The wrapper samples the daemon every 30 s
(private bytes, working set, handles, threads, TCP connections, CPU time, I/O, the Control database
and its WAL, the journals and the log) into `build/soak/samples.csv`; the TCP count includes the
in-process relay's own sockets. It fits slopes over the last 40 minutes against
the thresholds at the top of the script, and writes `summary.json` and `summary.txt`. The relay
counts the connections a fault cut after they were established and the bytes they carried
(`NetworkTunnel::totals`), the bound for what a fault may make the daemon send again.
## Running the interface

```bash
go build -o build/app/nextcloud-mygo.exe ./cmd/nextcloud-mygo # put paired SyncD beside it
NEXTCLOUD_MYGO_MOCK=1 go run ./cmd/nextcloud-mygo            # sample data
NEXTCLOUD_MYGO_MOCK=1 NEXTCLOUD_MYGO_FRESH=1 go run ./cmd/nextcloud-mygo # demo setup
NEXTCLOUD_MYGO_PROFILE=dev go run ./cmd/nextcloud-mygo       # a separate instance
SCREENSHOT_DIR=/tmp/shots go test ./internal/gui             # every page as PNG
go tool mygo generate                                        # after editing locales/*.json
```

Normal startup connects to its sibling `nextcloud-mygo-syncd.exe`. Sign-in remains disabled until
the daemon advertises its implemented capability. Explicit Mock mode changes no server files
and labels its sample data.

## Testing principles

Tests protect product contracts and error-prone logic, preferably table-driven; every fixed bug
gets a regression test, and each test names the invariant or acceptance case it covers.
Scheduling, scoring, backoff and switching run on a controllable clock. Tests that need a real
Nextcloud server run only when their environment variables are set. An HTTP mock never replaces a
real server for DAV, locking or chunked uploads in acceptance.
