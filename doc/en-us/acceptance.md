# Acceptance matrix

The acceptance matrix `PLAN.md` §20.6 asks for. Every row's status comes only from evidence
recorded in the concept docs; no run recorded means `not run`. It is updated with each result and
is not a release claim.

## Reading the tables

| Status | Meaning |
|---|---|
| `passed` | A real run of the case is recorded and meets the plan's pass condition |
| `passed (simulated)` | A real run of a simulation the user accepted in place of the real condition meets the pass condition; Missing names what only the real condition shows |
| `partial` | Part of the condition is evidenced, or only a mock, scripted-transport or unit test exists for a case the plan requires on a real server |
| `not run` | No run is recorded |
| `limitation` | Not shown for 0.1.0 and not claimed: the reason is under [Known limitations for 0.1.0](#known-limitations-for-010) |

Abbreviations: **ARM64** is native Windows 11 ARM64; **x64e** is x64 binaries under emulation on
the same ARM64 host, which counts as x64 evidence by the user's decision; **35.0.2** is the latest
Nextcloud server release, the one every case ran against (the server tier also passed once against
34.0.5, the previous major, [development](development.md#test-tiers)). **Server tier** is
`tools/test-all.ps1 -Tier server` on ARM64 against a disposable 35.0.2 container; **real pipe** is a
Go test against the compiled daemon over its Named Pipe. Evidence links name the document that records
the run. Cases the plan numbers only as a list (§20.5) get `P` numbers here in plan order.

## Basic sync and file safety (§20.2)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| S01 | `passed` | [platform notes](platform-notes.md): CLI, adapter and daemon fixtures upload, download, and an unchanged cycle with zero DAV content requests (positive control passed); ARM64 and x64e, 35.0.2 | 1,000-file manifest dataset |
| S02 | `passed` | [platform notes](platform-notes.md): bidirectional modifications with journal reuse; ARM64 and x64e, 35.0.2 | Manifest content check on a large dataset |
| S03 | `passed` | [platform notes](platform-notes.md): conflict content of both sides preserved; ARM64 and x64e, 35.0.2; B14: shown and resolved in the installed interface, ARM64 | A truly independent second client |
| S04 | `passed` | [platform notes](platform-notes.md): `file-safety` renames and directory moves both ways on the real server, server file IDs kept, no content sent again, no deletion review; isolated server ARM64 1 min 6 s, x64e 59 s, shared server both; in every ARM64 server tier since; B14: a local rename in the installed interface | none |
| S05 | `partial` | [platform notes](platform-notes.md): transfers cut through the owned relay recover with matching hashes (`network-active` upload, download and lost PUT reply; ARM64 and x64e) | A modification of an existing file during the cut; plain network loss without failover |
| S06 | `passed` | [platform notes](platform-notes.md): `network-chunked` 112 MiB upload cut in its second chunk resumes without resending the first (ARM64 2 min 55 s, x64e 2 min 58 s); `network-large` 4,296 MiB chunk session cut after 4 GiB resumes with matching streamed hashes (ARM64 28 min 35 s); `network-active` download cut and recovered; upstream `ChunkingNgTest` 22/22 | `network-large` on x64e; a download above 4 GiB |
| S07 | `passed` | [platform notes](platform-notes.md): a server-committed PUT and a chunk session's final MOVE with their replies withheld are recovered by rediscovery, hashes, server file ID, ETag and original journal match; ARM64 and x64e | Failure-stage injection beyond the reply |
| S08 | `passed` | [platform notes](platform-notes.md): `network-uncertain` MOVE and DELETE of existing entries committed with the reply lost, path created again by another client, recovered without a second MOVE or DELETE, re-created object untouched (same file ID and ETag); ARM64 12 min 31 s, x64e 10 min 27 s; in every ARM64 server tier since | none |
| S09 | `partial` | [platform notes](platform-notes.md): real filesystem notifications, dirty work during a cycle kept (ARM64 1.27 s, x64e 2.75 s) | Event at the cycle-end boundary with a live engine on a real server |
| S10 | `partial` | [platform notes](platform-notes.md): real buffer overflow requests a full audit; [engine](engine.md) | An omitted change found by the audit on a real server |
| S11 | `partial` | [platform notes](platform-notes.md): `file-safety` holds one directory with twelve descendants as 13 remote deletions (ratio threshold) before propagation, applied only after approval, ARM64 and x64e; B14: 25 of 30 reviewed in the interface; threshold arithmetic in `SyncDDeletionContracts`; the 100-deletion case was added to `file-safety` ([platform notes](platform-notes.md#acceptance-audit-for-010)) but has not run yet | A run of the 100-deletion case (absolute threshold alone, local direction) in the server tier |
| S12 | `passed` | [platform notes](platform-notes.md): `file-safety` removes one directory with twelve descendants locally: proposed as 13 remote deletions, not one, nothing removed before the decision; ARM64 and x64e, isolated and shared server | none |
| S13 | `partial` | [platform notes](platform-notes.md): `file-safety` cannot rename the mapped root away while leased (real server, ARM64 and x64e); unavailable-root contracts keep work pending, changed roots refused | A root denied by permissions against a real server, with nothing propagated |
| S14 | `partial` | [platform notes](platform-notes.md): `file-safety` remote root answering 404 reported as `REMOTE_ROOT_UNAVAILABLE`, every local file kept; ARM64 and x64e; 403/423/429 classification in contracts | A maintenance page and a partial 207 from a real server |
| S15 | `partial` | [engine](engine.md): identity rechecked before each cycle; changed-root contracts ([platform notes](platform-notes.md)) | Drive-letter reuse, replaced directory, SUBST |
| S16 | `partial` | [platform notes](platform-notes.md), [engine](engine.md): upstream regressions for sticky commit failure and preserved corrupt bytes (patches 0011, 0013); a corrupt Control database is kept byte for byte and refuses mutations on the real pipe (`TestRealPipeUnavailableStopsINV11INV12`) | Daemon stopping affected writes on a corrupt sync journal; journal error injection |
| S17 | `not run` | No run is recorded | All of it |
| S18 | `not run` | [engine](engine.md) describes preflight refusal; no run is recorded | All of it |
| S19 | `partial` | [platform notes](platform-notes.md): hierarchical root admission, 3 repetitions / 144 races; ARM64 18.27 s, x64e 26.27 s | Non-NTFS refusal, SUBST, crash during publication; a separate session is a [limitation](#known-limitations-for-010) |
| S20 | `passed` | [platform notes](platform-notes.md): `file-safety` approves a deletion after one more local deletion on the real server: the stale approval removes nothing, the changed set is proposed again and applied only after its own approval; ARM64 and x64e; old-epoch proposals refused in contracts | none |
| S21 | `not run` | No run is recorded | All of it |

## Network, trust and switching (§20.3)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| N01 | `passed` | [platform notes](platform-notes.md): two addresses of one server registered; ARM64 and x64e, 35.0.2; B14 step 6: the alternate added in the installed interface with certificate and server shown before any credential, the answer times of both, and every switch with its reason; scoring in deterministic contracts | none |
| N02 | `passed` | [platform notes](platform-notes.md): two real isolated instances, same UID, distinct namespace, refused and quarantined; ARM64 and x64e, 35.0.2 | none |
| N03 | `partial` | [platform notes](platform-notes.md): verification refused before consent; negative-consent contracts | A reachable unregistered endpoint under capture; same-name SSID |
| N04 | `partial` | [engine](engine.md): hard TLS/identity failures quarantine; connected regressions | Real expired, wrong-host and unknown-CA endpoints |
| N05 | `partial` | [url usage audit](url-usage-audit.md), [accounts](accounts.md): login and metadata clients never follow redirects; cross-origin login URLs rejected | Engine-path redirect to an unknown domain or HTTP |
| N06 | `not run` | [backend progress](backend-progress.md): different installation subpaths are not recorded | All of it |
| N07 | `limitation` | [engine](engine.md): strict VPN egress is not advertised | Not claimed for 0.1.0 |
| N08 | `partial` | [engine](engine.md): generation-bound probes; contracts; the seeded property run (P04) | Real rapid interface changes and late DNS |
| N09 | `partial` | [ipc](ipc.md), [engine](engine.md): deterministic policy with debounce on a caller clock | Margin, dwell and cooldown cases listed one by one |
| N10 | `partial` | [engine](engine.md): circuit opens on three independent failures; connected regressions; `network` waits out two real circuit-open windows | A real run that asserts the three independent failures |
| N11 | `partial` | [platform notes](platform-notes.md): healthy deferral contracts, 11/11; ARM64 and x64e; B14: a switch chosen during a 20 MiB upload waited for the transfer to finish; ARM64, 35.0.2 | A better address appearing during a healthy large transfer (automatic switching is off) |
| N12 | `passed` | [platform notes](platform-notes.md): interrupted transfers fail over to the other origin after drainage with a new epoch; ARM64 and x64e, 35.0.2 | none |
| N13 | `partial` | [engine](engine.md): old-epoch completions ignored in Control regressions | Real late request from an old epoch |
| N14 | `partial` | [platform notes](platform-notes.md): failed-target verification and fresh previous-origin recovery on both architectures | First business failure after successful verification |
| N15 | `partial` | [engine](engine.md): all-bad and fixed waiting, no alternation between bad addresses | Real all-addresses-down run |
| N16 | `not run` | No run is recorded | All of it |
| N17 | `not run` | No run is recorded | All of it |
| N18 | `partial` | [accounts](accounts.md), [engine](engine.md): throttle storage; scripted-transport regressions only | Real 429/Retry-After; live-engine consumers |
| N19 | `partial` | [ipc](ipc.md), [platform notes](platform-notes.md): a changed root mapping invalidates alternate verification | Real new-folder run |
| N20 | `not run` | No run is recorded | All of it |
| N21 | `passed` | [platform notes](platform-notes.md): primary/alternate/primary switches keep the native journal files, unchanged-cycle request checks; ARM64 and x64e, 35.0.2 | none |
| N22 | `partial` | [ipc](ipc.md): half-open probes limited in policy contracts | Multiple network events on a real run |
| N23 | `limitation` | [engine](engine.md): strict VPN egress is not advertised | Not claimed for 0.1.0 |

## Lifecycle, IPC and upgrade (§20.4)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| R01 | `passed` | [platform notes](platform-notes.md): the daemon survives a client disconnect, real Go↔C++ pipe, ARM64 and x64e; B14: the installed window force-killed, SyncD kept running, the new window showed the account; ARM64, 35.0.2 | x64e of the forced kill |
| R02 | `partial` | [platform notes](platform-notes.md): process kills during unfinished uploads and downloads; ARM64 | Discovery and finish phases; x64e |
| R03 | `passed` | [platform notes](platform-notes.md): `switch-kill` ends the process at each durable switch stage (`prepared`, `quiescing`, `verifying`, `committed`, `reconciling`) and restarts it on the same profile: previous binding for the first three, committed for the last two, fresh epoch, same credential reference, folder rows and journal, nothing reconciled sent again; ARM64 and x64e, isolated and shared server; in every server tier since | none |
| R04 | `passed` | [platform notes](platform-notes.md): pauses survive switching and recovery in real daemon runs; ARM64 and x64e, 35.0.2 | none |
| R05 | `partial` | [platform notes](platform-notes.md): `multi-account`, two accounts with two folders each on one daemon, content both ways in all four, one account switched while the other kept syncing unpaused; server tier ARM64; serial scheduler contracts ([engine](engine.md)) | A real run that asserts one engine cycle at a time under sustained changes |
| R06 | `limitation` | [platform notes](platform-notes.md): separate-session checks are unrun | A second Windows logon session |
| R07 | `partial` | [accounts](accounts.md): reauthentication and revocation real runs, 35.0.2 | Locked key store; token revoked by the server |
| R08 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeFramingR08R12` on the real pipe: a frame split inside its prefix and body, two frames in one write, and six malformed frames (zero length, above 1 MiB, invalid UTF-8, duplicate key, truncated JSON, nesting above 64) each end only that connection, the next client is served with the state kept, a never-completed half frame is dropped after 15.0 s; ARM64 16.2 s, x64e 17.1 s | none |
| R09 | `partial` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeImpostorR09`: a process holding the pipe name first is refused by the client (wrong executable) and keeps the daemon from starting under that name; a client through `\\127.0.0.1\pipe\` is refused as remote (access denied); unexpected server path refused (`TestRealPipeINV12INV13`); ARM64 and x64e | Cross-user and cross-session: a [limitation](#known-limitations-for-010) |
| R10 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeReplayR10R11R14`: a pause whose reply was never read committed once; the same key, sent again after reconnecting, after a later resume and after a daemon restart, returns the same receipt and never pauses again; ARM64 1.2 s, x64e 2.3 s | none |
| R11 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): the same key with other input returns `OPERATION_KEY_CONFLICT` and the state and revision stay unchanged, real pipe; ARM64 and x64e | none |
| R12 | `partial` | [ipc](ipc.md): atomic bootstrap and subscription; `TestRealPipeFramingR08R12`: a change committed right after a bootstrap reaches the subscription with a later `seq`, real pipe | A race with state changes from another source during bootstrap |
| R13 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeSlowClientR13`: a client that never reads is dropped on the bounded output queue (ARM64 after 2.3 s, x64e 8.6 s), the next client is served within 5 s, `events.resume` answers `RESYNC_REQUIRED`; restart re-bootstraps (`TestRealPipeINV12INV13`) | none |
| R14 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeReplayR10R11R14`: `session.hello` with major 2 returns `PROTOCOL_MISMATCH`, a mutation and a bootstrap before negotiation are refused the same way and execute nothing; ARM64 and x64e | none |
| R15 | `passed (simulated)` | [platform notes](platform-notes.md): `sleep-resume` ([development](development.md)), the whole daemon process suspended 35 s during a held upload and woken where only the alternate answers: dead connection failed, nothing read as deleted, switch at an idle folder with reason `unreachable`, upload completed once, journal and folder identity kept; ARM64, 35.0.2. [Engine](engine.md): resume notification on an injected clock; UTC rollback keeps throttle deadlines (`syncd_constraint_tests`). A later run on an idle machine failed at "switch did not wait for the folder to go idle" and is under investigation in another session | A real sleep: power transitions and their notifications reaching the running daemon, whether the monotonic clock counts the sleep, adapter and address changes, connections that die without a reset, hours-long sleeps, volume changes while asleep, Modern Standby; x64e |
| R16 | `partial` | [packaging](packaging.md#signed-updates): a refused download (wrong key, changed archive or manifest, other architecture, not newer) and a connection broken halfway through the archive (`TestUpdateInterruptedDownload`) leave nothing staged and start no installer (unit tests); the in-app path installed a signed 0.1.1 over a running 0.1.0 through setup and `daemon.prepareUpdate`, profile state kept, ARM64 | A failing setup in the in-app path on a real install; the installed interface starting it |
| R17 | `partial` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealNewerControlSchemaR17`: the compiled daemon given a Control database one schema newer serves only `databaseUnavailable`, refuses mutations and leaves the file byte for byte (ARM64 2.3 s, x64e 2.6 s); migration backups refuse a newer schema (`SyncDStartupContracts`); the installer refuses a downgrade (unit tests) | A sync journal upgraded by a newer Desktop release opened by an older one |
| R18 | `not run` | No run is recorded | All of it |
| R19 | `partial` | [packaging](packaging.md): ARM64 per-user install, reinstall and uninstall; startup entry, shortcut, Apps entry and install folder removed; official client registration unchanged; B14: synced files kept after account removal and uninstall, ARM64 | Uninstall with the account still signed in; the x64 package is a [limitation](#known-limitations-for-010) |
| R20 | `passed` | [platform notes](platform-notes.md): ARM64 real runs and B14: the installed ARM64 package (30 PE files native ARM64) signed in, synced, switched, reinstalled and uninstalled over publicly trusted TLS, 35.0.2; [packaging](packaging.md): dependency closure; sleep and resume (§15.2 item 7) passed by simulation only (R15) | A real sleep and resume (§15.2 item 7) |
| R21 | `not run` | Left to the accelerated soak run that replaces the 24-hour run by the user's decision | All of it |
| R22 | `partial` | [ipc](ipc.md): the support bundle holds only sanitized fields; B14: an exported bundle held no password, user, server, path or file name | A log scan for secrets |

## Properties (§20.5)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| P01 | `partial` | [platform notes](platform-notes.md): S19 admission races, ARM64 and x64e | Randomized event-sequence test |
| P02 | `partial` | [url usage audit](url-usage-audit.md), [engine](engine.md): origin gate in contracts and real runs | Randomized test |
| P03 | `partial` | [engine](engine.md): epoch-bound completion contracts | Randomized test |
| P04 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `SyncDPropertyContracts`, 500 seeded sequences of 400 random policy events: every ticket from an older network generation, an older configuration, a hard failure or a review, or one already consumed, is refused and changes no health field; ARM64 0.18 s, x64e 0.40 s | none |
| P05 | `partial` | [platform notes](platform-notes.md): pauses survive in contracts and real runs | Randomized test |
| P06 | `partial` | [engine](engine.md): old-epoch proposals refused | Randomized test |
| P07 | `not run` | No run is recorded | All of it |
| P08 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): the same run: no automatic or fixed proposal ever names an endpoint that is disabled, unapproved, unverified, without valid TLS, not allowed on the network, quarantined or blocked for authentication, and a fixed target never falls back to another one; a deliberately wrong model failed with seed 1 and shrank to two steps | none |
| P09 | `partial` | [ipc](ipc.md): durable receipts; restart persistence in real runs (`TestRealPipeReplayR10R11R14`) | Randomized test |

The property run prints its seed and the minimal failing sequence on a failure;
`SYNCD_PROPERTY_SEED` replays one seed.

## Final checklist (§23)

| Item | Status | Evidence | Missing |
|---|---|---|---|
| 23.1-1 latest upstream, dependencies, patches | `passed` | [platform notes](platform-notes.md), [backend progress](backend-progress.md): Desktop v35.0.3, the latest release (checked 2026-11-12), seventeen recorded patches in `build-lock.json`; the upstream suite with them 95/96 on ARM64 (the failing target passed 3/3 alone) and 96/96 on x64e, `SyncCfApiTest` 50 passed, 2 skipped | none |
| 23.1-2 native ARM64 engine and dependencies | `passed` | [platform notes](platform-notes.md): ARM64 real runs, PE scans; B14: full sync through the checked installed package | none on ARM64 |
| 23.1-3 sync after the UI exits | `passed` | [platform notes](platform-notes.md): daemon survives disconnect; B14: sync with the window closed and after a forced kill of the installed window | none |
| 23.1-4 identity, credential reference, journal stable | `passed` | [platform notes](platform-notes.md): account, credential reference, folder rows and native journal file checked unchanged across every `switch-kill` stage, failover, simulated sleep and renewal, ARM64 and x64e (N21, R03) | none |
| 23.1-5 scheduling and blocking semantics | `partial` | [engine](engine.md): scheduler contracts; two accounts with two folders each real (R05) | Fairness at scale |
| 23.1-6 origin, namespace, TLS, redirects | `partial` | [platform notes](platform-notes.md): N02 passed | N04, N05, N06 |
| 23.1-7 scoring, debounce, circuit, reasons | `partial` | [engine](engine.md): deterministic contracts; P04 and P08 passed; switch reasons shown in the interface (B14) | Scoring and debounce driving real switches (automatic switching is off) |
| 23.1-8 switch state machine under fault injection | `passed` | [platform notes](platform-notes.md): kills at all five durable switch stages (R03), lost replies of committed PUT, MOVE, DELETE and final MOVE (S07, S08), interrupted transfers and failover (N12), all real | none |
| 23.1-9 conflicts, deletion protection, root identity | `partial` | [platform notes](platform-notes.md): conflicts, deletion review at the ratio threshold, descendants and stale approvals real (S03, S12, S20) | S11 at 100 objects, S13, S14, S15, S18, S21 |
| 23.1-10 error classes, bounded retries | `partial` | [engine](engine.md): classification and backoff; real 404 and full quota | Real 429, maintenance |
| 23.2-1 schema, methods, events, errors, version | `partial` | [ipc](ipc.md): protocol 1.10, version negotiation refused across majors on the real pipe (R14) | The release that freezes protocol 1.10 |
| 23.2-2 real Go/C++ Named Pipe | `passed` | [platform notes](platform-notes.md): real pipe tests; ARM64 and x64e | none |
| 23.2-3 idempotent commands after reconnect | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): R10 and R11 on the real pipe, receipts across reconnect and restart | none |
| 23.2-4 snapshot and event races, backpressure | `partial` | [platform notes](platform-notes.md#acceptance-audit-for-010): R13 passed; R12 partial | R12's concurrent-source race |
| 23.2-5 frontend holds no secrets or database | `partial` | [gui](gui.md): the interface goes through IPC only | A scan of the connected interface |
| 23.2-6 consent, deletion confirm, reauth completable | `partial` | [ipc](ipc.md): real backend runs; B14 ([platform notes](platform-notes.md)): sign-in, address approval, deletion review and conflict completed in the installed interface | Reauthentication through the interface |
| 23.3-1 naming and icon | `partial` | [packaging](packaging.md#coexistence-with-the-official-client): the display name is **Nextcloud MyGo** (Start menu shortcut, Apps entry, window title, interface text), identifiers stay `Nextcloud-MyGo` (folders, keys, executables, pipe names); executable icon from the upstream icon; B14: the tray icon shown | Trademark check |
| 23.3-2 package architectures and dependencies | `partial` | [packaging](packaging.md): both packages checked (ARM64 28, x64 29 PE files, native); B14: the ARM64 package checked and run | Installing the x64 package: a [limitation](#known-limitations-for-010) |
| 23.3-3 install, upgrade, downgrade, uninstall, coexistence | `partial` | [packaging](packaging.md): ARM64 install, same-version upgrade, uninstall and coexistence; downgrade refusal unit-tested; B14: same-version reinstall over a running, signed-in copy kept account and folder; package smoke: 0.1.0 → 0.1.1 test-version upgrade over a running SyncD through `daemon.prepareUpdate`, profile state and startup report (previous version, schema, self-check passed) checked, ARM64 | An upgrade between versions with a signed-in account; x64 is a [limitation](#known-limitations-for-010) |
| 23.3-4 matrix, content checks, 24-hour report | `not run` | This document; the 24-hour run is left to the accelerated soak run | The soak report |
| 23.3-5 auto-update only with a trusted chain | `partial` | [packaging](packaging.md#signed-updates): updates exist only in packages built with the `mygo keygen` key, kept in a GitHub secret; the app installs only archives signed with the key in `mygo.json`, off until the user turns it on; package smoke: a signed 0.1.1 test update from loopback HTTP installed over 0.1.0 with a throwaway key, ARM64 | A release signed with the project key, read from GitHub; Authenticode is a [limitation](#known-limitations-for-010) |
| 23.3-6 §17.1 desktop features | `partial` | [platform notes](platform-notes.md): B14: tray icon and flyout, close to tray, Start menu reopen, Quit and stop syncing; setup reaches `daemon.prepareUpdate` before stopping SyncD (package smoke) | `daemon.prepareUpdate` refusals with a real deletion waiting; notifications; start at sign-in; a real sleep is a [limitation](#known-limitations-for-010) |
| 23.3-7 licenses, source, limits, diagnostics notes | `partial` | [platform notes](platform-notes.md): known limitations; this page's limitation list | User guide; release source materials |
| 23.3-8 no unverified capability listed as supported | `partial` | [Capability evidence](#capability-evidence): `fullSync`, `endpointAutoSwitch` and `virtualFiles` stay `false` | Release list |

## Milestone exits (§18)

| Milestone | Status | Evidence | Missing |
|---|---|---|---|
| M0 feasibility and audit | `partial` | [platform notes](platform-notes.md): real basic sync on native ARM64, 35.0.2; [backend progress](backend-progress.md) calls M0 incomplete | Remaining audits; the macOS File Provider audit is outside 0.1.0; native x64 is a [limitation](#known-limitations-for-010) |
| M1 contracts, accounts, single endpoint | `partial` | [ipc](ipc.md), [accounts](accounts.md): two-folder sync, login, no secrets in IPC; R08, R10, R11, R13, R14 on the real pipe | Whole exit not signed off |
| M2 reliability and file safety | `partial` | [engine](engine.md): scheduler, root lease, deletion veto; S04, S06–S08, S12, S20 passed | S05, S09–S11, S13–S18, S21 |
| M3 endpoint registry, read-only policy | `partial` | [ipc](ipc.md): consent, namespace check, policy; N01, N02 passed | N04, N06; N07 is a [limitation](#known-limitations-for-010) |
| M4 safe automatic switching | `partial` | [platform notes](platform-notes.md): manual and failover switching real, uncertain writes recovered (S07, S08), every switch stage killed and recovered (R03); automatic capability `false` | Automatic switching evidence ([Capability evidence](#capability-evidence)); VPN is a [limitation](#known-limitations-for-010) |
| M5 frontend, first release | `partial` | [platform notes](platform-notes.md): B14 joint acceptance on ARM64, 35.0.2, six of nine steps passed; the B14 findings fixed in SyncD and setup | Steps 2, 6 and 8 again through the interface; upgrade between versions with an account; the soak run |
| M6 cloud-file frameworks | `partial` | [on-demand](on-demand.md), [platform notes](platform-notes.md): Windows on-demand registration, hydration, free up space, always keep, switch boundary, crash recovery and unregistration on native ARM64 and x64e against server 35.0.2 through the engine runtime; hosts where Cloud Files cannot answer skip instead of passing | Paired daemon and interface, another origin, deletion protection and offline with placeholders, uninstall unregistration; `virtualFiles` false; the macOS half is outside 0.1.0 by the user's decision of 2026-11-11 |

## Joint release acceptance (B14)

Native Windows 11 ARM64, installed package, real window driven by UI Automation, Nextcloud 35.0.2,
2026-11-12; details, findings and capability evidence in [platform notes](platform-notes.md).

| Step | Status | Checked by |
|---|---|---|
| 1 install, setup, sign in, folder, first sync | `passed` | interface; the setup's Start step not exercised |
| 2 local and remote changes, Activity | `partial` | interface and state; times in UTC; protocol 1.10 adds `deletedLocal`, `renamedRemote` and `renamedLocal` (`testCycleActivity`), shown by the interface against the Mock (`TestActivityKinds110`), not yet from a real cycle |
| 3 bulk deletion review | `passed` | interface and server |
| 4 conflict | `passed` | interface and files |
| 5 folder pause through pause all and resume all | `passed` | interface and server |
| 6 addresses and switching | `partial` | interface; fixed/automatic not offered; a switch held past its drain window now ends as `switchFailed` (`switch-quota`, ARM64, 35.0.2), not run again in the interface |
| 7 close, reopen, quit from the tray | `passed` | interface and processes |
| 8 reinstall over the running copy | `partial` | interface and processes; since then setup calls `daemon.prepareUpdate`, the self-check no longer fails on `CANCELLED` (`SyncDFolderErrorContracts`) and a 0.1.0 → 0.1.1 test upgrade passed in the package smoke; not run again in the interface |
| 9 remove the account, uninstall | `passed` | interface, registry, server token list |
| sleep and resume | `passed (simulated)` | `sleep-resume` against the backend, not through the interface or the installed package (R15); a real sleep was ruled out for this machine by the user |

## Capability evidence

The three capabilities stay `false`; only the user decides to enable one. What exists and what is
missing for each:

| Capability | Evidence | Missing to enable it |
|---|---|---|
| `fullSync` | S01–S04, S06–S08, S12, S20 passed on a real server (ARM64 and x64e for most); conflicts, deletion review, pause and resume through the installed interface (B14); two accounts with two folders each (R05 partial); the daemon survives the UI (R01, 23.1-3) | S05, S09–S11, S13–S18 and S21; the soak run (R21); Activity of local deletions and renames from a real cycle (B14 step 2) |
| `endpointAutoSwitch` | Manual and fixed switching real, deferred to a safe point (N11, B14 step 6); failover with drainage and a fresh epoch (N12); every switch stage killed and recovered (R03); uncertain writes recovered (S07, S08); P04 and P08 on the policy; a simulated sleep switching with reason `unreachable` (R15, under investigation) | Real runs of the automatic policy: N08–N11, N13–N17, N19, N20 and N22 with scoring, margin, dwell and cooldown driving switches; the interface offering automatic mode; N04 and N05 on real endpoints |
| `virtualFiles` | Windows on-demand folders through the engine runtime on a real server: placeholders, hydration, free up space, always keep, a switch waiting for a hydration, a kill during a hydration and unregistration, ARM64 and x64e (M6) | The paired daemon and interface choosing the mode; another origin; deletion protection and offline behavior with placeholders; uninstall unregistration; the shell extension for thumbnails and overlays |

## Known limitations for 0.1.0

Cases or parts of cases this release does not show and does not claim, by the user's decision or
because the single machine cannot show them:

| Item | Reason |
|---|---|
| N07, N23 strict VPN egress | Not advertised in 0.1.0: candidate exclusion by route needs a real VPN path, and none is recorded; no endpoint is ever accepted by interface name |
| R06 and the separate-session parts of R09 and S19 | Need a second Windows logon session or user account on this machine, which this run does not create |
| Real sleep and resume (R15, R20, §15.2 item 7) | The user ruled out a real sleep of this machine; `sleep-resume` is the accepted simulation |
| Native x64 hardware and installing the x64 package (23.3-2, 23.3-3, R19) | One ARM64 machine: x64 binaries run under emulation and count as x64 evidence, but the x64 installer refuses to run on ARM64 by design |
| Authenticode (23.3-5) | No code-signing certificate: SmartScreen warns on the first install; updates are still signed with the MyGo updater key |
| macOS (M0 audit, M6 File Provider) | Outside 0.1.0 by the user's decision of 2026-11-11 |
| 24-hour run (R21, 23.3-4) | Replaced by an accelerated soak run of about one hour, recorded separately |
