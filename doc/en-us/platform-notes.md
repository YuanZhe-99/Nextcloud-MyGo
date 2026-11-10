# Platform notes

| Platform | Status |
|---|---|
| Windows 11 ARM64 | Connected SyncD core and real Go/C++ pipe passed; full daemon sync not implemented. Upstream CLI basic real-server sync passed (below). |
| Windows 11 x64 | Engine cross-built on ARM64; PE checks and basic sync under x64 emulation passed. No native x64 runtime acceptance. |
| Linux, macOS | Not supported. |

## Windows

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
  release.
- The tray flyout, notifications, open at sign-in and the update check were not exercised by
  hand; app updates are off in unsigned builds.
