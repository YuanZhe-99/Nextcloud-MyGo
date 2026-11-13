# Platform notes

| Platform | Status |
|---|---|
| Windows 11 ARM64 | Connected SyncD core and real Go/C++ pipe passed; the installed package passed most of the joint release acceptance (B14, below). Upstream CLI basic real-server sync passed (below). |
| Windows 11 x64 | Engine cross-built on ARM64; PE checks and basic sync under x64 emulation passed. No native x64 runtime acceptance. |
| Linux, macOS | Not supported. |

## Windows

Backend completion for 0.1.0 on the same ARM64 host (2026-11-12), protocol 1.9 and Control schema
11, SyncD rebuilt against `build/at-D0C17DFD07B7` (`build/syncd-ARM64`) and `build/xt-5F1D91E19629`
(`build/syncd-x64`, run under emulation), disposable Nextcloud 35.0.2 containers (`wslc`):

| Run | Native ARM64 | Emulated x64 |
|---|---|---|
| `tools/test-all.ps1 -Tier fast` | passed, 35.0 s: format and vet 2.7 s, Go tests with real pipes 19.4 s, 22 CTest targets 12.8 s | passed, 42.0 s: Go tests 21.7 s, 22 CTest targets 20.2 s (a first attempt failed `internal/syncd` once, reason not captured; three repeats of the x64 Go suite then passed) |
| `tools/test-all.ps1 -Tier server` | passed, 459.5 s: server start 62.0 s; active upload 108.4 s, download 65.8 s, lost PUT reply 104.3 s; lost MOVE 115.7 s, DELETE 68.1 s, final MOVE 119.1 s; kills at prepared 133.6 s, quiescing 135.0 s, verifying 125.9 s, committed 137.6 s, reconciling 137.8 s; file safety 64.4 s; chunk resume 147.8 s; held revocation 62.5 s; network 135.8 s; lifetime 4.7 s; **two accounts × two folders 64.1 s**; removal 1.9 s | passed, 162.5 s: server start 60.7 s, `switch-kill` at `committed` 100.0 s, removal 1.8 s |

The two-account case is the `multi-account` scenario ([development](development.md)). Once in six
parallel CTest runs `SyncDDurableConstraintContracts` failed and then passed alone 3/3 and in three
more parallel runs; its cause is not established. After the final rebuild the x64 fast tier
failed once in `SyncDNetworkContracts`: it looked up the network monitor's message-only window by
class and found the one of the fixture daemon running at the same time; the test now takes its own
process's window, and the tiers then passed again (ARM64 34.8 s, x64 33.7 s).

The paired-daemon fixture `tools/test-login.ps1 -SyncFolders -Reauthenticate` against the shared
server passed on native ARM64 in 561.7 s with every phase (policy, switching with restart and kill,
watcher, two folders with deletion review, active-transfer kills and cancellation, renewal with
cancellation, old-password revocation and restart, login cleanup). Seven earlier runs that day
failed: the first at HTTP 507 (the test account's quota was full of leftover collections),
two at the restart after the renewal (the client lost the daemon), two after the renewal with a
folder still syncing and the credential-reload switch open while the client's calls came back
busy, and two at the renewal's own 150 s wait, which was then raised to the step budget plus three minutes on the grounds that the shared server takes
2.5–3 s per authenticated request and holds the old password's revocation for at least 60 s.

Later that day the fixture was split into four parallel lanes (`switching`, `crash`, `cross`,
`renewal`, started 10 s apart) and passed against the shared server three times in a row on native
ARM64 (253.3 s, 259.6 s, 279.9 s), with no daemon, profile, local root, remote collection or issued
app password left behind. One emulated x64 run failed (262.9 s): the renewal lane's fixed-policy
reconciliation ended `NETWORK_UNAVAILABLE`, and its abort lost the pipe to the live daemon during
account removal; a second abort removed the account with confirmed revocation, and why that
connection dropped is not established. Earlier failures that day are explained: two
"every call busy" runs had a daemon that had stopped itself (exit 0 after a storage failure while the
quota was full) and a Go client that reported its own missing connection as busy, now
`ErrDisconnected` (`TestDisconnectedIsNotBusy`); the first parallel attempt put all lanes on one
profile because their clock-only profile names were equal within one Windows clock tick. The
account's 100 MiB quota holds 62.6 MiB of the server's default files, so about 37 MiB remain; one
run's payloads peak at 16 MiB. Every request that authenticates afresh costs the server about 2.3 s,
and more than about five at once exceed the daemon's 4-second metadata deadline, which a lane then
sees as a 60 s `NETWORK_UNAVAILABLE` stall; a killed upload's recovery took 25–67 s.

After the quota, metadata-deadline and abort fixes ([backend progress](backend-progress.md)), same
host and builds: fast tier passed on ARM64 (37.5 s) and emulated x64 (36.5 s); the server tier
passed on ARM64 in 477.9 s (server start 82.0 s, the new `slow-server` case 28.9 s, two accounts
with the quota phase 79.0 s, every other case within 150 s) and the x64 smoke in 199.2 s
(`switch-kill` at `committed` 120.4 s). The shared-server fixture passed once on emulated x64 in
244.9 s (lanes 211.6–241.3 s), with no daemon, profile, root or container of its own left behind.
Before the fixes the quota phase left a fixed policy running for 180 s and `slow-server` failed
within 15 s. The daemon that stopped itself on the shared server was not reproduced.


On-demand files on the same ARM64 host (2026-11-12), Desktop v35.0.3 with patches 0001–0017 in
`build/at-D0C17DFD07B7` (native ARM64), SyncD in `build/syncd-cfapi-ARM64`, a disposable Nextcloud
35.0.2 container (`wslc`); x64 with the recorded patch in `build/xt-5F1D91E19629`, run under emulation
on the same host ([on-demand](on-demand.md)):

| Run | Result |
|---|---|
| Upstream suite, `SyncCfApiTest` excluded | 95/96 in 518.0 s; `CfApiShellExtensionsIPCTest` failed while a `SyncCfApiTest` run overlapped it, then passed 3/3 alone through CTest. With the plugin built it registers a temporary `Nextcloud!` sync root of its own, removed afterwards |
| Upstream suite on emulated x64, `SyncCfApiTest` excluded | 96/96 in 594.6 s (`build/xt-5F1D91E19629`, the recorded patch) |
| `tools/test-cfapi-upstream.ps1` | 50 passed, 2 skipped, 0 failed: 97.1 s on ARM64, 102.9 s on emulated x64 (39 of 40 cases; the Explorer-opening case left out), including patch 0017's `testSyncRootAccountIdWithoutShellExtension` and `testRefusedRequestsFail`; no `OC-TEST` registration or shell-extension class left |
| SyncD CTest | 22/22 in 33.3 s on ARM64 (`SyncDOnDemandContracts` 12.6 s); 22/22 in 46.8 s on emulated x64 (`build/syncd-cfapi-x64` against `build/xt-5F1D91E19629`, 14.1 s) |
| Real Go↔C++ pipe (`go test ./internal/syncd`, `SYNCD_TEST_EXE`) | passed with protocol 1.8, no test skipped: 13.9 s on ARM64, 18.0 s on emulated x64 (`GOARCH=amd64`) |
| `tools/test-ondemand.ps1` | final runs, each starting and removing its own server: ARM64 65.8 s (fixture 30.7 s), emulated x64 59.2 s (fixture 32.3 s); every step listed in [on-demand](on-demand.md); the kill came at a partial hydration of a 64 MiB file and the retry after restart matched its hash; cleanup removed the remote collection, vault entry, local folder and registration, and no sync root was left. Earlier, 13 ARM64 runs passed and one failed at the switch step: a poll cycle running when the step began closed the root before the application's request arrived, a fixture race that the step now avoids by waiting for an idle runtime |
| `tools/package.ps1 -Arch ARM64` | the plugin is staged by import closure; 29 PE files native ARM64, every import resolved |

`testRefusedRequestsFail` first failed because `cmd /c type` exits 0 when a read fails; the patch
reads with `copy`. That test-only fix was verified by rebuilding `SyncCfApiTest` in this tree after
applying the same change to its source, so this tree's fingerprint names the patch before the fix;
the library code is identical. The x64 build uses the recorded patch. Under x64 emulation the engine
reported `NETWORK_UNAVAILABLE` for `https://localhost:<port>` while the container listened on IPv4
loopback only; the fixture addresses `127.0.0.1`, which the fixture certificate names.

Test redesign on the same ARM64 host (2026-11-12), with the SyncD binaries of `build/syncd-ARM64` and
`build/syncd-x64` built from this tree. Cause of the paired-daemon fixture failure below: after any
unclean exit the daemon stayed in crash backoff for good, because `main.cpp` passed its admission
clock's fixed start time (`msecsSinceReference()`) instead of the current time; the fixture's own
kills always caused one, so the probe waited for a connection that never came. Fixed with the
current time; `TestRealPipeCrashBackoffActivates` failed against the old binary and passes on both
architectures. Runs of `tools/test-all.ps1`, each with a disposable Nextcloud 35.0.2 container
(`wslc`, pulled and removed by the run):

| Tier | Native ARM64 | Emulated x64 |
|---|---|---|
| `fast` | passed, 39.9 s: format and vet 17.0 s, Go tests 15.9 s, 21 CTest targets 6.9 s | passed, 41.0 s: Go tests 30.8 s, 21 CTest targets 10.2 s |
| `server` | passed, 471.0 s, six lanes: server start 61.3 s; active upload 113.2 s, download 69.0 s, lost PUT reply 104.3 s; lost MOVE 66.3 s, DELETE 57.7 s, final MOVE 123.5 s; kills at prepared 140.0 s, quiescing 137.7 s, verifying 128.5 s, committed 142.1 s, reconciling 136.3 s; file safety 69.2 s; chunk resume 148.3 s; held revocation 62.7 s; network 124.1 s; lifetime 5.7 s; removal 2.5 s | passed, 206.2 s: server start 76.7 s, `switch-kill` at `committed` 127.3 s, removal 2.1 s |

Each kill stage is two processes (kill, restart) and stays a little above two minutes; the rest of
each case's time is the daemon's own failover detection, circuit-open and standby probe windows.
Not run: `network-large` (opt-in), `wrong-instance` (second instance), `-Runtime docker`.
Reliability package on the same Windows ARM64 host (2026-11-11), server 35.0.2 throughout. The
sixteen recorded patches were built from a fresh clone (`build/src-809fadb79180-A58E1844371E`) and
the upstream suite run serially: **96/96 on native ARM64 (427.45 s, `build/at-88D6D388A83D`) and on
emulated x64 (496.83 s, `build/xt-88D6D388A83D`)**, `UtilityTest` excluded because it registers
startup integration; `ChunkingNgTest` with patch 0016 passed in both. PE inspection matched 103
EXE/DLL build outputs per architecture (AA64 / 8664). SyncD built against those trees passed
**21/21 CTest targets on ARM64 (20.55 s) and emulated x64 (32.28 s)**.

Real fixtures (`tools/test-reauth-lifetime.ps1`, SyncD built against the fresh trees). The
isolated server was an owned Nextcloud 35.0.2 container on a loopback port, reached through two
reserved `.test` names, with a fixture-local root trusted only inside the acceptance process; no
Windows trust store, hosts file or proxy setting changed. The shared server is the approved test
instance with its two addresses.

| Scenario | Server | Native ARM64 | Emulated x64 |
|---|---|---|---|
| `network-uncertain`: lost reply of a committed MOVE, DELETE and chunk-session final MOVE | isolated | passed, 12 min 31 s | passed, 10 min 27 s |
| `switch-kill`: kill at `prepared`, `quiescing`, `verifying`, `committed`, `reconciling` and restart | isolated | passed, see below | passed, 11 min 0 s |
| `switch-kill`, same five stages | shared | passed, 15 min 59 s | passed, see below |
| `file-safety`: S04, S11, S12, S13, S14 (404), S20 | isolated | passed, 1 min 6 s | passed, 59 s |
| `file-safety` | shared | passed, 3 min 29 s | passed, 3 min 24 s |
| `held-revocation`: restart sends the held DELETE once | isolated | passed, 1 min 2 s | passed, 1 min 2 s |
| `network-chunked` (regression of the earlier case) | isolated | passed, 2 min 55 s | passed, 2 min 58 s |
| `network-active` (regression of the earlier cases) | shared | passed, 13 min 38 s | passed, 12 min 47 s |
| `network`, `network-lifetime`, `lifetime` (regressions) | shared | passed, 2 min 51 s, 32 s, 11 s | passed, 2 min 55 s, 36 s, 13 s |
| `network-large`: 4,296 MiB chunk session cut after 4 GiB | isolated | passed, 28 min 35 s | not run |

On ARM64 the first `switch-kill` run against the fresh tree passed `prepared` and `quiescing` and
then stopped in the fixture's setup for `verifying` (exit 1, before any switch was requested) while
the x64 upstream build was loading the machine; its reason was not captured, and the script now
echoes setup reasons too. The three remaining stages passed when run again on the rebuilt binary
(6 min 47 s). On the shared server the x64 run passed four stages and then failed in the
fixture's own teardown after every `reconciling` check had passed: it asked to remove the mapping
while the next poll cycle ran and was refused. The fixture no longer removes the mapping there, and
`reconciling` passed again on both architectures (x64 2 min 46 s, ARM64 4 min 1 s). Before the fresh trees existed, the same scenarios had already passed once on both
architectures against SyncD built on the fifteen-patch trees with patch 0016 applied
incrementally (ARM64: all five stages, 11 min 38 s in two runs; lost replies 11 min 55 s; x64:
11 min 38 s and 9 min 42 s). `network-large` ran only in that earlier ARM64 build; its fixture
code has not changed since. Each lost-reply case checks that the daemon was still waiting when the
other origin showed the committed effect, and that the committed entry's server file ID and ETag
are unchanged after recovery. Each kill stage checks the restarted process against a record written
before the kill: account, credential reference, folder rows, native journal file identity, content
hashes, a fresh epoch, and transfer counters that allow only the interrupted file to travel again.
The real Go↔C++ pipe tests passed (ARM64 9.390 s, emulated x64 9.958 s). The paired-daemon fixture `tools/test-login.ps1 -SyncFolders -Reauthenticate` did **not** pass on these binaries: on ARM64 its `finish` phase ran into its 15-minute deadline about two minutes after a daemon start, its `abort` phase ran into the same deadline, and the daemon it had started outlived its dead parent; the x64 run showed the same pattern. The fixture output was not captured, so the cause is not established: it is either a regression of this package's login and Control changes or the fixture against the changed Go client. Until it is found, the real daemon's login, two-folder sync and renewal are not re-verified for this tree, and that fixture's app passwords and collections on the shared server may need manual cleanup. The Go suite, the `CGO_ENABLED=0` suite, `go vet`, MyGo vet, i18n (401 messages) and the
docs mirror check passed. Not run: a real 429, a download above 4 GiB, `wrong-instance` (it needs a
second instance; its code path changed only in how the scenario is named), native x64 hardware and
clang-tidy. The owned container, volume, image, bootstrap credentials and TLS files were removed
afterwards; the pre-existing container was not touched.

Protocol 1.7 final startup/recovery integration remains in progress. The independent S19
root-ownership regression failed before remediation
(`cross-process descendant root admitted`). Kernel-handle hierarchical admission then passed three
repetitions / 144 races on native ARM64 (18.27 s) and emulated x64 (26.27 s), including sibling
concurrency, process termination and malformed-object refusal. Actual non-NTFS refusal remains
unverified because the available FAT32 fixture could not be created; separate-session, SUBST and
crash-during-publication checks also remain unrun. Eight standalone startup groups passed on ARM64
(1.69 s) and emulated x64 (2.32 s), covering crash gating, explicit stop intent and validated
WAL-consistent migration backups; these are not yet connected startup acceptance. Six isolated
engine-linked folder-error groups passed per architecture; credential/journal error injection and
post-inspection add-path cases remain unverified.

The shared protocol-1.7 consuming suites subsequently passed 19/19 on native ARM64 (12.59 s) and
emulated x64 (18.36 s), including startup helpers, deferred availability, authenticated startup pipes,
diagnostics ownership and folder-error projection. The actual service-entrypoint Go↔C++ pipe tests
passed in 6.766 / 6.815 s. N02 then passed against two real isolated Nextcloud 35.0.2 instances on
both architectures: identical fresh authenticated UID/credentials, distinct DAV namespace, durable
quarantine, refused activation and unchanged canonical binding. The user explicitly acknowledged
both loopback origins and a fixture-local CA; normal certificate-chain/hostname checks stayed enabled,
with no Windows root-store installation and no existing server password reused. Parent cleanup
reported both instances/data/secrets removed. Independent runtime lookups confirmed the exact owned
containers and volumes absent, no owned network bindings, no Windows listeners/portproxy/static NAT
entries for the reserved ports, only sanitized manifests retained, and no fixture root in either
Windows root store. The original container stayed running. Direct HNS-internal inspection required
elevation and was not run; no exhaustive HNS/storage-forensic claim is made.
Active transfer still requires final real acceptance.

The two-corrupt-storage-boot regression reproduced the one-receipt restriction on both architectures.
After bounded 32-receipt storage, isolated connected/pipe suites passed 2/2 on native ARM64 (2.08 s)
and emulated x64 (2.54 s). The actual Go↔service-entrypoint regression had independently reproduced
the old binary's second-stop refusal (`QUIESCE_STALLED`, 2.494 s). After the post-handoff rebuild it
passes, together with the existing pipe test: **5.698 s native ARM64, 5.865 s emulated x64**. The same
rebuild passed **21/21 CTest targets on ARM64 (18.17 s) and emulated x64 (25.25 s)**, including the
schema-8 durable constraints and metadata boundary. Build-output PE inspection checked 23 binaries
per architecture; package dependency closure remains unverified. A fresh clang-tidy attempt failed:
root `build` has no compilation database, and no checks were enabled. No clang-tidy pass is claimed.

LoginFlow constraint follow-up on the same Windows ARM64 host (2026-11-09): after the request gate,
monotonic expiry and held-revocation changes, the shared rebuild passed **21/21 CTest targets on
native ARM64 (19.47 s) and emulated x64 (24.52 s)**. `SyncDLoginContracts` now includes scripted
transport regressions on a manual clock: 403/423/429 at the start, poll, user and root requests,
numeric/HTTP-date/absent/malformed/oversized Retry-After, a delay outlasting the session, expiry with
a candidate in flight, throttled revocation past expiry, refused versus uncertain DELETE, failed
throttle persistence, borrowed credentials and owner destruction. They send no request and are not
real-server throttle acceptance; the previous `login.cpp` could not host them, so no before-run
exists. `SyncDRequestLifetimeContracts` passed with the binding-level constraint test. Actual
Go↔service-entrypoint tests passed 2/2: **5.784 s native ARM64, 5.962 s emulated x64**. The Go
suite, `CGO_ENABLED=0` suite, `go vet`, MyGo vet, i18n (334 messages) and the docs mirror check passed.

Real active-transfer acceptance against server 35.0.2 on the same host, final binaries:
`tools/test-reauth-lifetime.ps1 -Scenario network-active` passed on **native ARM64 (12 min 26 s)
and emulated x64 (14 min 28 s)**. Each run interrupts a positive-byte 8 MiB upload and download
through the owned relay and withholds the acknowledgement of a PUT the server committed; all three
recover by automatic cross-origin failover with matching local/remote hashes and the original
native journal, and the parent-owned cleanup of the remote collection, profile and borrowed vault
copy succeeded each time. The half-open probe regression in `SyncDNetworkContracts` failed before
its fix ("successful recovery probe blocked the active endpoint") and passes after it. Earlier
failing runs of this fixture were a 16 MiB quota refusal (HTTP 507) and that defect, not a lost write.
Failure-stage injection, uncertain MOVE/DELETE and chunked uploads are not covered by it.

Chunk-session acceptance on the same host: an owned Nextcloud 35.0.2 container (`wslc`, loopback
port, SQLite, fresh instance) served HTTPS with a two-day fixture certificate whose root was trusted
only inside the acceptance process; no Windows trust store, hosts file or proxy setting changed. Qt
never proxies loopback destinations, so the fixture reached it through two reserved `.test` names
that only the acceptance relay maps back to the loopback port. `-Scenario network-chunked` passed on
**native ARM64 (2 min 52 s) and emulated x64 (2 min 56 s)**; the before-run with the previous gate
ended "failover blocked, folder offline reason NETWORK_UNAVAILABLE". Upstream `ChunkingNgTest`
with patch 0016 passed 22/22 on both. After the gate change the shared rebuild passed 21/21 CTest
targets (ARM64 15.55 s, x64 21.66 s). Cleanup removed the container, volume, image, bootstrap
credentials and TLS files; only two sanitized manifests remain, and the pre-existing container was
untouched. This run covers one interruption point (inside the second chunk); a withheld final MOVE
was exercised only by the upstream regression, not against a real server. The regular
`network-active` fixture passed again on the final ARM64 build (12 min 24 s). Actual Go↔service
pipe tests then passed 12/12 per architecture over six rounds. One earlier emulated-x64 run of
`TestRealPipeINV12INV13` failed while that fixture was running concurrently; its message was not
captured and it did not recur in fifteen later runs, so its cause is unknown.

Metadata boundary regressions reproduced 26 misclassifications before remediation, including 403
reported as authentication failure and locked identity properties reported as an identity mismatch.
The corrected synthetic suites passed on native ARM64 (0.12 s) and emulated x64 (0.16 s); these are
not real DAV acceptance. Connected account-wide throttle/lock handling remains in progress. Go
constraint-error and contradictory-startup regressions failed before their fixes and then passed
20 repetitions. The mandatory `run-clang-tidy` wrapper was also attempted; the Python launcher was
unavailable (exit 9009), distinct from the direct executable's compilation-database failure.

Protocol 1.6 / Control schema 7 follow-up on this Windows ARM64 host: fifteen-patch SyncD consuming
suites passed 14/14 on native ARM64 (5.82 s) and emulated x64 (7.85 s). The standalone native ARM64
core suite passed 14/14 (12.04 s under concurrent upstream build load), including read-only portable
single-PUT/ETag-download admission, blocked polls/chunks, unchanged journal bytes and the bounded
local CONNECT-relay socket contracts. These local relay tests are not real DAV or TLS acceptance.
The first real mapped-transfer fault run stopped at a test-only hidden-journal enumeration error;
the parent cleaned the reserved remote collection and native credentials. The corrected fixture
subsequently failed a runtime operation with `NETWORK_UNAVAILABLE`; this is still under diagnosis,
not an active-transfer recovery pass. The full real transfer matrix remains pending.

Patch 0015's headless capability regression passed in both rebuilt upstream suites. Their initial
parallel runs each passed 95/96, failing only `PushNotificationsTest`; that fixture uses fixed local
port 12345. Isolated sequential reruns passed on ARM64 (51.54 s) and emulated x64 (51.79 s). Full
serial upstream reruns subsequently passed 96/96 on ARM64 (476.71 s) and emulated x64 (488.43 s),
excluding `UtilityTest`; the initial failed runs are not reported as passes. Clang-tidy
was attempted again with the local Python runtime and remains unavailable without the root compilation
database and `.clang-tidy` configuration. No release or handoff is claimed.

Parallel review reproduced and fixed support-bundle retention deleting unrelated UUID-named files,
business hard failures being lost behind occupied probe slots/open circuits, nonstring automatic-policy
targets being accepted, and cancellation being obscured by a transport EOF. Native ARM64 and emulated
x64 consuming suites then passed 15/15 (9.93 / 18.05 s), including connected hard-failure quarantine and
the added diagnostics ownership/atomicity regressions. Four Go transport/projection regressions passed
20 repetitions (41.168 s); the uncached Go suite and targeted vet/CGO-disabled checks passed afterward.
Final fifteen-patch read-only removal, renewal lifetime and fault-policy fixtures passed on both
architectures, with real Go↔C++ pipe tests in 4.478 / 4.482 s before the latest transport-only refinement.
The live approved server was rechecked as 35.0.2 with maintenance disabled. Active-transfer acceptance
is still being debugged; its failed runs remain failed, with parent-owned content/credential cleanup.

Protocol 1.5 / Control schema 6 renewal contracts passed 12/12 on native ARM64 (3.93 s) and emulated
x64 (4.68 s), including candidate/backup native-vault ownership, cancellation, rollback to the prior
credential or prior absence, interrupted cleanup, committed replacement, identical-password safety,
unavailable-root startup, arbitrary-reference refusal and truthful waiting/paused projections. Both
earlier real-daemon rotation runs against server 35.0.2 passed consent, unchanged account/folder/native
journal identities, pauses, old-password revocation, uploads before/after restart and cleanup; real pipe
tests passed (4.449 / 4.475 s). A later run hit HTTP 423 on a positive GET readback, and its owned profile,
credentials and collections were cleaned. Readback now retries only GET within a bounded deadline.

A nested-verification review found terminal-session replacement could free a still-used `LoginFlow`.
The controlled read-only fixture reproduced native ARM64 access violation `0xC0000005` before the fix;
its parent validated and deleted reserved native keys even after the failed child. A local shared
session lease fixes the lifetime; the ARM64 read-only regression then passed. Final retained-session
read-only and real-daemon acceptance passed on both architectures: cancellation/replacement lifetime,
consent rotation, original account/folder/journal identities, pauses, old-password revocation, renewed
uploads before/after restart, switching/deletion regressions and all owned cleanup. Final real-pipe Go
tests passed in 4.436 s (ARM64) and 4.669 s (emulated x64). No owned profile/credential manifests remained.
This is not an active-file-transfer or uncertain-write recovery claim.

The unchanged fourteen-patch upstream test builds were rebuilt and passed 96/96 on ARM64 (446.07 s)
and emulated x64 (474.92 s), excluding `UtilityTest` because it registers startup integration. No upstream
patch was added. PE inspection matched 117 EXE/DLL build-output files per architecture. Go vet, normal
and CGO-disabled Go tests, MyGo vet/i18n (334 messages), documentation mirrors and whitespace checks
passed. Clang-tidy was attempted and still lacks a root compilation database and `.clang-tidy` file.

The protocol-1.4 manual-switch consuming builds passed 11/11 contracts on native ARM64 (3.27 s) and
emulated x64 (3.95 s), including live reservation/deduplication, unknown-target and force-bypass refusal,
overlapping-operation exclusion, failed-verification gates, healthy deferral epoch retention, and
same-origin resume admission without journal changes. An earlier real-daemon run passed on both
architectures against server 35.0.2: primary/alternate/primary switching, global/individual pause,
interrupted reconciliation across graceful restart, fresh committed-origin recovery, alternate upload,
deletion protection and owned-resource cleanup. The final real-daemon runs then passed on both
architectures, including killing the exact owned child after binding publication while reconciliation
was user-paused, fresh committed-origin verification after restart, failed interrupted receipts,
subsequent reconciliation, and retention of the original two native journal-file identities. These
runs do not verify crashes during active uploads or uncertain remote writes. Final real Go↔C++ pipe
tests also passed (ARM64 4.439 s; emulated x64 4.697 s). Build-output PE inspection matched 116 EXE/DLL
files per architecture.
Go vet and both normal/CGO-disabled Go tests passed. MyGo vet attempts exceeded 120/300-second limits;
the subsequent background retry passed. I18n (334 messages), documentation mirrors and whitespace checks passed.
Clang-tidy remains unavailable: no root compilation database or `.clang-tidy` configuration.

Epoch-bound completion follow-up passed 11/11 contracts on native ARM64 (12.13 s) and emulated x64
(12.98 s). Control regressions reject both old-epoch completion/deletion proposals and completion for
restart-blocked intents without advancing the outbox. Both real adapter fixtures against server 35.0.2
then passed primary/alternate/primary rebinding while asserting each completion's epoch, journal reuse,
transfer/conflict checks, drained shutdown and cleanup. IPC activation remains unconnected.

Control schema 5 durable-switch contracts passed in 11-target SyncD suites on native ARM64
(2.21 s) and emulated x64 (3.86 s). They reopen the actual Control DB after prepared and committed
intents, require interrupted receipts and preserved admission gates, and cover transaction rollback,
stale/duplicate folder completions, paused-folder membership, changed roots, withdrawn origin consent,
empty-account reconciliation and epoch overflow. These are storage/restart contracts, not an end-to-end
IPC switch acceptance claim. Real-daemon regression for the schema change is in progress.
The schema-5 real Go↔C++ daemon regressions then passed on both architectures against server 35.0.2:
alternate staging/consent/verification, watcher-driven nested upload, two-folder upload/download,
independent pause, deletion veto/one-use approval, Login Flow v2 form consent, restart persistence,
confirmed app-password revocation and owned-resource cleanup. Subsequent contract refinements added
local-root relocation refusal, interrupted-switch error projection and IPC refusal to remove a pending
switch target; final consuming builds passed 11/11 on ARM64 (2.58 s) and emulated x64 (3.46 s).
Clang-tidy was attempted again but remains unavailable because the root compilation database and
`.clang-tidy` file are absent. No new upstream patch was required for this storage change.

The recorded fourteen-patch upstream suites passed 96/96 on native ARM64 (439.24 s) and emulated
x64 (485.65 s), using `build/src-809fadb79180-8414DE1F7525` and test trees
`build/at-C9CAA3EB6A0A` / `build/xt-C9CAA3EB6A0A`. SyncD built against these exact trees passed
10/10 contracts on ARM64 (11.91 s) and emulated x64 (12.65 s). Both real adapter runs against server
35.0.2 passed primary/alternate/primary rebinding, fresh identity checks, original-journal reuse,
upload/download, modifications, conflict preservation, unchanged-cycle request checks, actual
account/network/reply shutdown drainage and owned-resource cleanup. This verifies the adapter/runtime,
not durable Control/IPC switching or complete crash/uncertain-write recovery.

The recorded thirteen-patch upstream suites passed 96/96 on native ARM64 (435.98 s) and emulated
x64 (477.02 s). Real alternate-address rebinding then correctly refused a remaining download resume
row. Two new upstream regressions reproduced the completed-download cleanup defect on ARM64
(normal completion and resumed completion both failed their no-resume-row assertions). Patch 0014
corrects that key. Its complete ARM64 DownloadTest passed 7 entries in 1.059 s, with the existing
Windows directory-permission test skipped; both new failure assertions now pass. The real adapter
then passed alternate-address rebinding after fresh UID/account-root/folder-root checks, original
journal reuse, upload/download/modification/conflict checks, zero retained shutdown resources and
owned-resource cleanup against server 35.0.2. Combined fourteen-patch acceptance remains in progress.
The extended ARM64 fixture also passed primary → alternate → primary rebinding with monotonically
increasing epochs and fresh identity checks at each transition. Its consuming SyncD build passed
10/10 contracts (2.03 s), including missing-column and invalid-location journal refusal. These runs
exercise the adapter/runtime; durable Control switching and crash recovery remain unverified.

The ARM64 interim thirteen-patch-linked SyncD build compiled the resume-context gate and
account-scoped retirement/rebinding methods and passed 10/10 contract targets (2.02 s). The new
read-only gate covers absent, corrupt, missing-schema, empty and pending poll/upload/download
journals, checking that existing bytes remain unchanged. Real alternate-address rebinding acceptance
is in progress; these contract results do not establish durable Control/IPC switching.

Journal/lifetime work in progress: MyGo v0.8.3 migration preview, Go vet/tests (including
CGO-disabled tests), MyGo vet/i18n and documentation checks passed. The first eleven-patch ARM64
upstream build exposed missing direct SQLite linkage in the new commit-hook regression; patch 0011
now links that test explicitly. The corrected recorded build is still under acceptance.
An interim ARM64 SyncD build linked to the newly compiled journal code passed 9/9 contracts (2.09 s),
including active-versus-retained replies, duplicate completion and unknown write outcomes.
Two real adapter attempts failed at the initial cycle and cleaned their owned fixtures; retirement
acceptance is not claimed. The fixture now emits only allowlisted reason codes and a numeric exit
code on failure to diagnose this without exposing server data. Clang-tidy remains blocked by the
missing root compilation database/configuration.
The commit regression initially ran its write in autocommit mode; it now explicitly begins a journal
transaction before enabling SQLite's rejection hook. The corrected focused ARM64 run passed all
three QtTest entries in 111 ms, including actual rollback and sticky failure after close/reopen.
The real adapter diagnostic confirmed successful transfer followed by one retained account/network
manager and zero live replies. Patch 0012 headless-account validation is in progress; earlier transfer
success must not be reported as successful shutdown or switch quiescence.
The final eleven-patch upstream suites then passed 96/96 on native ARM64 (440.51 s) and emulated
x64 (477.25 s). With draft patch 0012, the focused ARM64 account lifetime test passed all three
QtTest entries (24 ms); the real ARM64 adapter fixture subsequently passed upload/download,
modifications, conflict preservation, journal reuse, zero retained accounts/managers/replies at
shutdown and owned-resource cleanup. Patch 0013 journal preservation and the combined thirteen-patch
build still need acceptance. These results do not enable endpoint switching.

2026-11-07 recursive-watcher/policy build: eight CTest targets passed on native Windows ARM64
(1.27 s) and x64 under emulation on the same host (2.75 s), using Qt 6.12.0 and the existing
ten-patch Desktop v35.0.3 engine. Native filesystem tests cover nested creation/modification,
rename/delete, dirty work during a cycle, pause preservation, real buffer overflow and pending-I/O
teardown. No registry or Explorer integration is used. Clang-format ran on changed C++ files.
The portable-Python clang-tidy retry could launch LLVM but failed because the MSBuild tree has no
compilation database and the repository root has no `.clang-tidy`; static analysis is unverified.
Both actual daemon architectures then passed the server 35.0.2 two-folder fixture, including a new
nested local file uploaded without a sync command within 25 seconds, both-address registration,
independent pause, deletion protection, login restart and confirmed password/profile/fixture cleanup.
Go vet/tests (also CGO-disabled), MyGo vet/i18n, documentation mirror and diff whitespace checks passed.
This does not establish engine-connected switching or uncertain-write recovery.

2026-11-07 endpoint registration: actual daemon fixtures passed on native ARM64 and emulated x64
against server 35.0.2 using both approved addresses. They checked public TLS staging, refusal to verify
before consent, explicit approval followed by UID and both mapped-root comparisons, endpoint removal,
and continued canonical-address sync. Two-folder deletion approval and login/profile/password cleanup
also passed. Six CTest targets passed (ARM64 1.01 s; emulated x64 1.65 s), including consent persistence
and restart invalidation. Go vet/tests (also CGO-disabled), MyGo vet/i18n and doc checks passed.
This does not verify endpoint switching, capability/permission completeness or native x64 hardware.
The failed-public-stage retry regression subsequently passed in the final six-target runs
(ARM64 1.08 s; emulated x64 1.87 s); rejected unapproved records cannot permanently consume a URL.

Latest x64 follow-up: Desktop v35.0.3 with all ten patches built in `build/xt-F8433E84771B`;
96/96 selected upstream targets passed in 479.75 s under Windows ARM64 x64 emulation.
Engine-linked SyncD built against that tree and passed 5/5 contracts in 1.23 s. Its real-daemon
two-folder/login fixture against server 35.0.2 also passed upload/download, pause isolation, deletion
veto/one-use approval, restart persistence, confirmed password revocation and owned-resource cleanup.
These results do not substitute for native x64 hardware acceptance.

The subsequent completion-storage fault regression passed on ARM64 (5/5 CTest targets, 0.77 s)
and emulated x64 (5/5, 1.18 s). It injects an outbox INSERT failure after folder/proposal writes,
checks complete rollback and an unchanged event cursor, and requires fail-closed daemon exit.
The normal success case also checks that a default-null Qt reason becomes non-null empty SQL text.

2026-11-06 connected folders: native Windows 11 ARM64, Qt 6.12.0, MSVC 14.51.36231,
Desktop v35.0.3 with patches 0001–0010 (`build/ae-F8433E84771B`), server 35.0.2.
`tools/test-login.ps1 -SyncFolders` passed actual-daemon two-folder upload/download, independent
pause, deletion blocking, matching one-use approval and full owned-fixture cleanup. The rerun after
retry/safety classification changes passed again; five connected CTest targets passed in 0.88 s,
including deterministic transient retry, independent pause and stale-failure rejection.
The separate latest ARM64 upstream suite (`build/at-F8433E84771B`) passed 96/96 selected targets
in 433.35 s, including patch 0010's pre-propagation abort/retry regression in SyncEngineTest.
UtilityTest remains excluded because it creates a real startup entry. Full SyncD recovery and
engine-driven endpoint switching remain incomplete. MyGo v0.8.1 migration preview needed no edits;
Go vet/tests, explicit CGO-disabled tests, MyGo vet/i18n and documentation mirror checks passed.
The daemon executable and its two upstream engine DLLs passed PE machine AA64 inspection;
this is not a packaged dependency-closure scan. Shared Go/C++ approval-envelope regression tests
also passed; the final connected CTest run was 5/5 in 0.94 s.

2026-11-06 engine/runtime unit: native ARM64 built `syncd_engine_acceptance` against patched
Desktop v35.0.3 and passed the server 35.0.2 isolated fixture, including unchanged-cycle request
counters and cleanup. Four core/login/scheduler/root CTest targets passed: ARM64 0.84 seconds;
x64 under ARM64-host emulation 0.91 seconds. This is not a shipping-daemon folder IPC acceptance.
The x64 adapter, propagation-safety proposals and full static analysis remain unverified.

Login-unit verification on 2026-11-05: both `SyncDContracts` and `SyncDLoginContracts` passed
on native ARM64 (0.29 seconds total) and x64 emulation (0.40 seconds total), including durable
cancellation replay. `tools/test-login.ps1` passed on ARM64 against server 35.0.2 with confirmed
issued-password revocation and profile cleanup; consent used HTTP HTML-form automation. Go vet,
Go tests (also explicit CGO-disabled), MyGo vet/i18n and documentation mirror checks passed.
Full engine integration, native x64 hardware and interactive browser consent remain unverified.

Connected-unit verification on 2026-11-05: Qt 6.12.0 / MSVC 14.51.36231 core CTest passed
on native ARM64 and x64 emulation (one nonempty contract target per architecture). The native
ARM64 real Go-to-C++ Named Pipe test passed against `build/syncd-ARM64/Release/nextcloud-mygo-syncd.exe`.
It exercised reconnect, daemon survival after UI disconnect, explicit shutdown and durable pause
across a daemon restart; its unique local profile was removed. No server data, registry or Explorer
integration was touched. Cross-user/session/spoofing acceptance and real engine switching are unverified.

The same real-pipe test also passed with an x64 Go client and x64 daemon under ARM64-host emulation.
Final core reruns after request-validation tightening passed in 0.22 seconds (ARM64) and 0.13 seconds
(x64). PE checks covered the native UI, both daemon/test EXEs and both architectures' QtCore, QtSql
and SQLite driver DLLs (11 files total, AA64 / 8664 as appropriate), not a release dependency closure.
The native connected window was inspected in `build/native-connected.png`; sign-in was explicitly
unavailable and Continue disabled. The smoke run's processes and profiles were removed; the local
test profile directory was empty afterward. `go vet ./...`, `go test ./...`, explicit
`CGO_ENABLED=0 go test ./...`, MyGo vet/i18n, documentation mirror and whitespace checks passed.

Desktop v35.0.3's nine-patch GUI-off engine subsequently built on native ARM64 in
`build/ae-D9ACB15A1ED9`. All three output EXE/DLL files report AA64, and CLI `--help` exited zero.
The latest native ARM64 artifact subsequently passed the isolated real-server fixture against
server 35.0.2: initial upload/download, modifications, conflict-content preservation, stable journal
reuse and an unchanged cycle with discovery but zero DAV content GET/write requests (initial-transfer
positive control passed). Owned remote collection, local profile and CLI cache cleanup passed.
This is basic upstream-engine acceptance, not full SyncD sync or switching acceptance; the new-version
upstream regression suite remains pending.

Verified on Windows 11 ARM64 (2026-11-03; Go 1.27.3, MyGo v0.7.4):

| Check | Result |
|---|---|
| Go module | `go vet ./...`, `go test ./...`, `go tool mygo vet .` and `go tool mygo i18n check` pass |
| Documentation mirror | `go run ./tools/checkdocs` passes |
| Interface | `go build` makes a native ARM64 executable that starts with its window and tray against the Mock and runs without errors; every page renders in light and dark, English and Simplified Chinese (view tests and screenshots) |
| Installer | ARM64 and x64 packages pass the architecture check; the ARM64 package installs, reinstalls over itself and uninstalls per user with nothing left, its UI and SyncD running as ARM64 from the version directory; the x64 installer refuses ARM64 ([packaging](packaging.md)) |

### Joint release acceptance (B14)

Run on 2026-11-12 on the native Windows 11 ARM64 host against the shared test server (Nextcloud
35.0.2, publicly trusted TLS), through the installed package and the real window. The package was
built from the current tree (`tools/package.ps1 -Arch ARM64`: UI rebuilt, `build/syncd-ARM64`
containing every backend change); `tools/pkg check` passed on 30 PE files, all native ARM64. The
window was driven by Windows UI Automation from PowerShell (its tree exposes every button, field,
check box, menu item and text), each step captured with PrintWindow and read back. Sign-in used Login
Flow v2: the interface opened the browser, and the link from Copy the sign-in link was granted in
memory as `tools/test-login.ps1` does. Remote changes went through WebDAV, only inside one owned
folder. Wall time from install to uninstall: 23 minutes.

| Step | Result | Checked by |
|---|---|---|
| 1 Install, setup, sign in, folder, first sync | passed with a detour: install 2 s; Your server, Sign in, Folders; the server's folder list is read once at sign-in, so the owned folder created afterwards was missing; the interface was killed (SyncD kept running, the new window showed the account) and the folder added through Add a folder under a new local parent chosen in the system folder picker; first sync 10 s. The Start step of setup was not exercised | interface, local files |
| 2 Local add, modify, rename, delete; remote add, modify, delete | partial: both sides matched by names and contents within 4 s each round; Activity lists the uploads, downloads and the server deletion but nothing for a local deletion caused by the server or for a rename, and shows its times in UTC | interface, WebDAV, local files |
| 3 Bulk deletion | passed: 25 of 30 files deleted on this PC; the review appeared in Overview and Activity within 2 s (count, direction, group, expiry); Decide later kept it with all 30 files on the server; Allow asked a second time; afterwards the server held 5 | interface, WebDAV |
| 4 Conflict | passed: both versions kept (the server's in place, this PC's as a conflicted copy), the conflict shown in Activity and closed with Mark as resolved | interface, local files |
| 5 Pause and resume | passed: a folder paused by the user stayed paused through Pause sync and Resume sync; a file created meanwhile was not uploaded in 20 s and was uploaded after Resume | interface, WebDAV |
| 6 Addresses | partial: the alternate address added as Internet (certificate and server shown before any credential, explicit approval, verified in 6 s, answer times of both); Use this one during a 20 MiB upload showed the pending switch for 20 s until the transfer finished, then switched with its reason, the upload's hash matched; switched back in 8 s. Fixed and automatic cannot be chosen while `endpointAutoSwitch` is false. A first attempt during a 64 MiB upload hit the account's quota (shown correctly as a full server); the pending switch then vanished without a switch or a reason | interface, WebDAV |
| 7 Close, reopen, quit | passed: closing the window kept the UI and SyncD running and both sides synced within 5 s; the tray icon and flyout worked; the Start menu shortcut raised the running instance; Quit and stop syncing ended both processes within 1 s | interface, processes |
| 8 Reinstall over the running copy | partial: same-version reinstall in 3 s closed the interface and stopped SyncD through `daemon.shutdown` (exit code 0, not terminated); account and folder kept, sync resumed within 8 s. The installer does not call `daemon.prepareUpdate`. Diagnostics then showed the self-check failed with `CANCELLED` and protocol 1.8. No higher-version package: `tools/package.ps1` takes the version only from the committed `mygo.json` | interface, processes, local files |
| 9 Remove the account, uninstall | passed: removal asked first, setup returned in 2 s, the saved credential was gone and no Nextcloud MyGo app password active in the last 10 minutes remained on the server; `uninstall.exe -uninstall` left no process, shortcut, Apps entry or install folder (no startup entry had been created), and the 15 local files stayed | interface, registry, server token list |

The exported support bundle (1,033 bytes) contained no password, user name, server name, local path,
folder or file name. Product findings of this run: Activity and Recent switches format times without
converting to local time (`clock`, `internal/gui/text.go`); SyncD's diagnostics and start report
still say protocol 1.8 (`backend/control_diagnostics.cpp`, `backend/startup_control.cpp`); the
self-check counts every folder in error, and after an ordinary shutdown it reported `CANCELLED`
(`selfCheck`, `backend/control_update.cpp`; cause not established); a switch pending while a cycle
fails disappears without a reason; setup preselects Documents and Photos and never re-reads the
server's folders (`loadRemote`, `internal/gui/setup.go`); quitting left `window-state.json.tmp` in
`%AppData%\Nextcloud-MyGo`; the protocol has no activity kind for a local deletion or a rename.

Not run: sleep and resume (needs the user), the Start step of setup, notifications, the startup
entry, an upgrade between two versions, on-demand folders, x64 hardware. Capability evidence:
`fullSync` passed every basic-sync, conflict, deletion-review and pause step here, but Activity is
incomplete and no 24-hour run exists; `endpointAutoSwitch` has only a manual switch deferred
correctly to a safe point and the vanished switch above; `virtualFiles` was not exercised. All three
stay `false`.

### B14 findings fixed in SyncD and setup

Same ARM64 host, 2026-11-12, SyncD rebuilt for ARM64 and x64 from this tree, protocol 1.10:

| Finding | Cause | Fix and evidence |
|---|---|---|
| Diagnostics said protocol 1.8 | literals in `control_diagnostics.cpp` and `startup_control.cpp` | both use `Control::protocolVersion()`; `testProtocolVersion` compares `contract.json`, `session.hello` and both diagnostics with `Control::protocolMinor` |
| Self-check `failed CANCELLED` after a reinstall | the cycle stopped at shutdown ends `CANCELLED` and was stored as a folder error; the next start kept it and `selfCheck` counted every folder in error | the start clears `CANCELLED` to offline and the self-check counts only mappings that did not attach; `previousCycle` (CANCELLED, SYNC_FAILED) in `SyncDFolderErrorContracts` |
| A switch vanished | a user request whose running cycle outlasted the 30-second drain window was dropped (receipt failed, pending policy deleted) with nothing in the snapshot, and the interface does not poll receipts | the request ends as a `switchFailed` activity with its reason; `switch-quota` failed before the fix at its outcome step and passed after it, 74.8 s through its wrapper on Nextcloud 35.0.2 |
| No Activity for local deletions or renames | the engine adapter counted only uploads, downloads and remote deletions | `deletedLocal`, `renamedRemote`, `renamedLocal`; `testCycleActivity`; the interface shows them (`TestActivityKinds110`, against the Mock) |
| Setup used `daemon.shutdown` only; no second version | see [packaging](packaging.md) | setup calls `daemon.prepareUpdate`; the package smoke upgraded 0.1.0 to a 0.1.1 test package over a running SyncD in 36.0 s (no account) |

Runs: fast tier ARM64 passed in 38.4 s. Fast tier on emulated x64 failed twice at
`TestRealPipeUnavailableStopsINV11INV12` (the daemon exited 1 after its diagnostic shutdown) while
another build loaded the host; the test passed 3 of 3 alone and all real-pipe tests passed together, so
it is recorded as unresolved, not passed. The server tier on ARM64 stopped at `chunk session resume`,
which exceeded its 150 s budget by 2.1 s (148.3 s before) under the same load; every case run before
it passed. The B14 steps were not run again through the installed interface.

### Simulated sleep and resume (R15)

Same ARM64 host, 2026-11-12, Nextcloud 35.0.2 in a disposable container. The user ruled out a real
sleep of this machine; `sleep-resume` ([development](development.md)) replaces it and is recorded
as a simulation. Through its wrapper it passed in 86.9 s alone and 87.7 s in the server tier: the
acceptance process was suspended for 35.0 s, and both a control-thread timer and an independent
thread stood still for 35.0 s. After waking, the held upload failed at once (`NETWORK_UNAVAILABLE`,
one transport failure), no local file went and no deletion was proposed. The account switched to
the alternate 16.6 s after waking with reason `unreachable`, the upload completed once 21.1 s
after waking, and hashes, the native journal file, folder rows and the credential reference were
unchanged.

The first runs found a SyncD bug: the switch committed, but reconciliation on the alternate waited
another 47 s. That was the 60-second retry delay of a cycle start refused on the unreachable
primary; a failure backoff can reach 300 s the same way. Rebinding now drops the folders' retry
delay and failure count ([engine](engine.md); `rebound` in `syncd_scheduler_tests`). Completion
fell from 68.6 s to 21.1 s after waking, and the server-tier failover cases became faster too
(active upload 113.2 s before, 59.6 s now). The power-notification handler already existed; it now
has an injected-clock test (`resume` in `syncd_endpoint_policy_tests`).

Runs with these changes, built together with other sessions' uncommitted work in the same tree:
fast tier ARM64 passed in 37.2 s and emulated x64 in 39.2 s. The server tier on ARM64 passed on
its third run in 457.5 s of 600 s, all 21 cases (server start 61.5 s; `kill at reconciling`
139.1 s). The first run stopped at `kill at reconciling` ("restart recovery transferred reconciled
files again") under load from another session's build. That stage then passed alone in 132.2 s;
the cause was not established. The rebind change is a no-op on that path unless a cycle failed
before the recovery rebind. The second run failed 10 s after server start, in two lanes at once,
when the alternate's first public probe failed before any changed code ran.

What a real sleep adds that this simulation cannot show: the power transition itself (S3,
hibernate, Modern Standby) and `PBT_APMSUSPEND` / `PBT_APMRESUMEAUTOMATIC` reaching the running
daemon (the simulation delivers none; `nativeResume` posts one to the monitor's own window);
whether `QElapsedTimer` (QPC) counts the sleep on this hardware; adapters going down and up with
address, route and DNS changes; connections that die without a reset and wait for upstream's
transfer timeout; sleeps of hours, with server sessions and locks expiring; volumes or drive
letters changing while asleep; and x64.

### Acceptance audit for 0.1.0

Same ARM64 host, 2026-11-12, SyncD of `build/syncd-ARM64` and `build/syncd-x64` (emulated), protocol
1.10, Desktop v35.0.3 (latest, checked that day), Go 1.27.4, MyGo v0.9.0. The [acceptance
matrix](acceptance.md) was compared row by row with the tests and runs recorded here; these tests were
added for cases that had no real run:

| Test | Cases | Native ARM64 | Emulated x64 |
|---|---|---|---|
| `TestRealPipeFramingR08R12` (real pipe) | R08, R12 | passed, 16.2 s | passed, 17.1 s |
| `TestRealPipeReplayR10R11R14` (real pipe) | R10, R11, R14 | passed, 1.2 s | passed, 2.3 s |
| `TestRealPipeSlowClientR13` (real pipe) | R13 | passed, 2.6 s | passed, 9.6 s |
| `TestRealPipeImpostorR09` (real pipe) | R09 | passed, 2.4 s | passed, 3.1 s |
| `TestRealNewerControlSchemaR17` (real pipe) | R17 | passed, 2.3 s | passed, 2.6 s |
| `SyncDPropertyContracts` (`backend/property_tests.cpp`) | P04, P08 | passed, 0.18 s | passed, 0.40 s |
| `TestUpdateInterruptedDownload` (`internal/release`) | R16 | passed | passed |
| `file-safety`: 100 server deletions under a fifth of the baseline | S11 | not run yet: the server tier waits for another session | not run (x64 runs the smoke only) |

What they showed. A frame split inside its length prefix and its body is one message, and two frames
in one write are answered in order. A zero length, a length above 1 MiB, invalid UTF-8, a duplicate
key, truncated JSON and nesting above 64 each end only that connection; the next client is served
and the paused state is kept. A half frame that never completes holds the single pipe instance for
15.0 s, the stall deadline, and no longer. A client that never reads is dropped once its replies
pass the 4 MiB output budget (ARM64 after 2.3 s, x64e 8.6 s) and the next client is served at once;
`events.resume` answers `RESYNC_REQUIRED`. A change committed right after a bootstrap reaches the
subscription with a later `seq`. `session.hello` with major 2 is `PROTOCOL_MISMATCH`, and a mutation
or a bootstrap sent before a successful hello is refused the same way and executes nothing. A pause
whose reply was never read committed once; its key sent again after reconnecting, after a later
resume and after an explicit stop and a new boot returned the same receipt and never paused again;
the same key with other input returned `OPERATION_KEY_CONFLICT` with state and revision unchanged.
A process of the same user that created the pipe name first is refused by the client (its
executable is not the paired daemon), and the daemon then exits nonzero instead of serving under
another owner; a client through `\\127.0.0.1\pipe\` is refused as remote ("Access is denied"). A
Control database one schema newer than the daemon makes it serve only `databaseUnavailable`, refuse
mutations and leave the file byte for byte. The property run drives `EndpointPolicy` with 500 seeded
sequences of 400 random events (configuration with random trust flags, reviews, network changes,
probes, outcomes, hard failures, activations, abandoned tickets, business successes and clock
steps) and checks after every event that no stale or consumed ticket is accepted or changes health
(P04) and that no proposal names a filtered endpoint or, for a fixed target, another one (P08); with
a deliberately wrong model it failed at seed 1 and shrank to a two-step sequence. A download whose
connection breaks halfway through the archive leaves the staging folder empty.

With them, `tools/test-all.ps1 -Tier fast` passed on ARM64 in 53.6 s (format and vet 2.0 s, Go tests
with real pipes 38.7 s, 23 CTest targets 12.9 s) and on emulated x64 in 53.6 s (Go tests 40.4 s, 23
CTest targets 13.1 s); the new real-pipe tests run in parallel, the half frame's 15-second stall
being the longest.

No product defect was found by these tests. Cross-user and cross-session pipe connections, a second
logon session and native x64 hardware remain outside this machine ([acceptance](acceptance.md#known-limitations-for-010)).

### Data-safety and recovery cases

Same ARM64 host, 2026-11-13, disposable Nextcloud 35.0.2 (`tools/test-server.ps1`), SyncD built from
the recorded commit plus these changes (native ARM64); each case ran alone through
`tools/test-reauth-lifetime.ps1 -Scenario data-safety -Case <name>` ([development](development.md)).
x64e did not run these cases.

| Case | Cases covered | Result, ARM64 |
|---|---|---|
| `watcher` | S09, S10 | passed, 104.1 s |
| `root-denied` | S13 | passed, 38.2 s |
| `maintenance` | S14 | passed, 80.4 s (runs alone: maintenance is instance-wide) |
| `partial-listing` | S14 | passed three times, 16.3–19.7 s; before the fix the vanished storage's local copy was deleted |
| `root-replaced` | S15 | passed, 50.0 s |
| `journal` | S16 | passed, 54.1 s |
| `names` | S17 | passed, 58.0 s |
| `excluded` | S18, S21 | passed, 43.1 s (local halves only) |
| `discovery-kill` | R02 | passed, 62.8 s |
| `revocation` | R07, R22 | passed, 45.4 s |
| `interrupted-change` | S05 | passed three times, 79.3–82.0 s, with the upload and the download both cut; before the fixes the server kept a truncated file, and the folder never left discovery once the network came back |
| `TestRealPipeConcurrentSourceR12` (real pipe) | R12 | passed, 2.5 s |
| `SyncDRootContracts` long path (`root_tests.cpp`) | S17 regression | passed, 6.1 s; failed before the fix |

What they showed. A change written after discovery while the cycle's upload was held started the
next cycle on its own three times, and a burst of 300 long names written while the control thread was
busy overflowed the 64 KiB watch buffer (a probe watcher of the same size saw the overflow) and the
audit found every change, both within the daemon's 48–72 s poll. A root whose listing is denied, a
root replaced by an empty folder while the daemon was stopped, a journal whose header was damaged and
a junction or offline file in the tree all stop the folder before propagation (`UNSUPPORTED_FOLDER_MODE`,
`FOLDER_ROOT_CHANGED`, `JOURNAL_ERROR`) with nothing deleted, uploaded or downloaded and the journal
kept byte for byte; these blocks hold until the daemon restarts (see below). A drive alias defined with
`DefineDosDevice` is refused as a root. Maintenance mode reads as `NETWORK_UNAVAILABLE`, not as an
empty server. Case clashes, names Windows cannot hold, NFD names (stored as NFC by the server),
non-BMP names, paths above 300 characters and a locked file follow upstream's rules without holding
other files back. A kill during remote discovery restarts on the same journal and finishes the
waiting changes once. An app password revoked on the server puts the account into `revoked`; no
secret is in any profile file, and the support bundle and the daemon's log hold no user, server, path,
file name or content.

Found and fixed: the pre-propagation scan (`RootLease::validateTree`, `backend/root_windows.cpp`)
called `GetFileAttributesW` and `FindFirstFileW` with plain paths, so any directory past `MAX_PATH`
refused the whole folder as unsupported, while upstream syncs such paths; it now uses
extended-length paths, and `SyncDRootContracts` checks a tree with a path above 400 characters.

Found and fixed afterwards (data integrity; same host, server and procedure, SyncD built against
the upstream engine with patch 0018, ARM64; each case three times through the wrapper):
- S05, server side: Nextcloud 35.0.2 writes any PUT that replaces an existing file over it in place
  ([upstream](upstream.md#nextcloud-server-apis)). A short PUT sent by hand to an existing 1 MiB file
  left it at 294,912 bytes with the old size and ETag still listed, until a GET made the server
  correct its cache and give it a new ETag; the same PUT to a new path left nothing, and one cut into
  a chunk session left the target intact. The earlier hand test had not reproduced it because the
  truncation shows only while the cut request is still running on the server or after something
  reads the file. Once the ETag changes, the next cycle sees a change on both sides and upstream's
  conflict handling puts the truncated copy at the path, keeping the good content only in a local
  conflict copy. Patch 0018 sends modifications as chunked uploads; in the three runs the cut chunk
  was answered 400 in the session, the server kept the previous version, and the next cycle resent
  it and assembled it with one MOVE.
- S05, SyncD side: after both addresses were refused and given back, the next cycle's first listing
  was never sent (no connection was opened for it) and the folder stayed in discovery until the
  case gave up. The cycle after an unsuccessful one now starts on a new network access manager.
- S14: SyncD deleted the local copy of the external storage: the vanished mount point is left out
  of the parent's listing for the engine's properties, and two removals stayed below the deletion
  thresholds. A cycle that would remove a mount point or all of its content now stops as
  `DISCOVERY_INCOMPLETE` and retries ([engine](engine.md#completion-and-safety)).

Fixed on 2026-11-13: the interrupted download's temporary file (`.remote.bin.~<id>`, 3 MiB of the
new remote content) was uploaded as a new file by the recovery cycle, although `]*.~*` is in the
exclude list. The engine was given the folder path without its trailing slash, so upstream keyed
the exclude patterns to a base path no file is below and applied none of them; SyncD now passes the
slash ([engine](engine.md#completion-and-safety)). S05 plants a `.<name>.~<id>` file too and
requires the server listing to hold only the two real files.

Found, not fixed:
- Hard folder blocks (`UNSUPPORTED_FOLDER_MODE`, `JOURNAL_ERROR`, `FOLDER_ROOT_CHANGED`) stay after
  their cause is gone until a restart, and "Sync now" does not clear them. This follows `PLAN.md`
  §12.2 (a manual retry never bypasses a safety block); whether an explicit retry should re-run
  the check is for the user to decide.

With these fixes, `tools/test-all.ps1 -Tier fast -Build` on SyncD built against the patched engine
passed on ARM64 (format and vet 20.0 s, Go tests with real pipes 39.5 s, CTest 21.4 s, while upstream
builds ran beside it) and on emulated x64 (Go tests 42.4 s, CTest 24.0 s).

With these changes `tools/test-all.ps1 -Tier fast` passed on x64e in 65.2 s; on ARM64 the Go steps
passed (40.1 s) and CTest stopped only because `syncd_reauth_tests` did not link another session's
switching work in progress; `SyncDRootContracts` passed on both.

### Automatic switching acceptance

Same ARM64 host, 2026-11-12, SyncD of `build/syncd-ARM64-sw` and `build/syncd-x64-sw` (emulated),
a disposable Nextcloud 35.0.2 started with `tools/test-server.ps1`. Each case below is
`tools/test-reauth-lifetime.ps1 -Scenario switching -Case <name>` in its own process and profile
(`backend/switching_acceptance.cpp`, [development](development.md#commands)); the contracts and
properties are `SyncDSwitchingContracts` (`backend/switching_tests.cpp`, ARM64 9.1 s, x64e 10.9 s); with it the fast tier passed in
54.4 s on ARM64 and 65.3 s on x64e.

| Case | Cases covered | Native ARM64 | Emulated x64 |
|---|---|---|---|
| `auto-switch` | N08, N09, N11 | passed, 81.0 s | passed, 83.4 s |
| `auto-fallback` | N14 | passed, 70.5 s | passed, 72.7 s |
| `tls-trust` | N03, N04 | passed, 54.8 s | passed, 82.1 s |
| `tls-auth` | N16, N18 | passed, 111.2 s | passed, 115.3 s |
| `tls-circuit` | N10, N15, N22 | passed, 108.1 s | passed, 108.7 s |
| `redirect` | N05 | passed, 54.9 s | passed, 55.8 s |
| `subpath` | N06 | passed, 41.3 s | passed, 44.0 s |
| `revoke-switch` | N17 | passed, 41.0 s | passed, 45.5 s |
| `new-folder` | N19 | passed, 96.6 s | passed, 70.3 s |
| `root-change` | N20 | passed, 40.7 s | passed, 43.4 s |
| `late-epoch` | N13 | passed, 47.4 s | passed, 51.3 s |

How they run. The relay maps `primary.syncd.test` and `alternate.syncd.test` to the server as in
the other server cases. Where a case needs an answer the real server does not give, a loopback
TLS-terminating proxy inside the test process takes the address: it presents a certificate from a
throwaway CA made with `openssl` under the profile and trusted only in that process, and forwards
HTTP/1.1 to the server over TLS verified against the fixture root. No trust store, hosts file or
proxy setting changes. The 120 s dwell is the only threshold the cases shorten, by aging the
policy's activation time through the test's access to Control; the switching contracts prove it on a
caller clock. Margin, stability, freshness, circuits and every trust check are the daemon's own.

What they showed. With a home address (priority 90 against 50) the automatic policy waited 32 s,
past the 30 s drain window, for a 12 MiB upload held unfinished on the primary, never cancelling
it, then switched with reason `faster` and one new epoch; 20 interface changes in 8 s moved nothing.
A better address that passed verification and failed its first business request after the commit
sent the account back to the freshly verified primary with a further epoch, journals and credential
unchanged. Answers held 6 s late on the old address delayed the commit until every old-epoch DAV
request was answered; late probe answers changed nothing on the new one. Expired, wrong-host and
unknown-CA certificates failed registration and, on the approved alternate, quarantined it
(`TLS_ERROR`) until an explicit review, and the account never moved to it while the primary was
down; an unregistered reachable address got its public check and nothing else. Redirects of the
address in use to an unknown domain and to plain HTTP were never followed and quarantined it
(`ENDPOINT_UNTRUSTED`); with a fixed policy the account then waits for that address, as `PLAN.md`
§8.5 requires. An entry under `/nc/` mapped DAV paths, MOVE Destination and PROPFIND hrefs. A 401
from the alternate alone blocked that address only; a 429 with Retry-After 20 on the address in use
stopped every request to both addresses for its duration without a switch. Three refused
connections spaced 5 s apart opened the circuit (12 attempts, Qt retrying each); 30 notifications
in 3 s gave one half-open probe in 18 s; with both addresses refused for 45 s the probes stayed at
5 and 3 and nothing switched. The server revoking the app password while a switch waited for an
upload failed the switch with `AUTH_REQUIRED`, asked for sign-in and tried no other address. An
alternate that could not check a new folder's root took no switch until it had verified it. A
remote root replaced by another with the same path stopped writes as `ENDPOINT_IDENTITY_MISMATCH`.

Found while running them. The kind chosen for an address (`home`, `vpn`, `internet`) never reached
the policy, so every address scored with priority 50 and only probe times could make one better; it
now sets `PLAN.md` §8.4's default priorities (90, 70, 50) in `Control::refreshNetwork` and never
trust. A replaced remote root is reported as the address's identity mismatch rather than as the
folder's own root change; writes stop either way. `revoke-switch` leaves four failed sign-ins on
the server's brute-force record for the relay's address (about 1.6 s of delay for every later
sign-in from it, growing with each run); the runs above reset it between rounds with
`occ security:bruteforce:reset` on the disposable server. An earlier emulated x64 round that ran
after several revocations failed every later case at sign-in for that reason. `tools/test-server.ps1`
now starts the disposable server with brute-force protection off; on 2026-11-13 six failed sign-ins
in a row there left no recorded attempt and no delay (`occ security:bruteforce:attempts`).

Rebuild for 0.1.0 on 2026-11-13, native ARM64 host, on the bootstrapped `.dev/ci-verify` prefixes
as CI builds: the engine trees with all eighteen patches (`build/cv/ae-40B9DDC19708`, `xe-…`) and
SyncD for both architectures; 24/24 SyncD CTest targets on each, including the new address-kind
priority row (home 90, vpn 70, internet 50). The upstream suite with `UtilityTest` and
`SyncCfApiTest` left out passed 96/96 on ARM64 (437.3 s) and emulated x64 (526.2 s), `ChunkingNgTest`
with `testSmallModificationIsChunked` included. `tools/test-all.ps1 -Tier fast` passed on ARM64
(71.1 s) and x64 (55.3 s), `TestRealDaemonInterface` included. The server tier now holds the
`switching` and `data-safety` cases ([development](development.md#test-tiers)): the default ARM64
tier passed three times in a row against Nextcloud 35.0.2 (483.6, 502.2 and 539.0 s), once against
34.0.5 (498.2 s) and once with `-Extended` (566.3 s, plus 115.2 s for the eight extended cases:
`tls-circuit` 94.4 s, `watcher` 59.2 s, `new-folder` 33.3 s, the others 17–29 s); the emulated x64
smoke passed in 156.6 s. Found while getting there: lanes started together failed their first
`endpoints.stage` as `NETWORK_UNAVAILABLE` on a fresh server; a lane kept the direct address of
`lifetime` for the cases after it, so `late-epoch` waited for a held request on a relay it never
used and S05's transfers finished unheld; fixture reads of a file the cycle held locked or of a busy
server now retry for 10 s instead of failing the case.

### Accelerated soak (R21)

Same ARM64 host, 2026-11-13, Nextcloud 35.0.2 in a disposable container, `tools/test-soak.ps1
-Minutes 60` ([development](development.md)). This is an accelerated 1-hour soak, not a wall-clock
24-hour run: the daemon's timers ran 24 times faster, so the hour carried about a day of polls,
probes and switch timing, and many more faults than a day has. One daemon, two accounts with two
folders each; restart at 12.3 min, so the slope window (the last 40 min, 76 samples at 30 s) holds
the second process only.

Covered in 89 cycles: 372 local adds, 352 modifies, 352 renames and 359 deletions; 356 remote adds,
352 modifies, 348 moves and 348 deletions; 12 uploads of 24–32 MiB (chunked); 5,172 queued polls
and 3,136 engine cycles; 16 primary and 15 alternate outages of 20 s, 14 losses of both addresses,
16 rounds of three 2 s flaps, 9 cuts of a running large upload (3 more found it already done); 69
switches (53 `unreachable`, 16 `faster`), each recorded with its reason and none while a cycle of
the account ran; two renewals, one restart, one daily audit; the interface client connected and
disconnected over the real pipe throughout (its totals were lost when the wrapper stopped it; the
10-minute dry runs counted 48 of 49 cycles connected, one in the restart gap).

Resources over the window, against thresholds fixed after the dry runs: private bytes +4.8 MB/h
(limit 10; 18.7–36.7 MB, large transfers make the band), handles −17/h (limit 60; 516–573),
threads −1/h (limit 4; 12–19), TCP −16/h (limit 20; 3–65, the in-process relay's sockets
included), Control database +40 KB/h (limit 1024; 3.1 MB), journals −20 KB/h (0.6–0.75 MB), CPU
15.4 % in the window's first half and 18.3 % in its second. The harness daemon writes no log.

Checks: every cycle converged with matching SHA-256 hashes on both sides and the final one rehashed
everything; no duplicate upload (718 confirmed of 736 changes; the 18 others were found done by
rediscovery after a cut lost their reply) and upload bytes 75 MB above the changes' own, within the
235.6 MB the relay's 635 cut connections had carried; no deletion beyond the soak's own and no
deletion proposal; no switch across a running cycle. One check failed: three local renames ended at
their old name, the upstream MOVE restore described in [upstream](upstream.md) (open).

Fixed on the way: the Control database grew by about 2 MB a minute, a full projection in the outbox
for every state change; the outbox now keeps the latest 100 events ([IPC](ipc.md),
`testOutboxBounded`, which failed before the fix). The fixture relay now refuses a faulted origin
with a `503`; its earlier close-after-`CONNECT` held requests in Qt until their transfer timeout
([upstream](upstream.md#qt)). With it the server tier failed once at `switch: TLS trust` under the lanes' load (the case then passed alone twice) and passed on the next run in 519.2 s. Not
explained: one 20-minute dry run ended with an access violation (0xC0000005) in its second half;
about five hours of later runs, with a stack printer installed, did not repeat it.

What it cannot show: midnight and date rollover, a real credential or session expiry (renewal used
the user's own password), real sleep, interface and DNS changes, leaks or growth below the
measured bounds (4.8 MB/h of private bytes over 40 min is within the band's noise and not proven
zero), behaviour after days of history, and x64.

## Known limitations

Upstream CTest on native Windows ARM64 passed SyncJournalDBTest, ConcatUrlTest,
AllFilesDeletedTest and FolderWizardTest (4/4, 49.53 seconds; Release with patches 0001–0004,
Qt offscreen). These are upstream tests, not validation of a SyncD implementation.

Patch 0005 was checked on the same native ARM64 host: the timeout/next-fixture regression crashed
at iteration two before the patch and passed 20/20 afterward. SyncEngineTest and
RemoteDiscoveryTest each passed three post-patch CTest runs (60.79 seconds combined).
Full-suite, x64 and static-analysis validation of this patch remain outstanding.

The subsequent broad ARM64 run passed 93 of 96 targets (434.94 seconds; UtilityTest excluded),
including discovery and engine tests. DateFieldBackendTest, NextcloudCmdProvisioningTest and
CfApiShellExtensionsIPCTest failed. Patch 0006 subsequently passed the date-boundary target
(0.09 seconds). Provisioning and Shell IPC investigation continued after that run.

The fresh nine-patch ARM64 build subsequently passed all 96 selected CTest targets in
407.29 seconds (`build/at-8061D4B4EECF/Testing/Temporary/LastTest.log`). UtilityTest remains
excluded; CFAPI runtime integration, native x64 acceptance and static analysis remain unverified.

The corresponding x64 build passed 96/96 selected targets in 497.32 seconds under emulation
on the ARM64 host (`build/xt-8061D4B4EECF/Testing/Temporary/LastTest.log`); UtilityTest was
excluded. This is x64 binary execution evidence, not native x64 hardware acceptance.

PE header scans of each fresh test build's `bin/Release` directory checked 103 EXE/DLL files:
all ARM64 files report AA64 and all x64 files report 8664. This scan covers those build outputs,
not a packaged dependency closure.

The nine-patch GUI-off builds also passed: `ae-8061D4B4EECF` and `xe-8061D4B4EECF` each
produced three correctly typed PE binaries and passed CLI `--help` startup (exit zero).
Real-server reruns of these artifacts are pending test credentials in the process environment.

On 2026-11-04 the upstream CLI and its two engine DLLs built successfully on Windows ARM64;
all three PE headers report AA64. Running `nextcloudcmd --version` succeeded and reported
Desktop 35.0.2daily (recorded release commit), Qt 6.12.0, OpenSSL 3.6.5 and Windows ARM64.
The isolated real-server fixture then passed upload/download, modifications, conflict content
preservation and repeated synchronization with a stable journal path against server 35.0.2.
Its owned remote collection, local profile and CLI cache were removed. This does not establish
endpoint switching or a SyncD build. A subsequent native ARM64 run observed discovery with zero
DAV content GET or write requests on the unchanged cycle, with initial transfers as a positive
control; this is request-level evidence, not transport-byte measurement.

M0 preparation on Windows ARM64 (2026-11-03): the Qt 6.12.0 target SDK's 326 DLLs were scanned
and all report PE machine AA64. QtKeychain 0.14.0 and KDSingleApplication 1.2.1 built successfully;
both output DLLs also report AA64. These dependency checks do not establish engine execution.
The first upstream engine configure stopped at missing OpenSSL; subsequent build and execution
results are recorded above. The Go checks listed in development.md, including CGO-disabled tests, were rerun and
passed; gofmt reported no files and the documentation mirror check passed.

- Native upstream basic sync is proven; product ARM64 support still requires the remaining M0 and release gates.
- The interface talks to `syncd.Mock`, not SyncD: no file syncs, no login reaches a server.
- The tray glyph and the mark are placeholders until the Nextcloud icon comes from the upstream
  release; the executable, shortcut and Apps entry already use it (`resources/icon.png`).
- The tray flyout, notifications, open at sign-in and the update check were not exercised by
  hand; app updates are off in unsigned builds.
