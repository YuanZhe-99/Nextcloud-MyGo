# Continuous integration and releases

GitHub Actions in the public repository build and test Nextcloud-MyGo natively on Windows x64 and
ARM64 for every push and pull request, and publish a `v*` tag as a pre-release with both
installers. Only `GITHUB_TOKEN` is used; no workflow signs anything.

| Workflow | Runs on | Does |
|---|---|---|
| `.github/workflows/ci.yml` | every push to a branch, every pull request, by hand | calls `build.yml` |
| `.github/workflows/build.yml` | called by the two others | the Go checks, the dependency bootstrap, the upstream engine and SyncD builds and the fast tier on `windows-latest` (x64) and `windows-11-arm` (ARM64); with `package`, the installers and license bundles |
| `.github/workflows/release.yml` | a pushed `v*` tag | checks the tag, calls `build.yml` with `package`, publishes the GitHub release |

## Runners

`windows-latest` (Windows Server 2025, x64) and `windows-11-arm` (Windows 11, ARM64) are free for
public repositories and both carry Visual Studio 2026 with the C++ CMake tools, which the build
scripts require (`Visual Studio 18 2026` generator); the first step fails clearly on an image
without it and prints the Visual Studio and MSVC versions it found. Each architecture builds and
runs on its own hardware: no job cross-compiles or emulates the other architecture's product, so
CI is native x64 evidence too, which this ARM64 development machine cannot give
([platform notes](platform-notes.md)). On ARM64 the x64 host Qt's tools (moc, rcc, lrelease) run
under the runner's x64 emulation, exactly as in the local builds; nothing they produce ships.

## Steps of `build.yml`

1. Go: `gofmt`, `go vet`, `go test ./...` with the default cgo setting and with `CGO_ENABLED=0`,
   `go tool mygo vet .`, `go tool mygo i18n check`, `go run ./tools/checkdocs` (AGENTS.md).
2. `tools/bootstrap-deps.ps1` (below) into `.dev/deps`.
3. `tools/build-upstream.ps1 -Mode engine`: `libsync`, `csync` and the CFAPI plugin that SyncD links,
   from the submodule plus the recorded patches. Upstream's own test suite (`-Mode tests`, about
   eight minutes of tests after a long build, Inkscape for its GUI resources) stays a local tier.
4. `tools/build-syncd.ps1`, which also runs SyncD's CTest targets.
5. `tools/test-all.ps1 -Tier fast` (the gofmt and vet step on ARM64, the real Go-to-C++ pipe tests,
   all CTest targets) with its two-minute budget.
6. With `package`: `tools/package.ps1`, `go run ./tools/pkg check` on the staged directory,
   and an upload of the installer and of the license bundle `tools/package.ps1` staged into it.

The **server tier does not run in CI**: it needs the disposable Linux Nextcloud container of
`tools/test-server.ps1`, and GitHub's Windows runners cannot run Linux containers. It stays a
local tier ([development](development.md#test-tiers)), run before a release on both architectures
and recorded in [acceptance](acceptance.md), as are the upstream tests, the install smoke test
(`tools/package-smoke.ps1` installs into the user's profile) and the shared-server fixture.

## Dependency bootstrap

`tools/bootstrap-deps.ps1 -Architecture ARM64|x64 [-Prefix <dir>]` assembles every native
dependency at exactly what `build-lock.json` records under `preparedDependencies`, into
`<Prefix>` (default `.dev/deps`), and writes `<Prefix>/env-<arch>.json` with the arguments of
`tools/build-upstream.ps1` and `tools/build-syncd.ps1` (`QtPrefix`, `QtHostPrefix`,
`DependencyPrefix`) and `PkgConfig`, the pkg-config that upstream's CMake finds libp11 with
(set it as `PKG_CONFIG`). It installs nothing outside `<Prefix>`.

| Component | Source | How |
|---|---|---|
| Qt 6.12.0 | `download.qt.io` online repository, `msvc2022_64` and `msvc2022_arm64_cross_compiled` | the base package and Qt 5 Compat, WebSockets and Shader Tools, without documentation and translations; SHA-1 checked; extracted unmodified to `Qt/6.12.0/msvc2022_64` (also the host Qt) and `msvc2022_arm64` |
| OpenSSL, SQLite (`json1`), zlib | vcpkg at the recorded commit and tool version, classic mode, triplet `<arch>-windows` | the installed versions and port versions must equal the record |
| pkg-config | the msys2 pkgconf package for the host (`clangarm64` or `mingw64`) that vcpkg's recorded commit pins | URL and SHA-512 read from vcpkg's own script, downloaded, checked and unpacked into `tools/pkgconf-<host>` (vcpkg fetches it itself only when it builds a port, not when its binary cache serves one) |
| ECM, QtKeychain, KDSingleApplication, KArchive | the projects' release tags, shallow | the tag must resolve to the recorded commit; CMake with the options of the original builds, Release, installed into `native/<arch>` |
| libp11 | release tag `libp11-0.4.21`, same commit check | its nmake makefile, `libp11.dll` only, against vcpkg's OpenSSL; headers and `libp11.pc` copied into `native/<arch>` |
| License texts | each shipped Qt module's `LICENSES/` at tag `v6.12.0`, vcpkg's `copyright` files, the sources' license files | copied into `native/<arch>/share/licenses` for `tools/licenses.ps1` |

A mismatch is a failure, never a fallback to another version. Every DLL and EXE in the installed
prefixes must have the target's PE machine. A finished architecture is skipped while its stamp
(`native/<arch>/bootstrap.json`: the record and the script's hash) still matches; Qt is extracted
again only when the list of archives changes. One prefix holds both architectures; run them one
after the other, because they share the vcpkg checkout.

### Differences from the hand-assembled `.dev`

- QtKeychain, KDSingleApplication and KArchive used to be built from one source checkout for both
  architectures; the bootstrap clones each per architecture. Same commits and options.
- ECM was installed once and copied into the x64 prefix; the bootstrap installs it per architecture.
- The Qt download is the same procedure as the original `get-qt.ps1` (aqtinstall 3.3.0 could not
  read the new repository layout).
- pkg-config used to be the copy vcpkg had unpacked under its `downloads/tools/msys2`; the bootstrap
  unpacks the same pinned package (pkgconf 3.0.7) itself.
- `native/<arch>/share/licenses` and `bootstrap.json` are new.

## Caches

Three caches per architecture, each saved as soon as its step has succeeded, so a later failure does
not throw away a finished build:

| Cache | Holds | Key |
|---|---|---|
| `qt-…` | the Qt archives (`.dev/deps/downloads`), about 0.25 GB for x64 and 0.5 GB for ARM64 (target and host) | Qt version, bootstrap script |
| `deps-…` | `native/<arch>`, vcpkg's installed tree and `tools/` (pkgconf) | bootstrap script, MSVC version, `preparedDependencies` |
| `upstream-…` | the patched source `build/src-*` and the engine build `build/?e-*`, without object files and PDBs | the `deps` key, the submodule commit, the recorded upstream commit, the patch hashes, `tools/build-upstream.ps1` |

Changing a patch, the submodule, a dependency in `build-lock.json`, either script or the runner's
MSVC version therefore rebuilds what depends on it and nothing else. Qt is extracted from the cached
archives on every run (its extracted trees are 2.5 GB each). A tag run can use the caches of the
default branch.

## Times

| Run | x64 (`windows-latest`) | ARM64 (`windows-11-arm`) |
|---|---|---|
| cold (no cache) | about 50 min expected | about 50 min expected |
| warm (all caches hit, SyncD changed) | about 12 min expected | about 12 min expected |

These are expectations, not measurements: they scale the local times below to the runners' four
cores. The first GitHub runs replace them with measured times. A cold run is mostly OpenSSL's vcpkg
build and the upstream engine; a warm run is the Go checks, extracting Qt, the SyncD build and the
fast tier.

Measured on 2026-11-12 on the ARM64 development machine (8 cores, shared with other builds), into the
fresh prefix `.dev/ci-verify` and the build root `build/cv`, with SyncD and the tests from a `git archive`
of the committed sources plus these scripts:

| Step | ARM64 (native) | x64 (built on ARM64, tests emulated) |
|---|---|---|
| Qt download and extraction (both SDKs for ARM64) | 91 s | 57 s (host SDK only) |
| vcpkg ports | 15 s (served by vcpkg's binary cache, ABI inputs identical) | 875 s (cold, `VCPKG_BINARY_SOURCES=clear`) |
| ECM, QtKeychain, KDSingleApplication, KArchive, libp11, pkgconf, license texts | about 160 s | about 220 s |
| bootstrap again, record unchanged | 1.0 s (Qt already extracted; CI first extracts it from the cached archives) | 1.0 s |
| upstream engine (`-Mode engine`) | 904 s | 843 s |
| SyncD build and its CTest (22 of 22) | 155 s | 156 s |
| fast tier | 42.1 s of 120 s (36.8 s again after the final bootstrap) | 38.8 s of 120 s (36.7 s) |
| `tools/package.ps1` and `tools/pkg check` | 11 s, 29 PE files native ARM64 (the Visual C++ runtime as ARM64X) | 13 s, 30 PE files native x64 |

Reproduction against the hand-assembled `.dev`: the 16 Qt archives are byte-identical; each
architecture's native prefix has the same 247 files, 241 of them byte-identical, the four DLLs
differing only in 4 to 14 bytes (PE time stamp and debug identifier) and `KF6Archive.pc` and
`ECMConfig.cmake` only in the prefix path; vcpkg's ABI inputs of all six port builds are identical,
and the ARM64 port DLLs byte-identical. The uncommitted backend work in the tree at the time was not
part of this run. Not run locally: the workflows themselves, on GitHub's runners.

## Release

Pushing a tag `vX.Y.Z` (after the commit, AGENTS.md) runs `release.yml`:

1. The tag must equal `Version` in `internal/version/version.go` and `version` in `mygo.json`.
2. `build.yml` with `package` on both architectures: everything above, then the installer, the
   architecture check of every PE file in the staged package (the ARM64 Visual C++ runtime is the
   accepted ARM64X exception, [packaging](packaging.md#architecture-check)), and the license bundle.
3. A GitHub release marked **pre-release**, titled as an unsigned test build, with
   `Nextcloud-MyGo-X.Y.Z-arm64-test-setup.exe`, `Nextcloud-MyGo-X.Y.Z-x64-test-setup.exe`,
   `Nextcloud-MyGo-X.Y.Z-licenses.zip` and `SHA256SUMS` over the three. The notes say that the
   installers are unsigned, that automatic updates are off and that the server tier ran outside CI.

The installers are never signed in CI: signing waits for a trusted signing chain (PLAN.md §16.2),
whose keys will live only in GitHub secrets and the maintainer's offline copy.

### License bundle

`tools/licenses.ps1 -SyncDBuild <tree> -Arch <arch> -Out <dir>` writes, from what that build actually
linked:

- `LICENSE.txt` (GPL-3.0, the product) and `THIRD_PARTY_NOTICES.md`;
- `SOURCE.txt`: the repository URL, tag and commit, the Nextcloud Desktop tag and commit of the
  submodule, that the patches in `patches/nextcloud/` apply in `build-lock.json`'s order, and every
  native dependency's version and commit;
- `licenses/Nextcloud-Desktop/`: `COPYING` and the snapshot's file-level `LICENSES/`;
- `licenses/Qt/<module>/`, `OpenSSL`, `SQLite`, `zlib`, `QtKeychain`, `KArchive`, `libp11`: from the
  dependency prefixes (it fails on a prefix the bootstrap did not make);
- `licenses/Go/`: the Go toolchain's license and the license files of every module linked into the
  UI and the installer (it fails on a module without one).

`tools/package.ps1` stages this bundle into every package, and `pkg stage` refuses an incomplete one
([packaging](packaging.md#license-texts)). The release zip holds one such directory per architecture, the
same files the installers carry.
