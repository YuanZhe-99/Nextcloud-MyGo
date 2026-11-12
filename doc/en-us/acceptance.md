# Acceptance matrix

The acceptance matrix `PLAN.md` §20.6 asks for. Every row's status comes only from evidence
recorded in the concept docs; no run recorded means `not run`. It is updated with each result and
is not a release claim.

## Reading the tables

| Status | Meaning |
|---|---|
| `passed` | A real run of the case is recorded and meets the plan's pass condition |
| `partial` | Part of the condition is evidenced, or only a mock, scripted-transport or unit test exists for a case the plan requires on a real server |
| `not run` | No run is recorded |
| `not possible on this hardware` | The single available machine cannot prove it |

Abbreviations: **ARM64** is native Windows 11 ARM64; **x64e** is x64 binaries under emulation on
the same ARM64 host; **35.0.2** is the only Nextcloud server version tested. Evidence links name
the document that records the run. Cases the plan numbers only as a list (§20.5) get `P` numbers
here in plan order.

## Basic sync and file safety (§20.2)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| S01 | `passed` | [platform notes](platform-notes.md): CLI, adapter and daemon fixtures upload, download, and an unchanged cycle with zero DAV content requests (positive control passed); ARM64 and x64e, 35.0.2 | 1,000-file manifest dataset; native x64 |
| S02 | `passed` | [platform notes](platform-notes.md): bidirectional modifications with journal reuse; ARM64 and x64e, 35.0.2 | Manifest content check on a large dataset |
| S03 | `passed` | [platform notes](platform-notes.md): conflict content of both sides preserved; ARM64 and x64e, 35.0.2; B14: shown and resolved in the installed interface, ARM64 | A truly independent second client |
| S04 | `partial` | [platform notes](platform-notes.md), B14 (installed package, real window, 2026-11-12): a local rename reached the server, both sides matched; ARM64, 35.0.2 | A server-side rename; directory moves |
| S05 | `partial` | [platform notes](platform-notes.md): transfers cut through the owned relay recover with matching hashes (ARM64 12 min 26 s, x64e 14 min 28 s) | A modification of an existing file; plain network loss without failover |
| S06 | `partial` | [platform notes](platform-notes.md): 112 MiB upload cut in its second chunk resumes, hashes match (ARM64 2 min 52 s, x64e 2 min 56 s); upstream `ChunkingNgTest` 22/22 | Other cut points, downloads, files above 4 GiB |
| S07 | `passed` | [platform notes](platform-notes.md): a server-committed PUT with its acknowledgement withheld is recovered by rediscovery, hashes and original journal match; ARM64 and x64e | Withheld final MOVE; failure-stage injection |
| S08 | `not run` | [backend progress](backend-progress.md) lists uncertain MOVE/DELETE as not done | All of it |
| S09 | `partial` | [platform notes](platform-notes.md): real filesystem notifications, dirty work during a cycle kept (ARM64 1.27 s, x64e 2.75 s) | Event at the cycle-end boundary with a live engine on a real server |
| S10 | `partial` | [platform notes](platform-notes.md): real buffer overflow requests a full audit; [engine](engine.md) | An omitted change found by the audit on a real server |
| S11 | `partial` | [platform notes](platform-notes.md): deletion blocked before propagation, one-use approval, consumed decision refused; ARM64 and x64e, 35.0.2; B14: 25 of 30 files reviewed in the interface, Decide later kept all, Allow after a second question; ARM64 | The 100-object and 10-object/20% thresholds at scale; direction and count check |
| S12 | `not run` | [engine](engine.md) describes descendant counting; no run is recorded | All of it |
| S13 | `partial` | [engine](engine.md), [platform notes](platform-notes.md): unavailable-root contract tests keep work pending, changed roots refused | Root removed or denied against a real server, with nothing propagated |
| S14 | `not run` | Only synthetic 403/423/429 classification ([platform notes](platform-notes.md)) | 404, maintenance page, partial 207 on a real server |
| S15 | `partial` | [engine](engine.md): identity rechecked before each cycle; changed-root contracts ([platform notes](platform-notes.md)) | Drive-letter reuse, replaced directory, SUBST |
| S16 | `partial` | [platform notes](platform-notes.md), [engine](engine.md): upstream regressions for sticky commit failure and preserved corrupt bytes (patches 0011, 0013; 96/96) | Daemon stopping affected writes on a corrupt journal; journal error injection |
| S17 | `not run` | No run is recorded | All of it |
| S18 | `not run` | [engine](engine.md) describes preflight refusal; no run is recorded | All of it |
| S19 | `partial` | [platform notes](platform-notes.md): hierarchical root admission, 3 repetitions / 144 races; ARM64 18.27 s, x64e 26.27 s | Non-NTFS refusal, separate session, SUBST, crash during publication |
| S20 | `partial` | [platform notes](platform-notes.md), [engine](engine.md): old-epoch and consumed proposals refused in contracts and the daemon fixture | Data changing after confirmation on a real server |
| S21 | `not run` | No run is recorded | All of it |

## Network, trust and switching (§20.3)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| N01 | `partial` | [platform notes](platform-notes.md): two addresses of one server registered; ARM64 and x64e, 35.0.2; scoring in deterministic contracts | Explainable scores shown through the connected interface |
| N02 | `passed` | [platform notes](platform-notes.md): two real isolated instances, same UID, distinct namespace, refused and quarantined; ARM64 and x64e, 35.0.2 | none |
| N03 | `partial` | [platform notes](platform-notes.md): verification refused before consent; negative-consent contracts | A reachable unregistered endpoint under capture; same-name SSID |
| N04 | `partial` | [engine](engine.md): hard TLS/identity failures quarantine; connected regressions | Real expired, wrong-host and unknown-CA endpoints |
| N05 | `partial` | [url usage audit](url-usage-audit.md), [accounts](accounts.md): login and metadata clients never follow redirects; cross-origin login URLs rejected | Engine-path redirect to an unknown domain or HTTP |
| N06 | `not run` | [backend progress](backend-progress.md): different installation subpaths are not recorded | All of it |
| N07 | `not run` | [engine](engine.md): strict VPN egress is not advertised | All of it |
| N08 | `partial` | [engine](engine.md): generation-bound probes; contracts | Real rapid interface changes and late DNS |
| N09 | `partial` | [ipc](ipc.md), [engine](engine.md): deterministic policy with debounce on a caller clock | Margin, dwell and cooldown cases listed one by one |
| N10 | `partial` | [engine](engine.md): circuit opens on three independent failures; connected regressions | Real three-failure run |
| N11 | `partial` | [platform notes](platform-notes.md): healthy deferral contracts, 11/11; ARM64 and x64e; B14: a switch chosen during a 20 MiB upload waited for the transfer to finish; ARM64, 35.0.2 | Healthy large transfer with a better address appearing |
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
| N23 | `not run` | [engine](engine.md): strict VPN egress is not advertised | All of it |

## Lifecycle, IPC and upgrade (§20.4)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| R01 | `passed` | [platform notes](platform-notes.md): the daemon survives a client disconnect, real Go↔C++ pipe, ARM64 and x64e; B14: the installed window force-killed, SyncD kept running, the new window showed the account; ARM64, 35.0.2 | x64e of the forced kill |
| R02 | `partial` | [platform notes](platform-notes.md): process kills during unfinished uploads and downloads; ARM64 | Discovery and finish phases; x64e |
| R03 | `partial` | [platform notes](platform-notes.md): kill after binding publication, prepared and committed crashes in Control contracts | A kill at every durable phase |
| R04 | `passed` | [platform notes](platform-notes.md): pauses survive switching and recovery in real daemon runs; ARM64 and x64e, 35.0.2 | none |
| R05 | `partial` | [engine](engine.md): serial scheduler contracts; one account with two folders in real runs | Two accounts; sustained changes |
| R06 | `not run` | [platform notes](platform-notes.md): separate-session checks are unrun | All of it |
| R07 | `partial` | [accounts](accounts.md): reauthentication and revocation real runs, 35.0.2 | Locked key store; token revoked by the server |
| R08 | `partial` | [ipc](ipc.md): shared Go/C++ framing fixtures | Each malformed case recorded on the real pipe |
| R09 | `partial` | [ipc](ipc.md): unexpected server path refused on the real pipe | Cross-user, cross-session, remote and impersonation |
| R10 | `partial` | [ipc](ipc.md): persistence and idempotency contracts; ARM64 and x64e | Injected lost reply on the real pipe |
| R11 | `partial` | [ipc](ipc.md): the conflict is part of the contract | Run on the real pipe |
| R12 | `partial` | [ipc](ipc.md): atomic bootstrap and subscription | Concurrent-update race test |
| R13 | `partial` | [ipc](ipc.md): reconnect re-bootstraps; bounded queues | Slow client; expired event window |
| R14 | `not run` | No run is recorded | All of it |
| R15 | `not run` | [backend progress](backend-progress.md): suspend/resume is not covered | All of it |
| R16 | `not run` | [backend progress](backend-progress.md): no installer or update channel | All of it |
| R17 | `not run` | No run is recorded | All of it |
| R18 | `not run` | No run is recorded | All of it |
| R19 | `partial` | [packaging](packaging.md): ARM64 per-user install, reinstall and uninstall; startup entry, shortcut, Apps entry and install folder removed; official client registration unchanged; B14: synced files kept after account removal and uninstall, ARM64 | x64 hardware; uninstall with the account still signed in |
| R20 | `passed` | [platform notes](platform-notes.md): ARM64 real runs and B14: the installed ARM64 package (30 PE files native ARM64) signed in, synced, switched, reinstalled and uninstalled over publicly trusted TLS, 35.0.2; [packaging](packaging.md): dependency closure | Sleep and resume (§15.2 item 7) |
| R21 | `not run` | [backend progress](backend-progress.md): no 24-hour run | All of it |
| R22 | `partial` | [ipc](ipc.md): the support bundle holds only sanitized fields; B14: an exported bundle held no password, user, server, path or file name | A log scan for secrets |

## Properties (§20.5)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| P01 | `partial` | [platform notes](platform-notes.md): S19 admission races, ARM64 and x64e | Randomized event-sequence test |
| P02 | `partial` | [url usage audit](url-usage-audit.md), [engine](engine.md): origin gate in contracts and real runs | Randomized test |
| P03 | `partial` | [engine](engine.md): epoch-bound completion contracts | Randomized test |
| P04 | `partial` | [engine](engine.md): generation-bound probes and results | Randomized test |
| P05 | `partial` | [platform notes](platform-notes.md): pauses survive in contracts and real runs | Randomized test |
| P06 | `partial` | [engine](engine.md): old-epoch proposals refused | Randomized test |
| P07 | `not run` | No run is recorded | All of it |
| P08 | `partial` | [engine](engine.md): trust filters apply before scoring | Randomized test |
| P09 | `partial` | [ipc](ipc.md): durable receipts; restart persistence in real runs | Randomized test |

No randomized state-machine run, saved seed or minimal failing sequence is recorded for any
property, so none is `passed`.

## Final checklist (§23)

| Item | Status | Evidence | Missing |
|---|---|---|---|
| 23.1-1 latest upstream, dependencies, patches | `partial` | [backend progress](backend-progress.md): Desktop v35.0.3, sixteen patches | Fresh sixteen-patch build and full upstream suite |
| 23.1-2 native ARM64 engine and dependencies | `passed` | [platform notes](platform-notes.md): ARM64 real runs, PE scans; B14: full sync through the checked installed package | none on ARM64 |
| 23.1-3 sync after the UI exits | `passed` | [platform notes](platform-notes.md): daemon survives disconnect; B14: sync with the window closed and after a forced kill of the installed window | none |
| 23.1-4 identity, credential reference, journal stable | `partial` | [platform notes](platform-notes.md): journal files kept across switches and renewal | Credential reference across all switches |
| 23.1-5 scheduling and blocking semantics | `partial` | [engine](engine.md): scheduler contracts | Fairness at scale; two accounts |
| 23.1-6 origin, namespace, TLS, redirects | `partial` | [platform notes](platform-notes.md): N02 passed | N04, N05, N06 |
| 23.1-7 scoring, debounce, circuit, reasons | `partial` | [engine](engine.md): deterministic contracts | Interface display; real runs |
| 23.1-8 switch state machine under fault injection | `partial` | [platform notes](platform-notes.md): several real fault runs | Every phase; uncertain MOVE/DELETE |
| 23.1-9 conflicts, deletion protection, root identity | `partial` | [platform notes](platform-notes.md): conflicts and deletion veto real | S12–S15, S18, S21 |
| 23.1-10 error classes, bounded retries | `partial` | [engine](engine.md): classification and backoff | Real 429, 404, maintenance |
| 23.2-1 schema, methods, events, errors, version | `partial` | [ipc](ipc.md): protocol 1.8 is unreleased | Freeze; R14 |
| 23.2-2 real Go/C++ Named Pipe | `passed` | [platform notes](platform-notes.md): real pipe tests; ARM64 and x64e | none |
| 23.2-3 idempotent commands after reconnect | `partial` | [ipc](ipc.md): contracts | R10, R11 on the real pipe |
| 23.2-4 snapshot and event races, backpressure | `partial` | [ipc](ipc.md): design and contracts | R12, R13 |
| 23.2-5 frontend holds no secrets or database | `partial` | [gui](gui.md): the interface goes through IPC only | A scan of the connected interface |
| 23.2-6 consent, deletion confirm, reauth completable | `partial` | [ipc](ipc.md): real backend runs; B14 ([platform notes](platform-notes.md)): sign-in, address approval, deletion review and conflict completed in the installed interface | Reauthentication through the interface |
| 23.3-1 naming and icon | `partial` | [packaging](packaging.md): installer, shortcut and Apps entry named Nextcloud-MyGo; executable icon from the upstream icon | Trademark check; tray and window icon |
| 23.3-2 package architectures and dependencies | `partial` | [packaging](packaging.md): both packages checked; B14: the ARM64 package checked and run | x64 package on x64 hardware |
| 23.3-3 install, upgrade, downgrade, uninstall, coexistence | `partial` | [packaging](packaging.md): ARM64 install, same-version upgrade, uninstall and coexistence; downgrade refusal unit-tested; B14: same-version reinstall over a running, signed-in copy kept account and folder | x64; upgrade between versions; `daemon.prepareUpdate` |
| 23.3-4 matrix, content checks, 24-hour report | `not run` | This document | All of it |
| 23.3-5 auto-update only with a trusted chain | `not possible on this hardware` | [gui](gui.md): updates stay off in unsigned builds | A trusted signing chain |
| 23.3-6 §17.1 desktop features | `partial` | [platform notes](platform-notes.md): B14: tray icon and flyout, close to tray, Start menu reopen, Quit and stop syncing | `daemon.prepareUpdate`; notifications; start at sign-in; sleep and resume |
| 23.3-7 licenses, source, limits, diagnostics notes | `partial` | [platform notes](platform-notes.md): known limitations | User guide; release source materials |
| 23.3-8 no unverified capability listed as supported | `partial` | [backend progress](backend-progress.md): `fullSync` and `endpointAutoSwitch` stay `false` | Release list |

## Milestone exits (§18)

| Milestone | Status | Evidence | Missing |
|---|---|---|---|
| M0 feasibility and audit | `partial` | [platform notes](platform-notes.md): real basic sync on native ARM64, 35.0.2; [backend progress](backend-progress.md) calls M0 incomplete | Remaining audits; CFAPI and File Provider audit; native x64 |
| M1 contracts, accounts, single endpoint | `partial` | [ipc](ipc.md), [accounts](accounts.md): two-folder sync, login, no secrets in IPC | Whole exit not signed off |
| M2 reliability and file safety | `partial` | [engine](engine.md): scheduler, root lease, deletion veto | Core §20.2 cases not passing |
| M3 endpoint registry, read-only policy | `partial` | [ipc](ipc.md): consent, namespace check, policy | N04, N06, N07 |
| M4 safe automatic switching | `partial` | [platform notes](platform-notes.md): manual and failover switching real; automatic capability `false` | VPN; uncertain writes |
| M5 frontend, first release | `partial` | [platform notes](platform-notes.md): B14 joint acceptance on ARM64, 35.0.2, six of nine steps passed | Sleep and resume; upgrade between versions; 24-hour run; B14 findings |
| M6 cloud-file frameworks | `partial` | [on-demand](on-demand.md), [platform notes](platform-notes.md): Windows on-demand registration, hydration, free up space, always keep, switch boundary, crash recovery and unregistration on native ARM64 against server 35.0.2 through the engine runtime | Paired daemon and interface, another origin, deletion protection and offline with placeholders, uninstall unregistration; `virtualFiles` false; the macOS half is outside 0.1.0 by the user's decision of 2026-11-11 |

## Joint release acceptance (B14)

Native Windows 11 ARM64, installed package, real window driven by UI Automation, Nextcloud 35.0.2,
2026-11-12; details, findings and capability evidence in [platform notes](platform-notes.md).

| Step | Status | Checked by |
|---|---|---|
| 1 install, setup, sign in, folder, first sync | `passed` | interface; the setup's Start step not exercised |
| 2 local and remote changes, Activity | `partial` | interface and state; Activity lacks local deletions and renames, times in UTC |
| 3 bulk deletion review | `passed` | interface and server |
| 4 conflict | `passed` | interface and files |
| 5 folder pause through pause all and resume all | `passed` | interface and server |
| 6 addresses and switching | `partial` | interface; fixed/automatic not offered, a switch vanished after a failed cycle |
| 7 close, reopen, quit from the tray | `passed` | interface and processes |
| 8 reinstall over the running copy | `partial` | interface and processes; `daemon.shutdown` only, no higher version, self-check `CANCELLED` |
| 9 remove the account, uninstall | `passed` | interface, registry, server token list |
| sleep and resume | `not run` | needs the user |

## Hardware and environment limits

- One machine: Windows 11 ARM64. x64 results come from emulation on it, never native x64.
- No macOS hardware: the M6 macOS File Provider cannot be proven here, and it is not part of 0.1.0.
- No trusted signing chain: signed updates, signed packages and notarization cannot be proven.
- One server version (35.0.2): no previous-major server, second account or interactive
  browser, SSO or MFA login.
- The fault fixtures reach the server through an owned local relay; no real VPN path is recorded.
- Not run for lack of a fixture or a package, not hardware: suspend/resume, 24-hour runs,
  files above 4 GiB, and clang-tidy.
