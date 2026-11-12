# Version history

## 0.1.0 (in progress)

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
