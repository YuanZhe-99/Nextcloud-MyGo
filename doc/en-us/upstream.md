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
| Files carry GPL-2.0-or-later and other file-level licenses | see [third-party.md](third-party.md) | read |

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
