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
| `PropagateRemoteMove::slotMoveJobFinished` (`propagateremotemove.cpp:211`) renames the local file back to its old name and records the old name in the journal when the MOVE fails for any reason, a transport failure included | a local rename made while an address is failing was undone: both sides kept the old name, no content was lost, the user's rename was. Patch 0019 keeps the rename and the journal's old record when the MOVE got no answer (refused or lost connection, timeout, aborted cycle) or a 502, 503 or 504; the next discovery pairs the new name with the old record by its local file ID (inode) and sends the MOVE again, or adopts the server's copy when the lost MOVE was made. An answer about the move itself (a 4xx such as 403 or a forbidden target, 500, 507) is still reverted, as upstream's own tests require ([engine](engine.md#completion-and-safety)) | seen three times in the accelerated soak, ARM64, 35.0.2, 2026-11-13; patch 0019 with `testMoveWithoutServerRefusalKeepsLocalRename` and the real `rename-cut` case |

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
its lifecycle. SyncD reuses the plugin for on-demand folders ([on-demand](on-demand.md)); these
points of Desktop v35.0.3 shaped that integration (read, and exercised by the on-demand runs):

- The sync root ID is `provider!SID!account display name!alias`, and the display name is
  `user@host[:port]`: an endpoint switch would register the same folder under a second key and
  leave the first. Patch 0017 adds `VfsSetupParams::syncRootAccountId`.
- `createSyncRootRegistryKeys` always names the shell extension's handler classes and a
  navigation-pane class. Built without the extension, those IDs are unregistered here or belong to
  an installed official client. Patch 0017 writes them only for a registered extension.
- `unregisterFolder` deletes the shell-extension class keys under HKCU when no root of the provider
  remains; the class IDs are upstream's own, so it can remove another installation's. SyncD calls only
  `CfApiWrapper::unregisterSyncRoot`.
- Hydration and population callbacks run on Cloud Files threads, wait in a local event loop for
  queued work on the VFS thread, and read `params().account`. Disconnecting or destroying the VFS while
  one runs can strand that thread. Patch 0017 adds `Vfs::refuseNewRequests` and
  `Vfs::hasRequestsInFlight`, counted by the callbacks themselves.
- New remote directories become on-demand directories (`postProcessServerNew`); listing one asks the
  provider to populate it through `Account::listRemoteFolder`, outside a sync cycle, and writes the
  journal on the VFS thread. A request from the provider's own process is refused.
- The plugin and its shell extension are gated on `BUILD_SHELL_INTEGRATION` (patch 0001); patch 0017
  adds `BUILD_CFAPI_VFS` for the plugin alone.
- `Folder::setVirtualFilesEnabled` deletes and recreates the journal; switching a folder's mode is
  therefore not offered.

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
| A MOVE keeps the entry's `oc:fileid`; a chunk session's final MOVE answers only after the file is assembled; a committed write stays committed when its reply is lost | upstream discovery recognizes the renamed entry by file ID and the assembled file by checksum, so a lost reply is recovered by rediscovery, never by a second request | real runs against server 35.0.2 |
| A PUT that replaces an existing file is written over it in place, with no part file (`File::put` drops the part file when the part path is not creatable but the target is updatable, which `Common::isCreatable` makes true for every file); a body cut short leaves the file truncated, answered `400 Expected filesize` once the server notices, and the file cache keeps the old size and ETag until something reads the file. A PUT of a new file uses a part file and leaves nothing | patch 0018 sends every nonempty modification as a chunked upload, whose final MOVE assembles on the server ([engine](engine.md#completion-and-safety)) | a short PUT by hand against server 35.0.2: an existing 1 MiB file was left at 294,912 bytes, a new file was not created |
| A `files_external` storage whose backing directory is gone is left out of its parent's listing for the client's properties, or listed with permissions `MG`, size 0 and an empty ETag, and its own listing answers 207 with no members | SyncD holds a cycle that would remove a mount point or all of its content locally (`DISCOVERY_INCOMPLETE`) | real runs against server 35.0.2 (`partial-listing`) and a PROPFIND by hand |

## Qt

| Point | Consequence | Checked |
|---|---|---|
| Qt 6 documents a Windows ARM64 configuration | a native path exists; the whole dependency chain is proven only on ARM64 hardware | read |
| An HTTP proxy that closes the connection after `CONNECT` without answering: a GET fails at once (`ProxyConnectionClosedError`), but a request with a body (a PROPFIND) waits for its transfer timeout, and so does every later request of the same manager, even once the proxy answers again; a `503` answer fails both at once (`ConnectionRefusedError`) | the fixture relay answers `503` for a faulted origin ([development](development.md#test-tiers)); a real proxy that just closes would hold a cycle until upstream's timeout, then the next cycle's fresh manager ([engine](engine.md#completion-and-safety)) recovers | Qt 6 of `build-lock.json`, ARM64, 2026-11-13, soak probe |

## MyGo

| Point | Consequence | Checked |
|---|---|---|
| MyGo's distribution documents `windows/arm64` | the UI can be native ARM64 | read; the Go module builds and tests on Windows 11 ARM64 |
| MyGo is a Go tool dependency (`go tool mygo …`) | its version moves with `go.mod` | built |
