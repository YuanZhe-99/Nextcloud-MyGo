# Version history

## 0.1.0

The first release: a test build for Windows 11, one per-user installer per architecture (x64 and
native ARM64).

- **One account, several addresses.** Each account can have several trusted addresses of the same
  server (internet, home network, VPN). An address serves the account only after its server, its
  TLS certificate and its folder namespace are verified. SyncD picks the address automatically, or
  keeps the one you fix, and switches only between sync cycles, all folders of the account together.
  A sign-in or certificate failure blocks an address; no score overrides it.
- **The Nextcloud Desktop engine.** Discovery, reconcile, propagation and the journal are upstream
  Nextcloud Desktop's, with 19 small patches, in SyncD, a background process of the current user.
  The interface (Go and MyGo) talks to it over a local Named Pipe, protocol 1.10; closing the
  window never stops sync.
- **File safety.** Deletion protection holds large or unusual deletions for your review. An
  unconfirmed upload, move or deletion is recovered by rediscovery; an unreachable server or an
  unreadable folder never reads as empty. A modification cut mid-transfer leaves the server's
  previous version in place (it is sent as a chunked upload). A local rename whose move fails on
  the network stays renamed and is sent again.
- **Several accounts**, each with several folders; signing in through the browser; renewal and
  revocation of the app password; a full server or a slow one each reported as such.
- **The interface:** Overview, Accounts & folders, Network, Activity with the deletion review,
  Settings, the tray and its flyout; English and Simplified Chinese.
- **Installation and updates:** per-user installers that upgrade in place and check that every
  binary matches the architecture. In-app updates install only an archive signed with the
  project's update key; they are off until turned on in Settings.

Known limitations:

- On-demand files are built but not offered (`virtualFiles`).
- The installers are not Authenticode-signed, so Windows SmartScreen warns on the first install.
- Tested on one native ARM64 machine. x64 binaries passed their tests under emulation there; the x64
  installer has not run on x64 hardware.
- Not shown: a real sleep and resume (simulated only), a 24-hour run (replaced by a 1-hour
  accelerated soak), strict VPN egress, a second Windows session or user, end-to-end encrypted
  folders, drive-letter reuse by another volume, and a journal restored by hand. Start at sign-in
  is checked after the release.
- Deletion protection counts each sync cycle on its own: deletions spread over several cycles are
  not added up.
- A safety block (a changed certificate, a server that is not the expected one) stays until SyncD
  restarts.
- On the test machine, Windows Credential Manager lost updates when several processes wrote to it
  at the same moment. SyncD's own writes never overlap, but another program writing at that moment
  could make an account ask you to sign in again.
- One access violation in SyncD during a 20-minute test run was not reproduced in about five hours
  of later runs and is not explained.
- macOS is not part of this release.

Versions: Nextcloud Desktop v35.0.3 (engine), tested against Nextcloud server 35.0.2 and the
previous major 34.0.5; Go 1.27.4; MyGo v0.9.0; Qt 6.12.0; OpenSSL 3.6.5.

## Development of 0.1.0

- Release readiness (2026-11-13): patch 0019 keeps a local rename whose MOVE got no answer; the
  pipe frees its instance when a client vanishes; the uninstaller removes the settings folder under
  `%APPDATA%` (Go's `os.RemoveAll` failed there on this Windows build). A real cycle's renames and
  deletions showed in the installed interface's Activity, and the user enabled `fullSync` for engine
  builds. Tests: P09 runs 20 seeds (the CI x64 runner hit the 60 s limit), and the tests that use
  the Windows credential store no longer run in parallel (concurrent writes lose updates). Go 1.27.4 / MyGo v0.9.0 / Desktop v35.0.3 / server v35.0.2 (previous major v34.0.5)
  checked, unchanged.

- Multi-account and protocol 1.9 (2026-11-12): several accounts (`multiAccount` after a real 2×2
  run), account removal that never waits on a stuck switch, credential renewal that reconciles with
  a full quota and retries a refused reload instead of stopping SyncD, a full quota that retries
  instead of freezing its folder, separate reach and metadata deadlines so a slow server is not
  unreachable, `daemon.prepareUpdate`, `--version`, DLL loading limited to the install folder.
  Control schema 11. The interface shows them; the display name is Nextcloud MyGo. Per-user
  packages per architecture with a PE architecture check (ARM64X Visual C++ runtime accepted by the
  user). Test tiers: fast about 37 s per architecture, server about 7–8 min on ARM64 with its own
  container. Server tier passed against 35.0.2 and the previous major 34.0.5 on ARM64. Go 1.27.4 /
  MyGo v0.9.0 / Desktop v35.0.3 / server v35.0.2 checked, unchanged. Committed and pushed to
  `origin`; no tag.

- Windows on-demand files (M6, 2026-11-12): upstream's CFAPI plugin serves on-demand folders through
  SyncD; patch 0017 (plugin without shell extension, endpoint-independent sync-root identity, no
  unregistered handler classes, a request gate for the switch boundary). Protocol 1.8 adds the
  folder `mode`; Control schema 10 stores it. Real on-demand runs against server 35.0.2 passed on
  native ARM64 and emulated x64; `virtualFiles` stays `false`. Go 1.27.4 / MyGo v0.9.0 / Desktop
  v35.0.3 / server v35.0.2 checked, unchanged. No commit, tag or push.

- Reliability package (2026-11-11): fresh sixteen-patch upstream builds with the full suite on
  both architectures; real lost-reply recovery for a MOVE and a DELETE of existing entries and for
  a chunk session's final MOVE; process kills at five durable switch stages with restart recovery;
  real file-safety cases; a chunk session above 4 GiB on native ARM64. Control schema 9 lets a
  restarted daemon send a new login's held revocation once. Protocol 1.7 and the sixteen patches
  are unchanged; `fullSync` and `endpointAutoSwitch` stay `false`. Go 1.27.4 / MyGo v0.9.0 (moved
  by the interface work; `migrate-ui` reported no changes) / Desktop v35.0.3 (checked, unchanged) /
  server v35.0.2 (previous major v34.0.5; checked, unchanged). No commit, tag or push.

- LoginFlow classifies 403/423/429 with the shared metadata rules, gates requests behind a
  confirmed server delay, never extends its 20-minute validity and keeps an acquired password owned
  while its revocation is throttled. A successful half-open probe no longer fails a verified switch
  onto a recovering address. Shared rebuild: 21/21 CTest targets and real Go↔C++ entrypoints on
  native ARM64 and emulated x64; the real active-transfer fixture (upload fault, download fault,
  withheld PUT acknowledgement) passed on both against server 35.0.2. Chunking-v2 sessions are admitted across origins: test-only upstream patch 0016 and a real
  112 MiB upload interrupted in its second chunk, resumed without resending the first chunk on both
  architectures against an isolated server 35.0.2. Protocol 1.7 and Control schema 8 are unchanged;
  sixteen patches. Go 1.27.3 / MyGo v0.8.3 / Desktop v35.0.3 / server v35.0.2 (previous major v34.0.5)
  checked again on 2026-11-09, unchanged; `origin` only, fetch 0/0. No commit, tag or push.

- Protocol 1.7 adds explicit startup availability and folder reasons. Control schema 8 durable
  account-throttle integration is in progress and not yet accepted. Shared startup suites and actual
  Go↔C++ entrypoints passed on both architectures; bounded 32-receipt
  diagnostic shutdown passed isolated regression, awaiting final shared reruns. Real independent
  instance N02 passed on both architectures with process-local CA trust and owned-resource cleanup.
  Active-transfer withheld-acknowledgement and metadata-constraint fixes remain unfinished. No new
  upstream patch. On 2026-11-09 Go 1.27.3 / MyGo v0.8.3 / Desktop v35.0.3 / server v35.0.2
  (previous major v34.0.5) checked, unchanged. Product remains 0.1.0; no commit, tag or push.

- Protocol 1.6 / Control schema 7 connect durable selection policy, network events/probes, failed-target
  fresh-epoch recovery, aggregate activity and local sanitized diagnostics. Patch 0015 preserves headless
  capability updates. Both consuming suites passed 14/14; final real cross-origin transfer acceptance and
  serial upstream reruns remain pending. Go 1.27.3 / MyGo v0.8.3 / Desktop v35.0.3 / server v35.0.2
  (previous major v34.0.5) checked, unchanged. No release bump, tag or push; remote remains `origin` only.

- Protocol 1.5 / Control schema 6 add account-preserving reauthentication, reserved native-vault
  candidate/backup slots, restart-safe rollback/cleanup and same-origin runtime reload. Final ARM64 and
  emulated x64 contracts, real consent/renewal/restart uploads, original journals and cleanup passed.
  The nested cancellation/replacement lifetime regression reproduced `0xC0000005` before the local
  session-lease fix and passed on both architectures afterward. Fresh upstream suites passed 96/96
  per architecture, excluding startup registration. Automatic switching and full fault recovery remain open.
  Go 1.27.3 / MyGo v0.8.3 / Desktop v35.0.3 / server v35.0.2 (previous major v34.0.5) checked, unchanged.
  No upstream patch or release version change; configured remote remains `origin` only, fetch 0/0.

- Protocol 1.4 connects manual account-wide switch acceptance and new-epoch reconciliation; interrupted
  operations recover through fresh verification of the committed origin. Both architecture real-daemon
  roundtrips, paused-reconciliation restart/process-kill recovery and original-journal identity checks
  passed. Active-transfer/uncertain-write fault acceptance remains open. No new upstream patch or release bump.
- Go 1.27.3, MyGo v0.8.3, Desktop v35.0.3, server v35.0.2 and previous-major server v34.0.5 checked,
  unchanged on 2026-11-08. Only `origin` is configured; fetch found zero commits ahead/behind.

- Control schema 5 adds transactional switch intent/epoch storage and restart gates; protocol remains
  1.3. Both architecture contract suites and schema-change real-daemon regressions passed. Runtime
  activation through IPC and complete switching/recovery acceptance remain unfinished.

- Latest follow-up check: Go 1.27.3, Desktop v35.0.3 and server v35.0.2 checked, unchanged;
  previous supported server major is v34 (latest stable v34.0.5). MyGo upgraded to v0.8.3;
  migration preview required no source changes. Patches 0011–0013 add observable journal commit
  failure, headless account lifetime and corrupt-journal preservation; the combined upstream suites
  passed on ARM64 and emulated x64. Patch 0014 fixes completed-download resume cleanup; ARM64
  focused and real alternate-address adapter acceptance passed, with combined acceptance in progress.

- 2026-11-07: protocol 1.3 and Control schema 4 connect alternate-origin staging/consent and
  read-only namespace verification. Go 1.27.3, MyGo v0.8.1 and Desktop v35.0.3 checked, unchanged;
  latest stable server tag remains v35.0.2. Switching remains disabled.

- Upstream check on 2026-11-06: Go 1.27.3 and Desktop v35.0.3 checked, unchanged;
  MyGo upgraded to v0.8.1. The UI migration preview required no source changes.

- Connected SyncD foundation: durable pause/operation receipts, authenticated local pipe, Go client,
  default frontend connection and standalone epoch/quiescence switch guards. Login/full sync remain disabled.
- Upstream check on 2026-11-05: Go 1.27.3 and MyGo v0.7.4 unchanged; Desktop snapshot advanced to
  v35.0.3. Its native ARM64 engine passed the isolated basic real-server fixture and cleanup against
  server 35.0.2; full acceptance remains pending. Prior v35.0.2 engine evidence is historical.

- M0 started: unmodified Desktop v35.0.2 submodule and audit-input build record; Go 1.27.3
  and MyGo v0.7.4 checked, unchanged. Native engine builds and sync acceptance are pending.

- Repository scaffold: working conventions (`AGENTS.md`), the plan (`PLAN.md`), mirrored
  documentation, the documentation mirror check and the Go module.
- The interface against sample data: main window (Overview, Accounts & folders, Network,
  Activity with the deletion review), Add an address, first-run setup, Settings, tray and
  flyout, close to the tray, open at sign-in, signed self-update; English and Simplified
  Chinese; `internal/syncd` with the Mock standing in for SyncD.
- Go, MyGo and Nextcloud follow their latest releases instead of being pinned. Latest on
  2026-11-03: Go 1.27.3, MyGo v0.7.4, Nextcloud Desktop v35.0.2.
