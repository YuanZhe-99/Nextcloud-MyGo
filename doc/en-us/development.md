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
The paired-daemon fixture also creates 16 MiB random owned upload/download objects within the approved
quota. It kills only its own child after diagnostics observe an unfinished content reply and positive
content bytes, then checks recovered hashes and native journal-file identities after restart. A run
that never observes an active transfer fails rather than claiming crash acceptance from UI status.
All objects remain under the fixture's reserved collections and are removed by its existing cleanup.
The extension also observes positive-byte unfinished 16 MiB transfers, explicitly pauses/cancels them,
switches the whole paused account to the other approved origin and reconciles after resume. It checks
recovered hashes and native journal identities in both directions. This is cancellation plus manual
cross-origin recovery, not proof of automatic failure selection during a mapped active transfer.
The separate `-Scenario network-active` acceptance mode uses an owned local CONNECT relay to bound
encrypted throughput and break a real unfinished transfer. TLS still terminates at the approved real
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

Reused build sources are checked in a temporary Git index: reverse the recorded patches in reverse
order, then compare the result to the recorded upstream commit. This handles overlapping patches
without modifying the source working tree or its real index.

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
