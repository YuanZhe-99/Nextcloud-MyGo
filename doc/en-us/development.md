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
profile and removes everything again; it refuses to run over an existing install. See
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

`-Scenario slow-server` makes the relay answer every client flight 1.5 s late
(`NetworkTunnel::setReplyDelay`; a health probe on a fresh connection then takes about 4.5 s) and
requires both addresses to stay healthy: a reachable server that is slow to answer is not
`NETWORK_UNAVAILABLE`. Before `metadataDeadline` it failed within 15 s.

`-Scenario file-safety` runs fault-free cases on the real server: renames and directory moves in
both directions with server file IDs kept, deletion review for one directory with twelve
descendants (direction and count), a stale approval after a later change, a leased local root and
a remote root answering 404. `-Scenario network-large` cuts the chunk session of a file above
4 GiB after its 4 GiB boundary and requires it to resume on the other origin; it writes, sends and
verifies the content as a stream and needs an isolated server with about 9 GiB free.
`-Scenario held-revocation` needs an isolated server too: it issues one app password with the
fixture's own password, leaves it recorded as the held revocation of an interrupted login, restarts
Control and requires one DELETE after the 60-second floor, the server's confirmation, and the
password to be rejected afterwards. The fixture's own password is never revoked.

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
| `server` | 10 min including the container | every real-server scenario below, in six parallel lanes | one smoke scenario (`switch-kill` at `committed`) |

The server tier starts its own disposable Nextcloud with `tools/test-server.ps1 -Start` (`wslc` by
default, `-Runtime docker` for hosted runners) and removes it with `-Stop` afterwards: one labelled
container with an anonymous volume on a loopback port, the image only if this run pulled it, and a
fixture-local root and admin password kept in ignored `build/test-server-<id>/` until then. The
root is trusted only inside each acceptance process (`SYNCD_TEST_CA_*`); no trust store, hosts
file or proxy setting changes, and TLS checks stay on. Clients address the server as
`primary.syncd.test` and `alternate.syncd.test` through the fixture relay, the cleanup step and the
`lifetime` case use the direct loopback origin. The tier needs no private server.
`-ServerVersion` picks the server release (default `35.0.2`, the latest); run it with the newest
release of the previous supported major too (`-ServerVersion 34.0.5` on 2026-11-12: passed on ARM64
in 431.4 s).

Its cases each run in a process and profile of their own, so every case starts on healthy
endpoints instead of waiting out the circuit history of the one before it: `network-active` runs
`upload`, `download` and `unacknowledged-upload` as separate cases, `network-uncertain` runs `move`,
`delete` and `final-move` (`-Case` selects them in `tools/test-reauth-lifetime.ps1`), and
`switch-kill` runs each `-Stage`. Independent cases share the container and run in parallel lanes.
The relay is no longer throttled: a transfer is held unfinished at a byte offset
(`NetworkTunnel::setStallAfter`, primary or alternate relays only, requests below 1 MiB unaffected)
and broken by the fault the case names. Most of a case's remaining time is the daemon's own
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

Outside the budget, by explicit switch: `-Large` adds `network-large` (a chunk session above 4 GiB
cut after its 4 GiB boundary, about 30 min, about 9 GiB free on the server), and `-SharedServer`
adds the paired-daemon fixture `tools/test-login.ps1` against the approved shared server in
`NEXTCLOUD_TEST_*`: the paired daemon trusts only a publicly trusted chain, so it cannot use the
disposable server. `wrong-instance` needs a second instance and is run on its own.
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
