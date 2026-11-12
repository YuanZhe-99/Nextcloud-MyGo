# Packaging and installation

Nextcloud-MyGo ships one per-user installer per architecture. Each carries the UI, the SyncD it was
verified with and only the DLLs they load. Until a trusted signing chain exists every package is an
unsigned **test build** with automatic updates off; the installer, the Apps entry and the shortcut
say so.

## Tooling

The installer is a small Go program, `packaging/windows/setup`, cross-compiled for the package's
architecture. MyGo's own NSIS installer was not used: NSIS writes an x86 `Uninstall.exe` into the
install folder, which an ARM64 package must not contain. An MSI built with WiX needs a .NET SDK and
WiX's license acceptance, and its custom actions would still need native code to stop SyncD. The Go
program needs nothing beyond the Go toolchain, runs natively on ARM64 and x64, talks to SyncD through
`internal/syncd`, and is checked by the same architecture checker as the rest of the package.

| Piece | Path | Role |
|---|---|---|
| Package script | `tools/package.ps1` | builds the UI, stages and checks a package, builds the installer |
| Checker and stager | `tools/pkg` | DLL closure, manifest, PE architecture check, a running SyncD's report |
| Installer / uninstaller | `packaging/windows/setup` | per-user install, upgrade and uninstall |
| Manifest | `internal/release` | what a package contains and which contracts it speaks |
| Install test | `tools/package-smoke.ps1` | real install, reinstall, upgrade and uninstall on this machine |

## Building a package

```powershell
./tools/bootstrap-deps.ps1 -Architecture ARM64   # prints .dev/deps/env-arm64.json
./tools/build-syncd.ps1 -Architecture ARM64 -QtPrefix <target Qt> -QtHostPrefix <host Qt>
./tools/package.ps1 -Arch ARM64        # or x64; SyncD from build/syncd-<Arch>
go run ./tools/pkg check -arch ARM64 build/package/ARM64/Nextcloud-MyGo-0.1.0-ARM64
./tools/package-smoke.ps1 -Setup build/package/Nextcloud-MyGo-0.1.0-arm64-test-setup.exe
```

For an upgrade test between two versions:

```powershell
./tools/package.ps1 -Arch ARM64 -TestVersion 0.1.1
./tools/package-smoke.ps1 -Setup build/package/Nextcloud-MyGo-0.1.0-arm64-test-setup.exe `
    -UpgradeTo build/package/Nextcloud-MyGo-0.1.1-testversion-arm64-test-setup.exe
```

`tools/package.ps1` reads the SyncD build's `CMakeCache.txt` for its upstream build and that build's
prefixes (Qt, native dependencies, vcpkg), so DLLs come only from the trees SyncD was linked against,
plus the newest Visual C++ runtime of the installed Visual Studio. Every step has a deadline. It writes
`build/package/<Arch>/Nextcloud-MyGo-<version>-<Arch>/` and
`build/package/Nextcloud-MyGo-<version>-<arch>-test-setup.exe`, and installs nothing. After staging it
runs the staged SyncD's `--version` and fails unless its version, Control schema and protocol match the
manifest, so a package never pairs a UI with a SyncD built from other sources.

### Test version

`-TestVersion X.Y.Z` builds a package of another version for upgrade tests, without editing
`mygo.json` or `internal/version`. It configures a SyncD tree of its own, `build/syncd-<Arch>-testversion`,
with the Qt prefix, host Qt and upstream build of `-SyncDBuild`'s CMake cache, `BUILD_TESTING=OFF` and
`SYNCD_TEST_VERSION=X.Y.Z` (which replaces `PROJECT_VERSION` as `SYNCD_VERSION`), and builds only SyncD and
the CFAPI plugin there; `-SyncDBuild` is never written. The UI is linked with
`MYGO_LDFLAGS=-X .../internal/version.Version=X.Y.Z` (`Version` is a variable for this), and the stager
gets `-test-version`, which sets the manifest's version and `testVersion: true`. The output is
`build/package/<Arch>-testversion/` and `Nextcloud-MyGo-X.Y.Z-testversion-<arch>-test-setup.exe`; its
installer says the version was stamped for a test. The UI executable's file properties keep
`mygo.json`'s version. Such a package is never a release.

### License texts

No package is made without the license texts of what it ships (GPL obligations, PLAN.md §16.5).
`tools/package.ps1` runs `tools/licenses.ps1` for the SyncD build ([CI](ci.md#license-bundle)) and passes
the result to the stager as `-notices`. `pkg stage` refuses before writing anything when `-notices` is
missing or lacks one of `LICENSE.txt`, `THIRD_PARTY_NOTICES.md`, `SOURCE.txt`, Nextcloud Desktop's
`COPYING`, the texts of the Qt modules qtbase, qt5compat, qtsvg and qtwebsockets, of OpenSSL, SQLite, zlib,
QtKeychain, KArchive and libp11, and the Go license. The native texts exist only in prefixes built by
`tools/bootstrap-deps.ps1`, so packages are built from those; the hand-assembled `.dev` trees are not a
source for packages, and `tools/licenses.ps1` says to rebuild with the bootstrap.

The installed copy keeps the bundle in its version directory: `LICENSE.txt` (GPL-3.0),
`THIRD_PARTY_NOTICES.md`, `SOURCE.txt` (where the corresponding source is) and `licenses\<component>\`.
These files are listed and hashed in `manifest.json` like every other file, so the installer verifies them,
and an upgrade or uninstall replaces or removes them with the directory. The release's
`Nextcloud-MyGo-<version>-licenses.zip` holds the same bundle per architecture.

The stager starts from the two executables, the Qt plugins `tls/qopensslbackend.dll` and
`sqldrivers/qsqlite.dll` and `uninstall.exe`, follows every normal and delay-load import, copies each
DLL it finds in the search directories and stops at an import that is neither found there nor part of
Windows. `libssl-3-<arch>.dll` and `libcrypto-3-<arch>.dll` are added by name (`-runtime`): Qt's
OpenSSL backend opens them at run time instead of importing them. The Visual C++ runtime is always
shipped, never taken from an installed redistributable. SyncD imports upstream's CFAPI plugin
(`nextcloudsync_vfs_cfapi.dll`), so the closure stages it beside SyncD, where upstream loads it by
name; its shell extension is not built and no Explorer-loaded DLL is shipped.
It writes `qt.conf` so that Qt loads plugins only from the version directory's `plugins`.

## Package layout

```text
%LOCALAPPDATA%\Programs\Nextcloud-MyGo\
  install.json                 {"current": "versions/0.1.0-1a2b3c4d"}
  uninstall.exe                copy of the current version's uninstaller
  versions\0.1.0-1a2b3c4d\
    Nextcloud-MyGo.exe         UI
    nextcloud-mygo-syncd.exe   SyncD
    Qt6*.dll, nextcloudsync.dll, qt6keychain.dll, libssl-3-*.dll, sqlite3.dll, MSVCP140*.dll, ...
    plugins\tls\qopensslbackend.dll
    plugins\sqldrivers\qsqlite.dll
    qt.conf, sync-exclude.lst, manifest.json, LICENSE.txt, THIRD_PARTY_NOTICES.md, SOURCE.txt, licenses\
```

User data stays where the processes keep it, outside the install folder: SyncD's profile under
`%LOCALAPPDATA%\Nextcloud-MyGo\syncd\<profile hash>` and the interface settings under
`%APPDATA%\Nextcloud-MyGo`.

## Manifest

`manifest.json` records the product, version and architecture; `testBuild: true`, `signed: false` and
`autoUpdate: false`; `testVersion: true` only in a `-TestVersion` package; each half's executable and protocol version (from `protocol/contract.json`);
SyncD's Control schema (read from the one `PRAGMA user_version=` assignment in `backend/control.cpp`;
staging fails if there is not exactly one); and every file's size and SHA-256. The installer verifies
the extracted files against it before the new version can become current.

## Architecture check

`go run ./tools/pkg check -arch ARM64|x64 <dir or file>...` needs no server and no Windows APIs. It
reads every file that starts with `MZ`, whatever its name, and fails on:

| Finding | Rule |
|---|---|
| x64 or x86 image in an ARM64 package, ARM64 or x86 in an x64 package | fail |
| ARM64EC image (AMD64 machine with CHPE metadata) | fail |
| ARM64X image (ARM64 machine with CHPE metadata) | fail, except the Visual C++ runtime |
| import that neither the package nor Windows provides | fail |
| Visual C++ runtime import satisfied only by an installed redistributable | fail |

Microsoft ships the ARM64 Visual C++ runtime only as ARM64X. An ARM64 process maps its ARM64 code and
never its ARM64EC code, so it runs natively; the checker prints each such file as a note. Every
other hybrid image fails. The user accepted this exception on 2026-11-12. Its tests build synthetic PE files, so CI runs them anywhere.

## Install and upgrade

The installer refuses before writing anything when:

- Windows runs on another architecture than the package (an x64 package on ARM64 would run
  emulated);
- the installed version is newer, or its Control schema is newer (PLAN.md §16.3);
- the installed package is for the other architecture: uninstall first; files and settings stay.

Otherwise it installs, reinstalls or upgrades in this order:

1. Extract into a new `versions\<version>-<random>.partial`, verify it against the manifest, then
   rename it. A failure removes only the partial directory.
2. Close the interface (a stateless client) so that it cannot start SyncD again; SyncD's pipe serves
   one client at a time, so this also frees it for setup.
3. If the installed version's SyncD runs for this profile, connect to it without ever starting one
   (`syncd.ConnectRemote`) and ask for the update safe point, `daemon.prepareUpdate(<version being
   installed>)`. A deletion waiting for review (`DECISION_PENDING`) stops the install at once:
   installing never decides it. A switch, renewal, address check or sign-in still running, the
   running cycle not yet finished (`QUIESCE_STALLED`) or no answer yet is retried every second for up
   to 2 minutes; then, like any other refusal, the install stops. A SyncD that exits meanwhile needs
   nothing. A stop removes the new directory, starts the closed interface again and leaves SyncD
   running; setup says why (below).
4. Send `daemon.shutdown` to SyncD and wait up to 30 seconds; a SyncD that does not stop is
   terminated, which it recovers from like any crash. If a process survives, the new directory is
   removed and the installed version stays current.
5. Copy `uninstall.exe` to the root, point `install.json`, the Start menu shortcut and the Apps entry
   at the new directory, and move startup entries that start this install to the new UI, keeping
   their arguments and the user's Task Manager on/off choice.
6. Remove the other version directories, then start the new UI, which starts its own SyncD.

What setup prints when it stops at step 3, each followed by "the installed version was kept":

| Case | Message |
|---|---|
| deletion waiting | Nextcloud MyGo is waiting for you to review a deletion. Open Nextcloud MyGo, review it, then run setup again |
| still busy after 2 minutes | Nextcloud MyGo is still busy (signing in, checking or switching a server address, or finishing a sync) and did not get ready within 2 minutes. Wait a few minutes, then run setup again |
| no answer after 2 minutes | Nextcloud MyGo's background sync is running but did not answer within 2 minutes. Quit Nextcloud MyGo from its tray icon, then run setup again |
| any other refusal | Nextcloud MyGo could not get ready for the update. Quit Nextcloud MyGo from its tray icon, then run setup again |

Only the profile's SyncD running from the installed version directory is prepared; another SyncD
process under the install root (another profile's) is only stopped as in step 4. The uninstall
stops SyncD as in steps 2 and 4, without the safe point.

A new SyncD never shares a directory with another version's Qt DLLs, and the previous version is
removed only after the new one is complete and current. The same rules make a same-version
reinstall a full switch to a fresh directory.

## Uninstall

`uninstall.exe -uninstall` (the Apps entry's command) closes the interface, stops SyncD as above,
removes this install's startup entries, the shortcut, the Apps entry and the install folder, and
keeps accounts, settings and synced files. `-remove-user-data` additionally starts SyncD and removes
each account through `accounts.remove`, which deletes its saved credential and revokes its app
password on the server, then deletes the profile's SyncD data and interface settings. An unconfirmed
revocation is reported and does not block the uninstall; an account SyncD cannot remove keeps the
user data in place and is reported. Neither option touches synced folders or server content.
An on-demand folder keeps its Cloud Files registration through an uninstall, so a reinstall serves
it again. `-remove-user-data` cannot remove an account that still has folders, so it keeps that data and
never orphans a registration; removing the folders first at uninstall is an open item ([on-demand](on-demand.md)).

The installer and uninstaller act on the default profile; `NEXTCLOUD_MYGO_PROFILE` selects a
development profile, as it does for the interface.

## Coexistence with the official client

The install folder, Apps entry (key `Nextcloud-MyGo`), shortcut, configuration directories and pipe
names are Nextcloud-MyGo's own. The Start menu shortcut and the Apps entry show the display name
**Nextcloud MyGo**; folders, keys and executables keep `Nextcloud-MyGo`. The installer registers no URL scheme (`nc://`) or file association, and
reads or changes nothing of the official client. Startup entries are matched by the executable they
start, so other applications' entries stay untouched.

## Verified

ARM64 (Windows 11 on ARM64 hardware), 2026-11-12:

- both packages built (`tools/package.ps1`, 15 s and 16 s) and checked: ARM64, 28 PE files,
  native ARM64, with the Visual C++ runtime as ARM64X; x64, 29 PE files, native x64;
  cross-checking either package against the other architecture fails;
- `tools/package-smoke.ps1` on the ARM64 installer (25 s): install and launch; the UI and the
  SyncD it started run from the version directory as machine `0xAA64` (`GetProcessInformation`), and
  every module SyncD loaded came from the version directory or Windows, with no packaged DLL taken
  from an installed copy; reinstall over the running
  version moved to a fresh directory, stopped SyncD through `daemon.shutdown` without termination,
  removed the old directory and moved the startup entry; uninstall left no process, startup entry,
  shortcut, Apps entry or program file and kept the profile data; a second install and
  `-remove-user-data` removed the profile data and settings; the official client's `nc` registration,
  configuration folder and Apps entries were unchanged;
- the x64 installer refuses to run on ARM64;
- the update safe point and an upgrade between two versions (`tools/package-smoke.ps1 -UpgradeTo`,
  36 s): the 0.1.0 package (SyncD built before this change) and a `-TestVersion 0.1.1` package (16 s
  with its SyncD tree already built; a first build of that tree took 56 s) built from the committed
  sources plus this change. The same-version reinstall (3.7 s) and the upgrade over the running copy
  (3.8 s) each printed `SyncD is ready` before `daemon.shutdown`; the upgrade wrote a new
  `control.pre-update.*.sqlite`; the new SyncD reported version 0.1.1 and `startupReport`
  `previousVersion` 0.1.0, schema 11 to 11, self-check `passed`; the pause of every folder set before
  the upgrade was kept; uninstall then left nothing. Before the interface was closed first, a reinstall
  stopped after 2 minutes with the no-answer message, removed its new directory and kept the
  installed version: SyncD's pipe serves one client at a time.

Not verified: installing the x64 package (needs x64 hardware); which TLS backend SyncD loads at its
first network request (no server in this run); `-remove-user-data` with a signed-in account; a
failed install on real hardware (covered by unit tests only); the deletion-waiting and still-busy
refusals of the safe point on a real install (unit tests only, `TestPrepare`); an upgrade with a
signed-in account or a Control schema change.

## What SyncD needs to provide

SyncD now provides all four. The installer uses `daemon.prepareUpdate` and `ConnectRemote`
(step 3) and `tools/package.ps1` checks `--version` against the manifest:

- `daemon.prepareUpdate(targetVersion)` ([IPC](ipc.md)): the safe point of PLAN.md §16.2 (no new
  cycle, the running one finished, a consistent Control copy `control.pre-update.<id>.sqlite`), sent
  in step 3 before `daemon.shutdown`; after the new SyncD starts,
  `snapshot.daemon.startupReport` gives the previous version and schema, the new schema and the
  self-check result with its reason codes (`go run ./tools/pkg report` prints it).
- `internal/syncd.ConnectRemote(ctx, profile, executable)`: a client that never starts SyncD.
- `nextcloud-mygo-syncd.exe --version` prints one JSON line, `{"version", "protocol": {"major",
  "minor"}, "controlSchema", "engine"}`, without a profile. The manifest's schema is still read from
  source; packaging fails when the two differ.
- With a `qt.conf` beside the executable (the installed layout) SyncD calls
  `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)`
  and `SetDllDirectory("")` before loading anything and removes `QT_PLUGIN_PATH`, so DLLs and Qt
  plugins come only from the version directory and Windows; a development build has no `qt.conf`
  and keeps its PATH. Not yet verified in an installed package.
