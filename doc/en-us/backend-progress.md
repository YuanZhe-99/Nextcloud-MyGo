# Backend progress

## M0 / B01: dependency and interface audit

Status: in progress; no backend milestone has passed its exit criteria.

### Early handoff (2026-11-09)

The backend is deliberately handed off incomplete; no capability may be presented as accepted. The
current ARM64 tree builds and all 21 consuming CTest targets pass (18.17 s), including the new
schema-8 durable-constraint and metadata-boundary targets. The emulated x64 tree builds and all 21
targets pass as well (25.25 s). Real service-entrypoint Go tests pass on both (5.698 s ARM64,
5.865 s emulated x64), including a second corrupt-storage boot's fresh shutdown key. Known in-flight gaps are the LoginFlow 429/expiry cleanup ownership, constraint
observation after a network-generation change, the real active-transfer withheld-acknowledgement
case, modern chunk/MOVE/DELETE recovery, and packaging/update (B13). `HANDOFF.md` lists the exact
files, commands and next steps; do not resume the stopped subagent sessions.

### Takeover after the handoff (2026-11-09)

Two of the handed-off gaps are closed and one was found already closed. `LoginFlow` now classifies
403/423/429 with the shared metadata rules, gates its own requests behind a confirmed server delay,
commits a 429 through the account throttle before sending anything else, expires on a monotonic
20-minute deadline that no delay extends, and keeps a password it acquired owned while a bounded
delay holds its revocation back ([accounts](accounts.md)). Constraint observation after a
network-generation change was already bound to the immutable binding in the tree; its test passes.
The shared rebuild passed 21/21 CTest targets on native ARM64 (19.47 s) and emulated x64 (24.52 s),
and the real service-entrypoint Go tests passed on both (5.784 s / 5.962 s), including the second
corrupt-storage shutdown key. No protocol field, schema version or upstream patch changed.

The real active-transfer fixture (`tools/test-reauth-lifetime.ps1 -Scenario network-active`) then
passed on native ARM64 against server 35.0.2 in 12 min 26 s: an unfinished upload fault, an
unfinished download fault and a server-committed PUT whose acknowledgement was withheld, each with
automatic cross-origin failover, matching local/remote hashes, unconfirmed-write evidence for the
uploads and the original native journal. Two causes had kept it failing. First, a product defect: a
successful half-open probe on the active endpoint was treated as its failure, so a manual switch
back onto a recovering address was failed as `NETWORK_UNAVAILABLE` after passing full verification
([engine](engine.md)); `SyncDNetworkContracts` reproduced it before the fix. Second, a fixture
limit: three 16 MiB files exceed the test account's quota, so the third PUT was refused with HTTP
507 and never became visible; the content is now 8 MiB. The fixture also waits for both addresses
to recover before switching back and requires the primary to be in use before each fault. See
[platform notes](platform-notes.md) for the x64 result.

Still open, unchanged in scope: real-server throttle acceptance for login; automatic revocation
retry across a daemon restart (recovery reports the unconfirmed revocation instead); modern
chunk/MOVE/DELETE recovery, which needs a new upstream regression and an approved isolated server;
and packaging/update (B13), whose dependencies B08/B12 are not accepted, so `daemon.prepareUpdate`
stays undispatched. Full sync and automatic switching stay disabled; this is not a backend handoff.

Parallel handoff review identified additional implementation gaps rather than just missing evidence:
cross-process nested-root ownership, diagnostic/crash-storm startup and preserved attachment errors.
Hierarchical root admission and stable error preservation now have isolated dual-architecture
regressions. Protocol 1.7 adds explicit unavailable startup state and folder reasons; shared startup
contracts and actual Go↔C++ pipes passed on both architectures. A subsequent 32-receipt diagnostic
shutdown refinement passed isolated regressions but still requires shared reruns. The real automatic
active-transfer fixture remains unpassed: its latest run could not establish server visibility of the
PUT whose acknowledgement was withheld. Existing same-origin crash evidence must not be promoted
into cross-origin/uncertain-write acceptance. N02 passed against two real isolated Nextcloud 35.0.2
instances using explicitly approved process-local CA trust; independent cleanup confirmed owned
containers, data volumes, credentials, TLS material and observable forwarding entries absent, with
the preexisting container unchanged. Metadata lock/throttle/permission classification fixes are in
progress. Full backend handoff remains false.

2026-11-09 B09–B12 connected path: protocol 1.6 / Control schema 7 now persist endpoint policy,
pending fixed selection, security quarantine and frozen switch evidence. Windows notifications feed
generation-bound bounded probes, and proposals drive the real coordinator with failure cancellation,
fresh-epoch previous-binding recovery and all-bad/fixed waiting. Real read-only fault acceptance passed
on native ARM64 and emulated x64, including failed-target verification and fresh previous-origin
recovery; these tests use no mapped content and are not active cross-origin transfer acceptance.
The live daemon fixture separately passed fixed-policy pending/actual separation and paused-root
reconciliation. It also passed process kills during positive-byte unfinished 16 MiB upload/download,
same-origin restart rediscovery, matching hashes and original journals on ARM64.

The extended fixture exposed an endpoint-removal lifetime failure after the prior completed policy
sequence. A controlled read-only nested-removal regression reproduced ARM64 `0xC0000005`; owned
endpoint keys, entry reacquisition and per-step consent checks remediate it. The regression and the
full ARM64 policy/watcher/deletion/active-crash/renewal/cleanup fixture then passed. Final consuming
13/13 ARM64 contracts passed in 5.55 s. Both-architecture consuming regression, support-bundle/privacy
acceptance and final evidence are still being completed. Aggregate confirmed-item activity and
sanitized runtime diagnostics/local atomic bundles are connected without logging filenames or secrets.
The adapter now installs filtered approved-origin capabilities once per epoch. Patch 0015 keeps
capability updates headless; both matching consuming builds passed 14/14. The read-only resume gate
admits relative ETag-download and checksum-qualified single-PUT state, retaining the poll/chunk blocks.
The real manual cancellation/cross-origin and automatic mapped-transfer/lost-outcome matrix is still
in progress. A bounded encrypted relay exists only in acceptance code; it changes no system network
settings. The initial real mapped-transfer fixture stopped on hidden-journal enumeration before
injecting a transfer fault, and parent-owned remote/native cleanup passed. See platform notes for
the initial upstream fixed-port failures and serial reruns. No handoff or new capability is claimed.

2026-11-08 B04/B08/B12 renewal slice: protocol 1.5 and Control schema 6 connect account-preserving
reauthentication, candidate/previous native-vault reservations, crash-safe replacement rollback,
resumable local cleanup and fresh same-origin credential reload. Typed scheduler blockers permit only
credential failures to be cleared by verified renewal. Twelve consuming contracts passed on native
ARM64 (3.67 s) and emulated x64 (4.48 s), including cancellation, prepared rollback, previously missing
credentials, interrupted rollback cleanup, committed restart, identical-credential safety and an
unavailable root without startup termination. The initial ARM64 real-daemon consent/rotation run
passed unchanged account/folder/journal identities, pause preservation, previous-password revocation,
renewed uploads and cleanup against server 35.0.2. Final retained-session suites passed 12/12 on ARM64
(3.93 s) and emulated x64 (4.68 s). A deterministic read-only cancellation/replacement fixture reproduced
ARM64 access violation `0xC0000005` before the local session-lease fix, then passed on both architectures.
Both final real-daemon runs passed renewal/restart transfers, switching, deletion safety and cleanup;
real Go↔C++ pipes passed too. Fresh upstream rebuilds passed 96/96 per architecture, excluding the
startup-registering `UtilityTest`. See platform notes for evidence and the transient locked-GET fixture
retry refinement. No upstream patch or plan decision changed. Wrong-account, SSO/MFA and full
fault-recovery acceptance remain outstanding; full sync and automatic switching stay disabled.

2026-11-08 B11/B12 connected slice: protocol 1.4 exposes manual account-wide switching with durable
acceptance, natural-completion deferral, actual old-runtime retirement, fresh target verification,
gated binding publication and per-folder new-epoch reconciliation. The first native ARM64 real-daemon
primary/alternate/primary run passed, including alternate uploads, independent/global pauses, receipt
completion and cleanup. Interrupted-operation restart coordination now uses a new epoch on the last
committed origin; same-origin transfer rows are retained for upstream recovery. Both contract suites
passed 11/11 (ARM64 2.54 s; emulated x64 3.77 s) before the final precommit-admission refinement.
The final suites passed 11/11 (ARM64 3.27 s; emulated x64 3.95 s). Both real-daemon runs then passed
graceful-restart and owned-process-kill recovery at the committed/user-paused reconciliation boundary,
with the same native journal files, alternate uploads, all-root/UID/capability/read-permission checks,
pause preservation, deletion protection, revocation and cleanup. Real pipe tests passed too. This does
not verify active-transfer crash or uncertain-write recovery. Network-policy activation, controlled
failed-target fallback, reauthentication and the complete fault matrix remain unfinished; no handoff.

2026-11-08 B10/B11 switching prerequisites: account-wide scheduling gates, actual account/QNAM/reply
retirement, monotonic-epoch rebinding and conservative read-only resume-context admission now exist in
the runtime. The standalone guard separates old-epoch drainage from committed-epoch reconciliation.
Patches 0011–0014 expose commit failure, preserve corrupt journals, omit interactive account ownership
cycles and fix completed-download resume cleanup. The recorded fourteen-patch upstream suites passed
96/96 on native ARM64 and emulated x64. The interim ARM64 fixture passed primary/alternate/primary
rebinding, fresh identity checks, original-journal reuse and cleanup; both final-tree adapter runs also
passed. See [platform notes](platform-notes.md) for exact evidence (INV-03/04/05/06/08/09).
Control schema 5 now stores monotonic switch intents/epochs and committed bindings, blocks interrupted
work at startup and restores the runtime gate. Eleven contract targets passed on ARM64 (2.21 s) and
emulated x64 (3.86 s), including preparation/commit crashes, transaction rollback, stale results, changed
root membership, consent withdrawal, empty accounts and epoch overflow. Manual Control/IPC activation
is connected in the later slice above;
network-policy integration, complete crash/uncertain-write recovery and reauthentication remain
unfinished at that checkpoint. Current protocol is 1.6, schema is 7, and full sync /
automatic-switch capability gates stay disabled. This is prerequisite progress, not a backend handoff.

2026-11-07 B06/B10 follow-up: Windows recursive notifications now feed the serial scheduler,
including new subtrees and full-audit recovery after buffer overflow. Tests exercise real filesystem
events, concurrent dirty generations, independent pause and cancellation with pending I/O (INV-07/09).
The deterministic endpoint policy covers trust gates, scoring, debounce, revision-bound results,
circuit recovery and fixed-target refusal (INV-03/04/06/14), but remains proposal-only and unconnected
to runtime network monitoring or switching. Eight CTest targets passed on native ARM64 (1.27 s)
and emulated x64 (2.75 s). No upstream patches or plan changes were added.

2026-11-07 B09 registration unit: Control schema 4 and protocol 1.3 now connect immutable public
staging, explicit origin consent, authenticated UID/account-root/all-folder identity comparison,
retry and removal. Interrupted verification fails its receipt on restart; approved origins remain
approved but lose verified status. Root mapping changes invalidate prior alternate verification.
Native ARM64 and emulated x64 real-daemon runs against server 35.0.2 passed the supplied alternate
address path plus two-folder sync/deletion/login cleanup. Six CTest targets passed on both architectures
(1.01 / 1.65 seconds). Negative consent and interrupted-probe contracts are covered without networking.
No upstream patch was added. Capability/permission completeness, a distinct wrong instance/account,
network monitoring/scoring, fresh health evidence and engine-connected switching remain unfinished;
this is not B09–B12 acceptance. The active address remains canonical.

2026-11-06 B05/B06 implementation progress: an in-process `EngineAdapter`, serial `EngineRuntime`,
deterministic scheduler and Windows root lease are implemented. The native ARM64 runtime fixture
passed real-server upload/download, modifications, conflicts, fixed-journal reuse and unchanged
request checks. Four foundation/login/scheduler/root targets passed on ARM64 and emulated x64.
Subsequent native ARM64 daemon acceptance against server 35.0.2 passed two-folder upload/download,
independent pause, pre-propagation deletion blocking, matching approval and rejection of a consumed
decision. The run also passed login restart persistence, app-password revocation and owned-resource
cleanup. Control schema 3 now persists folders and proposals; protocol 1.2 connects their operations.
Patch 0010 adds an effective pre-propagation veto. The run caught and fixed null-success-reason SQL
binding and inconsistent approval parameter naming. Five connected CTest targets passed in 1.26 s.
Full recovery, endpoint switching and complete acceptance remain open; see [engine integration](engine.md).

Connected-unit update (2026-11-05): B02/M1 foundation now has a real C++ daemon, per-profile
SQLite settings/receipts/outbox, authenticated Win32 pipe, Go client and default frontend connection.
ARM64 and x64 core CTest targets each passed (0.13 / 0.18 seconds; x64 under ARM64-host emulation).
The native ARM64 real Go-to-C++ pipe test passed, including durable pause across restart, reconnect,
UI disconnect without daemon shutdown and explicit shutdown. Standalone switch guards passed trust,
epoch and all-folder quiescence tests, but are not connected to libsync. Login and account persistence
are now connected; see [accounts](accounts.md) for real consent and cleanup evidence. Engine integration,
automatic switching and update preparation remain disabled. This is foundation progress,
not B02 or M1 acceptance; cross-user/session/spoofing and real-server integration remain outstanding.

Latest upstream check found Desktop v35.0.3 (`809fadb79180eeacb3fd5b83c804765822e2cd09`).
Its diff from v35.0.2 changes release metadata, packaging and translations, not engine sources.
The snapshot was advanced with the existing nine patches; previous v35.0.2 runs remain
historical evidence and do not establish v35.0.3 acceptance. Go 1.27.3 / MyGo v0.7.4 are unchanged.
All nine patches replayed successfully and the v35.0.3 native ARM64 GUI-off engine build completed
in `build/ae-D9ACB15A1ED9`. Its initial configure needed the already-prepared pkg-config tool on
PATH; no new upstream patch was required. Fresh upstream regression and real-server acceptance
for this version were initially pending. With maintainer-approved local credentials, the latest
native ARM64 artifact then passed the isolated fixture against server 35.0.2: upload/download,
modifications, conflict preservation, journal reuse and unchanged-cycle request checks. Owned-resource
cleanup passed. The new-version upstream regression suite and full SyncD acceptance remain pending.

Checked on 2026-11-03:

| Component | Observation |
|---|---|
| Repository | Clean `main`, fetched configured remotes, zero commits ahead/behind `origin/main` before work |
| Go / MyGo | Latest stable Go 1.27.3 and MyGo v0.7.4; already used, unchanged |
| Nextcloud Desktop | Release v35.0.2, commit `07761410a90a2f27007932fcb61afc23ddda5998`; source retrieved for audit |
| Nextcloud server | Latest stable tag v35.0.2; supplied test instance reports 35.0.2, installed, no maintenance or pending database upgrade; unauthenticated HTTPS status request only |
| Native environment | Windows ARM64; Visual Studio 2026 18.11.0, MSVC 14.51.36231, Windows SDK 10.0.26100.0 |
| Qt | 6.12.0 ARM64 and x64 SDK archives downloaded from the official architecture-specific repository, SHA-1 checked, extracted under ignored `.dev/Qt`; aqt 3.3.0 could not resolve the new repository layout |
| Architecture inspection | All 326 DLLs in the extracted ARM64 Qt tree have PE machine `0xAA64`; this is not a complete product dependency scan |
| Native dependencies | ECM 6.31.0 installed into the isolated prefix; QtKeychain 0.14.0 and KDSingleApplication 1.2.1 built for ARM64 and installed there; no credential-store or application runtime tests yet |
| First upstream configure | MSVC ARM64 compiler checks passed; configuration stopped at missing ECM before the isolated dependency setup; no engine or upstream test executable built |
| Additional dependencies | OpenSSL 3.6.5, SQLite 3.54.1 and zlib 1.3.2 built and installed for ARM64 through the isolated vcpkg checkout |
| Read-only DAV probe | HTTPS PROPFIND Depth 0 returned 207 with successful collection, oc:id and oc:fileid properties; available quota was 39,211,471 bytes; no write request |

## Verified source contracts

These are source observations, not runtime acceptance results. Paths refer to Desktop v35.0.2.

| Source | Finding | Integration consequence |
|---|---|---|
| `CMakeLists.txt:12,32–33` | C++20; root Qt requirement 6.10.0 | Record the actual selected Qt/toolchain combination after a successful build |
| `CMakeLists.txt:89,256–266` | ECM 6, OpenSSL, pkg-config/libp11, Zlib, SQLite and KDSingleApplication required | GUI-off builds still need these configure-time dependencies |
| `src/CMakeLists.txt:32–48` | QuickWidgets, Core5Compat and QtKeychain lookup | GUI-off does not mean Core/Network-only |
| `src/libsync/CMakeLists.txt:5,259` | KF6Archive; Qt WebSockets, Xml, Sql, Gui, Svg, Widgets | Include these target libraries in both architecture inventories |
| `CMakeLists.txt:333–339` | Tests are skipped unless BUILD_GUI is enabled | Separate upstream test and headless build configurations; zero tests is not a pass |
| `src/libsync/syncengine.cpp` | Global `s_anySyncRunning`, propagation and removal signals | Lifecycle and veto semantics still require detailed audit and runtime proof |

## Test isolation and remaining gates

After patch 0001, GUI-off configuration succeeded and compilation reached Windows common code,
where `utility_win.cpp` includes the GUI-generated `configgui.h`. Patch
`0002-generate-common-windows-icon-config.patch` generates that existing template for GUI-off
Windows builds too, without changing its contents or GUI-on behavior. The regression evidence is
the original C1083 compiler failure followed by successful compilation and linking of
`nextcloud_csync.dll` on ARM64. The next failure was a missing installed libp11 header
(`p11_ver.h`, then `p11_err.h`); both have now been installed from the matching source,
covering the full public header list in libp11's `src/Makefile.am`. The incremental engine
build succeeded on 2026-11-04. `nextcloudcmd.exe`, `nextcloudsync.dll` and `nextcloud_csync.dll`
all have PE machine AA64. The CLI's `--version` ran successfully on Windows ARM64 and reported
Qt 6.12.0 and OpenSSL 3.6.5. The real-server fixture passed initial upload/download, a repeated
cycle with the same journal path, bidirectional modifications and preservation of both conflict
contents against server 35.0.2. Its finally cleanup removed the owned remote collection, local
profile and CLI cache. A strengthened native ARM64 run also passed its request-observation
positive control (initial content GET and writes) and observed discovery with zero DAV content
GET or write requests in the unchanged cycle. It measures request creation, not transport bytes.
The upstream test suite remains unverified.

On 2026-11-04, the interrupted jobs reported successful x64 builds and isolated installation of
QtKeychain 0.14.0, KDSingleApplication 1.2.1, OpenSSL 3.6.5, SQLite 3.54.1 and zlib 1.3.2.
libp11 0.4.21 and KArchive 6.31.0 subsequently built for x64 as well. These are cross-build results on ARM64,
not native x64 runtime acceptance.

The separate ARM64 upstream test configuration reached the GUI dependencies and stopped because
no SVG converter was available. Portable Inkscape 1.4.4 was downloaded from its official media
host, SHA-256 verified against the Scoop Extras manifest and extracted into `.dev/` without an
installer. Test configuration has been restarted with `SVG_CONVERTER_DIR` pointing to its binaries;
no upstream tests have executed yet. No additional upstream patch was needed for these dependency
installation issues.

The x64 CLI and two engine DLLs built and passed PE machine 8664 checks; CLI startup and the
original real-server fixture passed under x64 emulation on the ARM64 host. Native x64 hardware
acceptance remains outstanding. The strengthened request-observation fixture initially failed
its positive control and cleaned its resources. Explicit Qt stderr routing fixed request capture;
the native ARM64 rerun then passed. The earlier x64 run did not include request observation.

The GUI-enabled upstream test build passed SVG generation but failed compiling
`shellextensionsserver.cpp`: its icon configuration header is generated only inside the disabled
CFAPI plugin. Patch 0003 generates that existing header with the upstream default indices when
Shell integration is disabled, preserving explicitly supplied indices and the enabled build path.
It does not build or register the Shell extension. The short-path build compiled and linked the GUI core and application, confirming the missing-header fix.
The next incremental test build hit a 275-character MSBuild tracking path (MSB3501).
The build script now uses short architecture/mode names plus a 12-character source hash and an
isolated staging prefix. That build progressed past the path failure but exposed unconditional CFAPI test linkage and a single-configuration generated-header path in FolderWizardTest. Patch 0004 links CFAPI only when its target exists, builds SyncCfApiTest only with that plugin, and selects the configuration-specific UI header directory for multi-configuration generators. The standalone Shell IPC test remains enabled. CFAPI runtime coverage is unavailable in this configuration; patch verification is pending.

The incremental ARM64 Release build with patches 0001–0004 completed successfully, including
FolderWizardTest and the standalone Shell IPC test. The reused source/build directory names
predate patch 0004; they identify an incremental diagnostic build, not a fresh fingerprinted build.
CTest passed FolderWizard, SyncJournalDB, AllFilesDeleted and ConcatUrl on native Windows ARM64
(4/4, 49.53 seconds, Release, Qt offscreen platform). This verifies the FolderWizard build fix
and exercises upstream journal, deletion-protection and URL contracts; it is not SyncD acceptance.
The next 18 engine-related targets completed in 546.35 seconds: 16 passed, SyncEngineTest
failed and RemoteDiscoveryTest exited with exception 0xc00000aa. Neither failure was a CTest
timeout. Both are being rerun with explicit QtTest text reports to identify the failing cases;
no full-suite pass is claimed.

Explicit QtTest text reports from standalone reruns show SyncEngine: 61 passed, zero failed,
six skipped (bulk upload disabled); RemoteDiscovery: 34 passed, zero failed. These runs used the
repository working directory and file-based result output. The original CTest failures remain
unexplained. A bounded repeat under the original CTest working directory passed SyncEngineTest
three times (22.99, 18.21 and 16.30 seconds), but RemoteDiscoveryTest crashed again after
6.29 seconds with access violation 0xc0000005. Remote-discovery stability is therefore an open
blocker; the standalone pass does not resolve it. Per-case report capture is in progress.

Five runs with file-based QtTest reports passed; with default output, two passed and the third
exited with access violation 0xc0000005. The CTest crash log places the crash at the beginning of
testMissingData, immediately after the timeout row: local-discovery worker messages continue
after the preceding fixture closes its journal and removes its temporary directory. Source
inspection confirms local discovery uses the global thread pool and borrows a raw VFS pointer.
This is a lifetime-race hypothesis, not yet a confirmed root cause; a focused timeout/next-case
reproduction is running.

The focused timeout/next-case sequence reproduced access violation on run three. A proposed
upstream fix gives each discovery phase its own local-worker pool, destroyed before its sync
options (and VFS). This retains worker dependencies until tasks exit without waiting on unrelated
global-pool work. It does not make SyncEngine::finished a quiescence guarantee. Regression and
build verification are pending; run-clang-tidy is unavailable in this environment.

Patch 0005 and `tools/test-discovery-lifetime.ps1` record this proposed fix and its 20-process
regression. The script failed on the pre-fix executable at iteration two (0xc00000aa), confirming
it detects the reproduced fault. The patched RemoteDiscoveryTest and SyncEngineTest are building.
Touched C++ ranges were processed with clang-format; full static analysis remains outstanding.

With patch 0005, the 20-process regression passed 20/20 on native ARM64. CTest then passed
SyncEngineTest and RemoteDiscoveryTest three times each (60.79 seconds total). This is positive
before/after evidence for the worker-lifetime fix, not proof of endpoint-switch quiescence.
The remaining upstream targets are being rebuilt before broader regression execution.

The complete ARM64 upstream target rebuild succeeded with patch 0005. Broader CTest execution
is running with UtilityTest excluded: its testLaunchOnStartup creates a real startup entry,
which conflicts with M0 isolation. This exclusion is also applied by the build script.

The broad ARM64 run completed: 93 passed and three failed out of 96 targets (434.94 seconds),
with UtilityTest excluded. SyncEngineTest and RemoteDiscoveryTest passed. Explicit QtTest
reports reproduce the remaining failures: DateFieldBackend loses a day when UTC midnight is
converted to a local date; NextcloudCmdProvisioning uses a binary path lacking the Release
subdirectory and reads the default exit code after a failed process start; Shell IPC receives
empty thumbnail and custom-state results, whose cause remains under investigation.

Patches 0006 and 0007 address the UTC conversion and CLI test path respectively. The existing
DateFieldBackend boundary test and nine failed provisioning cases provide pre-fix regression
evidence. Post-fix targeted builds and runtime checks are pending. These are upstream UI/test
corrections, not new SyncD capabilities; static analysis remains unavailable.

The targeted ARM64 rebuild succeeded. DateFieldBackendTest passed (0.09 seconds), while
NextcloudCmdProvisioningTest still failed. Its explicit report now has six passes and five
failures: children without explicit profiles exit with 0xc0000409, while all three cases using
temporary profiles pass. The same missing-option invocation with an isolated profile returned
the expected -1. Patch 0008 supplies temporary profiles to the remaining provisioning cases;
its runtime verification is pending. The cause of the default-profile crash remains unverified.

With patch 0008, NextcloudCmdProvisioningTest passed on native ARM64 (13.69 seconds; CTest
total 13.73 seconds). Shell IPC is the only unresolved target from the broad run. A temporary
connection-error diagnostic in the generated source is being used to narrow that failure; it is
not a recorded upstream patch.

The diagnostic confirmed successful connection, write and read, but only a 17-byte reply.
A Nextcloud process was running while the test used the application's fixed pipe name; this
suggests a collision with an existing client. Patch 0009 permits a dedicated server name and
uses a temporary fixture name for both test clients and server. Diagnostic logging was removed.
The existing client was not stopped. Verification with the dedicated pipe is pending.

Patch 0009 passed CfApiShellExtensionsIPCTest on native ARM64 (3.69 seconds; CTest total
3.74 seconds) with the existing client still running. All three original failures now have
passing targeted runs, but a fresh fingerprinted build remains required. The temporary
diagnostic changes were removed before this run.

The fresh native ARM64 Release build in `build/at-8061D4B4EECF` with all nine recorded patches
succeeded; CTest passed 96/96 targets in 407.29 seconds, excluding UtilityTest. Evidence is in
`Testing/Temporary/LastTest.log` under that build. This closes the ARM64 incremental-build
reproducibility gap, not the remaining M0 gates. The x64 attempt exposed a source-reuse check
bug with overlapping patches. Verification now reverses patches sequentially in a temporary
Git index and compares against the recorded commit. The x64 build was restarted; its tests
will run under emulation on this ARM64 host.

The x64 build in `build/xt-8061D4B4EECF` succeeded and passed 96/96 CTest targets in 497.32 seconds
under ARM64-host emulation, excluding UtilityTest. Its `Testing/Temporary/LastTest.log` holds
the evidence. This also exercises the corrected overlapping-patch source-reuse check. Native
x64 hardware acceptance remains outstanding. GUI-off engine builds with all nine patches are
now running for both architectures.

Both fresh GUI-off engine builds succeeded (`build/ae-8061D4B4EECF` and
`build/xe-8061D4B4EECF`). Each CLI and its two engine DLLs passed PE architecture checks
(AA64 / 8664); both CLIs returned zero for `--help`. The x64 run used emulation. The current
process has no test-server environment credentials, so the nine-patch artifacts have not yet
repeated the real-server fixture; earlier real-server evidence remains tied to its earlier build.

The GUI-off configure reached the CFAPI Shell extension despite `BUILD_SHELL_INTEGRATION=OFF`
and failed at `GenerateIconsUtils.cmake` (missing SVG converter). Patch
`0001-honor-shell-integration-for-cfapi.patch` gates the entire Windows CFAPI plugin on that option.
It leaves the upstream default ON behavior intact; full-file M0 builds need neither this plugin
nor its Shell registration code. The patch is applied only to a generated build-source clone.
Reapply it and repeat GUI-off configuration after each upstream release; M6 must explicitly enable
and adapt the plugin. This is build isolation, not proof of deletion or switching safety.

- Use an isolated development profile and uniquely owned local/remote test directories.
- Test credentials, private addresses and raw server responses must stay out of committed artifacts.
- Remove only resources created by the test run; preserve pre-existing user data and configuration.
- M0 tests must not install the official client, register cloud roots, Shell extensions, startup
  entries or file associations. Future registration tests must record and reverse their changes.
- The supplied 100 MB quota can cover small-file acceptance; it cannot cover the >4 GiB case.
- Second-account, previous-supported-major server, alternate-origin, different-instance, x64,
  macOS, signed packaging and 24-hour acceptance evidence remain outstanding.
- The test account was used for the read-only DAV probe and the disposable real-engine fixture.
  No credential-store entry or Explorer registration was requested by the fixture.
- M0 is incomplete pending its remaining checks; B02–B14 remain unverified and the frontend still uses the Mock.
