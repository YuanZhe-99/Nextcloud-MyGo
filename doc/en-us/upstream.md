# Upstream contracts

What Nextcloud-MyGo relies on from its upstreams, and how far each point has been checked.
Nextcloud-MyGo follows the latest release of each upstream, so every point is re-checked when that
upstream moves. The Nextcloud Desktop points below were read from the `master` branch and the
`latest` documentation on 2026-11-03, without building or running anything; M0 re-checks each one
against the latest release and replaces "read" with the result.

## Nextcloud Desktop

| Point | Consequence | Checked |
|---|---|---|
| The root CMake build has `BUILD_GUI` and `BUILD_LIBRARIES_ONLY` switches | a sync path without a visible GUI can be built, but `src/CMakeLists.txt` still finds Qt modules beyond Core and Network | read |
| With the GUI off, the root build may skip the upstream test targets | the upstream test build and the service build are verified separately | read |
| `nextcloudcmd` (`src/cmd/cmd.cpp`) creates a `SyncEngine` directly | a reference for initialization order, not a service | read |
| `SyncJournalDb` is declared in `src/common/syncjournaldb.h`; its naming function takes the remote URL among its inputs | the journal path must be fixed independently of the active endpoint | read |
| `SyncEngine` checks `s_anySyncRunning` | one full sync cycle per process at a time | read |
| `Folder` and `FolderMan` (`src/gui/`) reference GUI code | their business logic is extracted and their interaction points replaced | read |
| The account URL and credential storage are coupled (`account.cpp`, `httpcredentials.cpp`) | calling `Account::setUrl()` is not an endpoint switch | read |
| `Account::setCapabilities` re-enters status/push setup even after `create(false)` | patch 0015 preserves headless lifetime before the adapter installs verified sync capability metadata | narrow regression failed before the fix; consuming builds pending |
| Files carry GPL-2.0-or-later and other file-level licenses | see [third-party.md](third-party.md) | read |

### Release audit: Desktop v35.0.2

The M0 checkout is commit `07761410a90a2f27007932fcb61afc23ddda5998` in
`third_party/nextcloud`. The following source findings supersede the corresponding preliminary
master-branch observations above; none is a runtime acceptance claim.

- `src/cmd/cmd.cpp:427` uses `QCoreApplication`; its `--confdir` path is applied through
  `ConfigFile::setConfDir`. Its journal name is derived from the URL at lines 686–697, so the stock
  CLI is suitable for isolated single-address M0 checks, not proof of endpoint-independent identity.

- `SyncEngine::finalize` emits `finished` before clearing `_propagator`, and schedules discovery
  destruction with `deleteLater`. An adapter must wait beyond the signal and track remaining jobs
  before claiming quiescence (INV-01, INV-05, INV-06).
- `aboutToPropagate` is followed directly by propagator construction and `start`, with no veto
  branch. Connecting an observer does not implement the product's deletion guard.
- `promptUserBeforePropagation` passes `bool cancel` to `cancelSyncOrContinue`: **true cancels**.
  The callback uses QPointer guards and deferred destruction; the adapter must consume an answer
  once itself rather than rely on a second call being rejected before deferred deletion runs.
- `handleMassDeletion` disables its prompt in command-line sync mode. M0 command-line runs must
  stay confined to disposable test data; SyncD must not inherit that bypass.
- `AbstractCredentials` exposes `createQNAM`, `fetchFromKeychain`, `persist`, `invalidateToken`
  and `forgetSensitiveData`. The stock HTTP implementation constructs keychain keys using the
  runtime URL and can migrate/delete older keys. SyncD needs its stable-reference adapter rather
  than using those persistence paths (INV-02, INV-03).
- `HttpCredentialsAccessManager::createRequest` adds Basic Authorization unless
  `DontAddCredentialsAttribute` is set; it does not itself check an approved origin. The product
  boundary must validate the destination before credential injection (INV-03).
- `AccessManager::createRequest` defaults to manual redirects, but accepts an already specified
  redirect policy. It also logs request URLs and response headers. The service must enforce its
  redirect policy and suppress/redact upstream network logging before authenticated tests.
- `AccessManager::createRequest` disables HTTP/2 by default (`accessmanager.cpp:106–110`).
  SyncD's origin-bound manager must preserve that stable HTTP/1.1 default rather than silently
  inheriting Qt's HTTP/2 default. Withholding encrypted downstream traffic on HTTP/2 can also
  withhold flow-control frames, so a blocked PUT is not proof of a lost success acknowledgement.
- `SyncJournalDb` accepts an explicit database path. This is the stable journal integration seam;
  cross-endpoint upload/download recovery has not yet been proven (INV-02, INV-08, INV-09).

### Modern cloud-file frameworks

Windows code lives in `src/libsync/vfs/cfapi` (`vfs_cfapi`, `hydrationjob`, `cfapiwrapper`) and its
`shellext` subtree. `CfApiWrapper` has register/unregister operations; `VfsCfApi` invokes them during
its lifecycle. These are real system side effects, so M0 must leave the plugin inactive.
Product-specific registration identities, hydration quiescence and complete teardown remain M6
work; the upstream implementation has not been run here.

The macOS extension target is under `shell_integration/MacOSX/NextcloudIntegration/FileProviderExt`.
Its stub names `NextcloudFileProviderKit.FileProviderExtension` as the actual implementation in
`shell_integration/MacOSX/NextcloudFileProviderKit`; the GUI-side bridge is under `src/gui/macOS`.
Reusing the target without auditing its networking/journal ownership is insufficient to establish
the planned single-writer XPC boundary. Signing, App Groups, GUI coupling and platform acceptance
remain unverified. These sources retain their own SPDX headers and bundled license texts.

## Nextcloud server APIs

| Point | Consequence | Checked |
|---|---|---|
| Login Flow v2 opens the system browser and polls for an app password | SyncD runs the login; the UI never receives the app password | read |
| WebDAV `oc:id` carries the instance namespace; `oc:fileid` is unique only within an instance | identity checks compare the full `oc:id`, never a bare `fileid` | read |

## Qt

| Point | Consequence | Checked |
|---|---|---|
| Qt 6 documents a Windows ARM64 configuration | a native path exists; the whole dependency chain is proven only on ARM64 hardware | read |

## MyGo

| Point | Consequence | Checked |
|---|---|---|
| MyGo's distribution documents `windows/arm64` | the UI can be native ARM64 | read; the Go module builds and tests on Windows 11 ARM64 |
| MyGo is a Go tool dependency (`go tool mygo …`) | its version moves with `go.mod` | built |
