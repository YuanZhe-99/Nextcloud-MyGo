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
| Checker and stager | `tools/pkg` | DLL closure, manifest, PE architecture check |
| Installer / uninstaller | `packaging/windows/setup` | per-user install, upgrade and uninstall |
| Manifest | `internal/release` | what a package contains and which contracts it speaks |
| Install test | `tools/package-smoke.ps1` | real install, reinstall and uninstall on this machine |

## Building a package

```powershell
./tools/build-syncd.ps1 -Architecture ARM64 -QtPrefix <target Qt> -QtHostPrefix <host Qt>
./tools/package.ps1 -Arch ARM64        # or x64; SyncD from build/syncd-<Arch>
go run ./tools/pkg check -arch ARM64 build/package/ARM64/Nextcloud-MyGo-0.1.0-ARM64
./tools/package-smoke.ps1 -Setup build/package/Nextcloud-MyGo-0.1.0-arm64-test-setup.exe
```

`tools/package.ps1` reads the SyncD build's `CMakeCache.txt` for its upstream build and that build's
prefixes (Qt, native dependencies, vcpkg), so DLLs come only from the trees SyncD was linked against,
plus the newest Visual C++ runtime of the installed Visual Studio. Every step has a deadline. It writes
`build/package/<Arch>/Nextcloud-MyGo-<version>-<Arch>/` and
`build/package/Nextcloud-MyGo-<version>-<arch>-test-setup.exe`, and installs nothing.

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
    qt.conf, sync-exclude.lst, manifest.json, LICENSE.txt, licenses\
```

User data stays where the processes keep it, outside the install folder: SyncD's profile under
`%LOCALAPPDATA%\Nextcloud-MyGo\syncd\<profile hash>` and the interface settings under
`%APPDATA%\Nextcloud-MyGo`.

## Manifest

`manifest.json` records the product, version and architecture; `testBuild: true`, `signed: false` and
`autoUpdate: false`; each half's executable and protocol version (from `protocol/contract.json`);
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
2. Close the interface (a stateless client) so that it cannot start SyncD again, then send
   `daemon.shutdown` to SyncD and wait up to 30 seconds; a SyncD that does not stop is terminated,
   which it recovers from like any crash. If a process survives, the new directory is removed and the
   installed version stays current.
3. Copy `uninstall.exe` to the root, point `install.json`, the Start menu shortcut and the Apps entry
   at the new directory, and move startup entries that start this install to the new UI, keeping
   their arguments and the user's Task Manager on/off choice.
4. Remove the other version directories, then start the new UI, which starts its own SyncD.

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

The install folder, Apps entry (`Nextcloud-MyGo`), shortcut, configuration directories and pipe names
are Nextcloud-MyGo's own. The installer registers no URL scheme (`nc://`) or file association, and
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
- the x64 installer refuses to run on ARM64.

Not verified: installing the x64 package (needs x64 hardware); which TLS backend SyncD loads at its
first network request (no server in this run); `-remove-user-data` with a signed-in account; a
failed install on real hardware (covered by unit tests only); an upgrade between two different
versions.

## What SyncD needs to provide

SyncD now provides the first four; the installer and stager have not adopted them yet, and the
license bundle is still open:

- `daemon.prepareUpdate(targetVersion)` ([IPC](ipc.md)): the safe point of PLAN.md §16.2 (no new
  cycle, the running one finished, a consistent Control copy `control.pre-update.<id>.sqlite`), to
  replace `daemon.shutdown` in step 2 followed by `daemon.shutdown`; after the new SyncD starts,
  `snapshot.daemon.startupReport` gives the previous version and schema, the new schema and the
  self-check result with its reason codes.
- `internal/syncd.ConnectRemote(ctx, profile, executable)`: a client that never starts SyncD.
- `nextcloud-mygo-syncd.exe --version` prints one JSON line, `{"version", "protocol": {"major",
  "minor"}, "controlSchema", "engine"}`, without a profile, so staging can stop reading the schema
  from source. A startup check of the manifest against it is the stager's part.
- With a `qt.conf` beside the executable (the installed layout) SyncD calls
  `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)`
  and `SetDllDirectory("")` before loading anything and removes `QT_PLUGIN_PATH`, so DLLs and Qt
  plugins come only from the version directory and Windows; a development build has no `qt.conf`
  and keeps its PATH. Not yet verified in an installed package.
- The license bundle: Qt, QtKeychain, OpenSSL, SQLite, zlib, KArchive and libp11 texts are not yet
  shipped next to `LICENSE.txt` and the Nextcloud Desktop `COPYING`.
