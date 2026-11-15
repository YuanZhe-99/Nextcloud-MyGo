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
| S04 | `passed` | [platform notes](platform-notes.md): `file-safety` renames and directory moves both ways on the real server, server file IDs kept, no content sent again, no deletion review; isolated server ARM64 1 min 6 s, x64e 59 s, shared server both; in every ARM64 server tier since; B14: a local rename in the installed interface; `data-safety -Case rename-cut`: a local rename whose MOVE the relay refused kept the user's name, and a later cycle made the MOVE (same server file ID and content, same local file, nothing in the trash), patch 0019; ARM64, 35.0.2, in the server tier (13.5 s) and three times alone with an earlier revision of the patch (12.8–16.1 s); failed without the patch (the old name came back) | x64e did not run `rename-cut` |
| S05 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `data-safety -Case interrupted-change`: a local and a remote modification of existing files, both cut mid-transfer with both addresses refused; the server kept the previous version (the modification is sent as a chunked upload, patch 0018: Nextcloud 35.0.2 writes a replacing PUT over the file in place), the local side kept whole versions, no conflict copy, and both converged on the same address once the network came back; three runs, ARM64, 35.0.2; earlier relay cuts of new files recover with matching hashes (`network-active`) | A server without chunking v2 still receives one PUT per modification; x64e did not run the case |
| S06 | `passed` | [platform notes](platform-notes.md): `network-chunked` 112 MiB upload cut in its second chunk resumes without resending the first (ARM64 2 min 55 s, x64e 2 min 58 s); `network-large` 4,296 MiB chunk session cut after 4 GiB resumes with matching streamed hashes (ARM64 28 min 35 s); `network-active` download cut and recovered; upstream `ChunkingNgTest` 22/22 | `network-large` on x64e; a download above 4 GiB |
| S07 | `passed` | [platform notes](platform-notes.md): a server-committed PUT and a chunk session's final MOVE with their replies withheld are recovered by rediscovery, hashes, server file ID, ETag and original journal match; ARM64 and x64e | Failure-stage injection beyond the reply |
| S08 | `passed` | [platform notes](platform-notes.md): `network-uncertain` MOVE and DELETE of existing entries committed with the reply lost, path created again by another client, recovered without a second MOVE or DELETE, re-created object untouched (same file ID and ETag); ARM64 12 min 31 s, x64e 10 min 27 s; in every ARM64 server tier since | none |
| S09 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `watcher`: a change written after discovery while an upload was held started the next cycle by itself, 3 of 3 within 20 s (poll every 48–72 s); ARM64, 35.0.2 | none |
| S10 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `watcher`: a real watch-buffer overflow (300 long names, a rename and a modification while the control thread was busy, proved by a probe watcher of the same size) started an audit that found every change, 2 of 2 within 30 s; ARM64, 35.0.2 | none |
| S11 | `passed` | [platform notes](platform-notes.md): `file-safety` holds one directory with twelve descendants as 13 remote deletions (ratio threshold) before propagation, applied only after approval, ARM64 and x64e; B14: 25 of 30 reviewed in the interface; threshold arithmetic in `SyncDDeletionContracts`; `file-safety-bulk` ([platform notes](platform-notes.md#acceptance-audit-for-010)): another client deletes 100 of 510 files, held by the absolute threshold alone as 100 local deletions, no local file removed before approval, applied after it; ARM64 server tier five times on 2026-11-13 (61.0–69.8 s), against 35.0.2 and 34.0.5 | x64e did not run `file-safety-bulk` |
| S12 | `passed` | [platform notes](platform-notes.md): `file-safety` removes one directory with twelve descendants locally: proposed as 13 remote deletions, not one, nothing removed before the decision; ARM64 and x64e, isolated and shared server | none |
| S13 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `root-denied`: a mapped root the user may not list is `UNSUPPORTED_FOLDER_MODE`, nothing deleted on the server, no deletion proposed; the block stays until a restart; ARM64, 35.0.2 | none |
| S14 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `maintenance`: maintenance mode (503) is `NETWORK_UNAVAILABLE`, a local deletion held back, converged afterwards; `file-safety` 404 root; `partial-listing`: a `files_external` storage whose directory vanished (left out of the listing, or listed empty) is `DISCOVERY_INCOMPLETE` with its local copies kept and no deletion proposed, three runs; ARM64, 35.0.2 | A storage really emptied or unmounted on the server stays held until its local copies are removed |
| S15 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `root-replaced`: a root replaced by an empty folder while stopped is `FOLDER_ROOT_CHANGED`, nothing propagated, both folders untouched; a `DefineDosDevice` drive alias refused as a root; ARM64, 35.0.2 | Drive-letter reuse by another volume is a [limitation](#known-limitations-for-010) |
| S16 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `journal`: a journal with a damaged header after a restart is `JOURNAL_ERROR`, kept byte for byte, no local or remote change propagated; upstream commit-failure regressions (patches 0011, 0013); ARM64, 35.0.2 | none |
| S17 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `names`: a case clash, three names Windows cannot hold, NFD and non-BMP Unicode, paths above 300 characters both ways and a locked file: every other file synced, no server file changed, the locked copy kept until released, no deletion proposed; ARM64, 35.0.2. Found and fixed: the pre-propagation scan refused paths above `MAX_PATH` | none |
| S18 | `partial` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `excluded`: a local tree with a junction and an offline file is blocked before its first propagation as `UNSUPPORTED_FOLDER_MODE`, nothing sent; ARM64, 35.0.2 | An E2EE folder and a genuine Cloud Files placeholder: a [limitation](#known-limitations-for-010) |
| S19 | `partial` | [platform notes](platform-notes.md): hierarchical root admission, 3 repetitions / 144 races; ARM64 18.27 s, x64e 26.27 s | Non-NTFS refusal, SUBST, crash during publication; a separate session is a [limitation](#known-limitations-for-010) |
| S20 | `passed` | [platform notes](platform-notes.md): `file-safety` approves a deletion after one more local deletion on the real server: the stale approval removes nothing, the changed set is proposed again and applied only after its own approval; ARM64 and x64e; old-epoch proposals refused in contracts | none |
| S21 | `partial` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `excluded`: a junction appearing in a synced mapping blocks it before propagation, nothing sent, downloaded or deleted either way; ARM64, 35.0.2 | An E2EE subfolder and a genuine placeholder: a [limitation](#known-limitations-for-010) |

## Network, trust and switching (§20.3)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| N01 | `passed` | [platform notes](platform-notes.md): two addresses of one server registered; ARM64 and x64e, 35.0.2; B14 step 6: the alternate added in the installed interface with certificate and server shown before any credential, the answer times of both, and every switch with its reason; scoring in deterministic contracts | none |
| N02 | `passed` | [platform notes](platform-notes.md): two real isolated instances, same UID, distinct namespace, refused and quarantined; ARM64 and x64e, 35.0.2 | none |
| N03 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `tls-trust`, an unregistered address reachable through the test proxy got its public check only, no credential and no further request while both approved addresses were down; nothing reads an SSID, so a same-name network gives nothing to trust; ARM64 and x64e, 35.0.2 | none |
| N04 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `tls-trust`, expired, wrong-host and unknown-CA certificates refused at registration and, on the approved alternate, quarantined (`TLS_ERROR`), never used while the primary was down, released only by an explicit review; ARM64 and x64e, 35.0.2 | none |
| N05 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `redirect`, the address in use answering 302 to an unknown domain and to plain HTTP: nothing followed, no request reached either target, the address quarantined `ENDPOINT_UNTRUSTED`; login and metadata clients never follow redirects ([url usage audit](url-usage-audit.md)); ARM64 and x64e, 35.0.2 | none |
| N06 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `subpath`, an entry under `/nc/` verified and used: upload, rename (MOVE Destination) and download (PROPFIND hrefs) reached the same server paths; ARM64 and x64e, 35.0.2 | none |
| N07 | `limitation` | [engine](engine.md): strict VPN egress is not advertised | Not claimed for 0.1.0 |
| N08 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `auto-switch`, 20 interface changes in 8 s moved nothing; `SyncDSwitchingContracts`: results of an older generation refused, 3-second flaps for 5 minutes made no switch; ARM64 and x64e, 35.0.2 | none |
| N09 | `passed` | `SyncDSwitchingContracts`: margin 12.15 switches and 11.7 does not, 1 ms short of dwell, 14.999 s of stability, 15.001 s old evidence, the dwell after a switch (which covers the 60 s cooldown); `auto-switch`: the home address (priority 90 against 50) taken with reason `faster` from the daemon's own probes; ARM64 and x64e | none |
| N10 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `tls-circuit`, the circuit opened after 12 refused connections forming 3 independent failures over 10.5 s; contracts: one request's callbacks and a burst inside 5 s count once; ARM64 and x64e, 35.0.2 | none |
| N11 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `auto-switch`, a better home address waited 32 s, past the 30 s drain window, for a held 12 MiB upload without cancelling it, then switched with one new epoch and unchanged journals; ARM64 and x64e, 35.0.2 | none |
| N12 | `passed` | [platform notes](platform-notes.md): interrupted transfers fail over to the other origin after drainage with a new epoch; ARM64 and x64e, 35.0.2 | none |
| N13 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `late-epoch`, answers 6 s late on the old address: the switch committed only after every old-epoch DAV request was answered; late probe answers left the new address, binding and progress unchanged; ARM64 and x64e, 35.0.2 | none |
| N14 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `auto-fallback`, the better address verified, committed and failed its first business request: the previous address was verified again and used with a further epoch, journals and credential unchanged; ARM64 and x64e, 35.0.2 | none |
| N15 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `tls-circuit`, both addresses refused for 45 s with three network notifications: no switch, 5 and 3 probes; `network` (all-bad waiting, no ping-pong); contracts: 30 minutes down, never a proposal, probing bounded by the circuit; ARM64 and x64e, 35.0.2 | none |
| N16 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `tls-auth`, 401 from the alternate only: that address blocked `AUTH_REQUIRED`, the account signed in, the credential kept and used on the primary, no switch to it; ARM64 and x64e, 35.0.2 | none |
| N17 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `revoke-switch`, the server revoked the app password while a switch waited for a held upload: the switch failed `AUTH_REQUIRED`, the account shows Sign in, the binding stayed and no other address was tried for 15 s; ARM64 and x64e, 35.0.2 | none |
| N18 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `tls-auth`, 429 with Retry-After 20 on the address in use: no request to either address during it, no switch, the upload resumed on the same address; ARM64 and x64e, 35.0.2 | none |
| N19 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `new-folder`, an alternate that could not check a new folder's root took no automatic switch with the dwell aged and refused a manual one until it verified it; ARM64 and x64e, 35.0.2 | none |
| N20 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `root-change`, the mapped remote root replaced by a new one of the same path: writes stopped (`ENDPOINT_IDENTITY_MISMATCH`), local files kept, a network change did not resume them; ARM64 and x64e, 35.0.2 | none |
| N21 | `passed` | [platform notes](platform-notes.md): primary/alternate/primary switches keep the native journal files, unchanged-cycle request checks; ARM64 and x64e, 35.0.2 | none |
| N22 | `passed` | [platform notes](platform-notes.md#automatic-switching-acceptance): `tls-circuit`, 30 network notifications in 3 s against an open circuit: one half-open probe in 18 s; contracts: one probe in flight and early half-open probes at least 10 s apart; ARM64 and x64e, 35.0.2 | none |
| N23 | `limitation` | [engine](engine.md): strict VPN egress is not advertised | Not claimed for 0.1.0 |

## Lifecycle, IPC and upgrade (§20.4)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| R01 | `passed` | [platform notes](platform-notes.md): the daemon survives a client disconnect, real Go↔C++ pipe, ARM64 and x64e; B14: the installed window force-killed, SyncD kept running, the new window showed the account; ARM64, 35.0.2 | x64e of the forced kill |
| R02 | `partial` | [platform notes](platform-notes.md): kills during unfinished uploads and downloads; [platform notes](platform-notes.md#data-safety-and-recovery-cases): `discovery-kill`, a kill while the cycle was in remote discovery, the restart finished both waiting changes once on the same journal; ARM64 | The finish phase; x64e |
| R03 | `passed` | [platform notes](platform-notes.md): `switch-kill` ends the process at each durable switch stage (`prepared`, `quiescing`, `verifying`, `committed`, `reconciling`) and restarts it on the same profile: previous binding for the first three, committed for the last two, fresh epoch, same credential reference, folder rows and journal, nothing reconciled sent again; ARM64 and x64e, isolated and shared server; in every server tier since | none |
| R04 | `passed` | [platform notes](platform-notes.md): pauses survive switching and recovery in real daemon runs; ARM64 and x64e, 35.0.2 | none |
| R05 | `partial` | [platform notes](platform-notes.md): `multi-account`, two accounts with two folders each on one daemon, content both ways in all four, one account switched while the other kept syncing unpaused; server tier ARM64; serial scheduler contracts ([engine](engine.md)) | A real run that asserts one engine cycle at a time under sustained changes |
| R06 | `limitation` | [platform notes](platform-notes.md): separate-session checks are unrun | A second Windows logon session |
| R07 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `revocation`: an app password revoked on the server (`DELETE ocs/v2.php/core/apppassword`) puts the account into `revoked` and its folder `AUTH_REQUIRED`, nothing sent after it, no secret in any profile file; [accounts](accounts.md): reauthentication; ARM64, 35.0.2 | A locked key store is a [limitation](#known-limitations-for-010) |
| R08 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeFramingR08R12` on the real pipe: a frame split inside its prefix and body, two frames in one write, and six malformed frames (zero length, above 1 MiB, invalid UTF-8, duplicate key, truncated JSON, nesting above 64) each end only that connection, the next client is served with the state kept, a never-completed half frame is dropped after 15.0 s; ARM64 16.2 s, x64e 17.1 s | none |
| R09 | `partial` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeImpostorR09`: a process holding the pipe name first is refused by the client (wrong executable) and keeps the daemon from starting under that name; a client through `\\127.0.0.1\pipe\` is refused as remote (access denied); unexpected server path refused (`TestRealPipeINV12INV13`); ARM64 and x64e | Cross-user and cross-session: a [limitation](#known-limitations-for-010) |
| R10 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeReplayR10R11R14`: a pause whose reply was never read committed once; the same key, sent again after reconnecting, after a later resume and after a daemon restart, returns the same receipt and never pauses again; ARM64 1.2 s, x64e 2.3 s | none |
| R11 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): the same key with other input returns `OPERATION_KEY_CONFLICT` and the state and revision stay unchanged, real pipe; ARM64 and x64e | none |
| R12 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `TestRealPipeConcurrentSourceR12`: bootstraps during the daemon's own crash-backoff activation, the activation arrives as an event with a later `seq`, seq never goes back, a pause between two bootstraps of one batch is in an event; ARM64 2.5 s; `TestRealPipeFramingR08R12` | none |
| R13 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeSlowClientR13`: a client that never reads is dropped on the bounded output queue (ARM64 after 2.3 s, x64e 8.6 s), the next client is served within 5 s, `events.resume` answers `RESYNC_REQUIRED`; restart re-bootstraps (`TestRealPipeINV12INV13`) | none |
| R14 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealPipeReplayR10R11R14`: `session.hello` with major 2 returns `PROTOCOL_MISMATCH`, a mutation and a bootstrap before negotiation are refused the same way and execute nothing; ARM64 and x64e | none |
| R15 | `passed (simulated)` | [platform notes](platform-notes.md): `sleep-resume` ([development](development.md)), the whole daemon process suspended 35 s during a held upload and woken where only the alternate answers: dead connection failed, nothing read as deleted, switch at an idle folder with reason `unreachable`, upload completed once, journal and folder identity kept; ARM64, 35.0.2. [Engine](engine.md): resume notification on an injected clock; UTC rollback keeps throttle deadlines (`syncd_constraint_tests`). A later run on an idle machine failed at "switch did not wait for the folder to go idle" and is under investigation in another session | A real sleep: power transitions and their notifications reaching the running daemon, whether the monotonic clock counts the sleep, adapter and address changes, connections that die without a reset, hours-long sleeps, volume changes while asleep, Modern Standby; x64e |
| R16 | `partial` | [packaging](packaging.md#signed-updates): a refused download (wrong key, changed archive or manifest, other architecture, not newer) and a connection broken halfway through the archive (`TestUpdateInterruptedDownload`) leave nothing staged and start no installer (unit tests); the in-app path installed a signed 0.1.1 over a running 0.1.0 through setup and `daemon.prepareUpdate`, profile state kept, ARM64 | A failing setup in the in-app path on a real install; the installed interface starting it |
| R17 | `partial` | [platform notes](platform-notes.md#acceptance-audit-for-010): `TestRealNewerControlSchemaR17`: the compiled daemon given a Control database one schema newer serves only `databaseUnavailable`, refuses mutations and leaves the file byte for byte (ARM64 2.3 s, x64e 2.6 s); migration backups refuse a newer schema (`SyncDStartupContracts`); the installer refuses a downgrade (unit tests) | A sync journal upgraded by a newer Desktop release opened by an older one |
| R18 | `limitation` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): SyncD keeps no journal backup and offers no restore; migration backups hold only the Control database and refuse a newer schema; the installer refuses a downgrade | A journal restored by hand is not detected: a [limitation](#known-limitations-for-010) |
| R19 | `partial` | [packaging](packaging.md): ARM64 per-user install, reinstall and uninstall; startup entry, shortcut, Apps entry and install folder removed; official client registration unchanged; B14: synced files kept after account removal and uninstall, ARM64 | Uninstall with the account still signed in; the x64 package is a [limitation](#known-limitations-for-010) |
| R20 | `passed` | [platform notes](platform-notes.md): ARM64 real runs and B14: the installed ARM64 package (30 PE files native ARM64) signed in, synced, switched, reinstalled and uninstalled over publicly trusted TLS, 35.0.2; [packaging](packaging.md): dependency closure; sleep and resume (§15.2 item 7) passed by simulation only (R15) | A real sleep and resume (§15.2 item 7) |
| R21 | `partial` | [platform notes](platform-notes.md#accelerated-soak-r21): accelerated 1-hour soak, not wall-clock 24 h (the user's decision), ARM64, 35.0.2: 89 cycles, about 3,600 file changes, 12 chunked uploads, 54 address faults and 16 flap rounds, 69 switches with reasons and none across a running cycle, two renewals, a restart, UI pipe connections; slopes within bounds (private bytes +4.8 MB/h, handles −17/h, threads −1/h, Control database +40 KB/h); hashes matched every cycle, no duplicate upload or unexpected deletion. Fixed: unbounded outbox. Failed: three local renames undone after a failed MOVE (upstream). Rerun twice with patch 0019: no rename undone in either (332 and 352 renames); the first failed only the TCP band (65 of 60), the second passed every check, slope and band | Midnight rollover, real token expiry, real sleep, leaks below the measured bounds, x64 |
| R22 | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): `revocation`: no password, app password or fixture password in any profile file; no user, server, local path, file name or content in the support bundle or the daemon's log; B14 bundle check; ARM64, 35.0.2 | none |

## Properties (§20.5)

| Case | Status | Evidence | Missing |
|---|---|---|---|
| P01 | `passed` | `SyncDSwitchingContracts`, 500 seeded sequences of 300 random admission events: never a second engine cycle, so never a second journal writer; S19 admission races; ARM64 9.1 s, x64e 10.9 s (whole test) | none |
| P02 | `passed` | `SyncDSwitchingContracts`, 20,000 URLs from hostile parts: every one the request gate admits is HTTPS on the approved host and port inside the installation path; real runs never sent credentials to an unapproved origin (N03, N05); ARM64 and x64e | none |
| P03 | `passed` | `SyncDSwitchingContracts`, 500 seeded switch sequences: a cycle ends at the epoch it began at, the epoch moves by one only after every frozen folder proved it quiet; ARM64 and x64e | none |
| P04 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): `SyncDPropertyContracts`, 500 seeded sequences of 400 random policy events: every ticket from an older network generation, an older configuration, a hard failure or a review, or one already consumed, is refused and changes no health field; ARM64 0.18 s, x64e 0.40 s | none |
| P05 | `passed` | `SyncDSwitchingContracts`, the same admission run: a user-paused folder is never admitted whatever recovery, retry, throttle or unfreeze event arrives; ARM64 and x64e | none |
| P06 | `passed` | `SyncDSwitchingContracts`, the same switch run: a request or proof naming another epoch, an untrusted target and an older epoch's reconciliation are refused; ARM64 and x64e | none |
| P07 | `not run` | No run is recorded | All of it |
| P08 | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): the same run: no automatic or fixed proposal ever names an endpoint that is disabled, unapproved, unverified, without valid TLS, not allowed on the network, quarantined or blocked for authentication, and a fixed target never falls back to another one; a deliberately wrong model failed with seed 1 and shrank to two steps | none |
| P09 | `passed` | `SyncDSwitchingContracts`, 20 seeded sequences of 60 steps (40 until 2026-11-13; the CI x64 runner exceeded its 60 s test limit) of mutations, replays, conflicts, stale revisions and restarts on a real Control database: every accepted receipt returned unchanged after restarts; ARM64 and x64e | none |

The property run prints its seed and the minimal failing sequence on a failure;
`SYNCD_PROPERTY_SEED` replays one seed.

## Final checklist (§23)

| Item | Status | Evidence | Missing |
|---|---|---|---|
| 23.1-1 latest upstream, dependencies, patches | `passed` | [platform notes](platform-notes.md), [backend progress](backend-progress.md): Desktop v35.0.3, the latest release (checked 2026-11-12), seventeen recorded patches in `build-lock.json`; the upstream suite with them 95/96 on ARM64 (the failing target passed 3/3 alone) and 96/96 on x64e, `SyncCfApiTest` 50 passed, 2 skipped; for 0.1.1 twenty-three patches; the 96/96 on both and `SyncCfApiTest` 53 passed, 2 skipped were measured on the 21-patch build (0001–0021), and the final run on the 23-patch build is recorded under [further evidence](#further-evidence) | none |
| 23.1-2 native ARM64 engine and dependencies | `passed` | [platform notes](platform-notes.md): ARM64 real runs, PE scans; B14: full sync through the checked installed package | none on ARM64 |
| 23.1-3 sync after the UI exits | `passed` | [platform notes](platform-notes.md): daemon survives disconnect; B14: sync with the window closed and after a forced kill of the installed window | none |
| 23.1-4 identity, credential reference, journal stable | `passed` | [platform notes](platform-notes.md): account, credential reference, folder rows and native journal file checked unchanged across every `switch-kill` stage, failover, simulated sleep and renewal, ARM64 and x64e (N21, R03) | none |
| 23.1-5 scheduling and blocking semantics | `partial` | [engine](engine.md): scheduler contracts; two accounts with two folders each real (R05) | Fairness at scale |
| 23.1-6 origin, namespace, TLS, redirects | `partial` | [platform notes](platform-notes.md): N02 passed | N04, N05, N06 |
| 23.1-7 scoring, debounce, circuit, reasons | `partial` | [engine](engine.md): deterministic contracts; P04 and P08 passed; switch reasons shown in the interface (B14) | Scoring and debounce driving real switches (automatic switching is off) |
| 23.1-8 switch state machine under fault injection | `passed` | [platform notes](platform-notes.md): kills at all five durable switch stages (R03), lost replies of committed PUT, MOVE, DELETE and final MOVE (S07, S08), interrupted transfers and failover (N12), all real | none |
| 23.1-9 conflicts, deletion protection, root identity | `partial` | [platform notes](platform-notes.md): conflicts, deletion review at the ratio threshold, descendants and stale approvals real (S03, S12, S20) | S11 at 100 objects, S13, S15, S18, S21 |
| 23.1-10 error classes, bounded retries | `partial` | [engine](engine.md): classification and backoff; real 404 and full quota | Real 429, maintenance |
| 23.2-1 schema, methods, events, errors, version | `partial` | [ipc](ipc.md): protocol 1.10, version negotiation refused across majors on the real pipe (R14) | The release that freezes protocol 1.10 |
| 23.2-2 real Go/C++ Named Pipe | `passed` | [platform notes](platform-notes.md): real pipe tests; ARM64 and x64e | none |
| 23.2-3 idempotent commands after reconnect | `passed` | [platform notes](platform-notes.md#acceptance-audit-for-010): R10 and R11 on the real pipe, receipts across reconnect and restart | none |
| 23.2-4 snapshot and event races, backpressure | `passed` | [platform notes](platform-notes.md#data-safety-and-recovery-cases): R12 and R13 on the real pipe | none |
| 23.2-5 frontend holds no secrets or database | `partial` | [gui](gui.md): the interface goes through IPC only | A scan of the connected interface |
| 23.2-6 consent, deletion confirm, reauth completable | `partial` | [ipc](ipc.md): real backend runs; B14 ([platform notes](platform-notes.md)): sign-in, address approval, deletion review and conflict completed in the installed interface | Reauthentication through the interface |
| 23.3-1 naming and icon | `partial` | [packaging](packaging.md#coexistence-with-the-official-client): the display name is **Nextcloud MyGo** (Start menu shortcut, Apps entry, window title, interface text), identifiers stay `Nextcloud-MyGo` (folders, keys, executables, pipe names); executable icon from the upstream icon; B14: the tray icon shown | Trademark check |
| 23.3-2 package architectures and dependencies | `partial` | [packaging](packaging.md): both packages checked (ARM64 28, x64 29 PE files, native); B14: the ARM64 package checked and run | Installing the x64 package: a [limitation](#known-limitations-for-010) |
| 23.3-3 install, upgrade, downgrade, uninstall, coexistence | `partial` | [packaging](packaging.md): ARM64 install, same-version upgrade, uninstall and coexistence; downgrade refusal unit-tested; B14: same-version reinstall over a running, signed-in copy kept account and folder; package smoke: 0.1.0 → 0.1.1 test-version upgrade over a running SyncD through `daemon.prepareUpdate`, profile state and startup report (previous version, schema, self-check passed) checked, ARM64 | An upgrade between versions with a signed-in account; x64 is a [limitation](#known-limitations-for-010) |
| 23.3-4 matrix, content checks, 24-hour report | `partial` | This document; the 24-hour report is the accelerated 1-hour soak's (`build/soak/summary.txt`, R21), not a wall-clock 24 h | What an accelerated hour cannot show (R21) |
| 23.3-5 auto-update only with a trusted chain | `partial` | [packaging](packaging.md#signed-updates): updates exist only in packages built with the `mygo keygen` key, kept in a GitHub secret; the app installs only archives signed with the key in `mygo.json`, off until the user turns it on; package smoke: a signed 0.1.1 test update from loopback HTTP installed over 0.1.0 with a throwaway key, ARM64 | A release signed with the project key, read from GitHub; Authenticode is a [limitation](#known-limitations-for-010) |
| 23.3-6 §17.1 desktop features | `partial` | [platform notes](platform-notes.md): B14: tray icon and flyout, close to tray, Start menu reopen, Quit and stop syncing; setup reaches `daemon.prepareUpdate` before stopping SyncD (package smoke) | `daemon.prepareUpdate` refusals with a real deletion waiting; notifications; start at sign-in; a real sleep is a [limitation](#known-limitations-for-010) |
| 23.3-7 licenses, source, limits, diagnostics notes | `partial` | [platform notes](platform-notes.md): known limitations; this page's limitation list | User guide; release source materials |
| 23.3-8 no unverified capability listed as supported | `partial` | [Capability evidence](#capability-evidence): `endpointAutoSwitch` and `fullSync` are `true` with their evidence; `virtualFiles` is `true` in 0.1.1 by the user's decision of 2026-11-14 on native ARM64 evidence ([0.1.1 on-demand evidence](#011-on-demand-evidence)); the 0.1.0 summary in the [version history](version-history.md) offered no on-demand files | `virtualFiles` on x64, after an upgrade from 0.1.0 and where Cloud Files is unavailable |

## Milestone exits (§18)

| Milestone | Status | Evidence | Missing |
|---|---|---|---|
| M0 feasibility and audit | `partial` | [platform notes](platform-notes.md): real basic sync on native ARM64, 35.0.2; [backend progress](backend-progress.md) calls M0 incomplete | Remaining audits; the macOS File Provider audit is outside 0.1.0; native x64 is a [limitation](#known-limitations-for-010) |
| M1 contracts, accounts, single endpoint | `partial` | [ipc](ipc.md), [accounts](accounts.md): two-folder sync, login, no secrets in IPC; R08, R10, R11, R13, R14 on the real pipe | Whole exit not signed off |
| M2 reliability and file safety | `partial` | [engine](engine.md): scheduler, root lease, deletion veto; S04–S10, S12–S17, S20 passed | S11, S18, S21 |
| M3 endpoint registry, read-only policy | `partial` | [ipc](ipc.md): consent, namespace check, policy; N01, N02 passed | N04, N06; N07 is a [limitation](#known-limitations-for-010) |
| M4 safe automatic switching | `partial` | [platform notes](platform-notes.md): manual and failover switching real, uncertain writes recovered (S07, S08), every switch stage killed and recovered (R03); automatic capability `false` | Automatic switching evidence ([Capability evidence](#capability-evidence)); VPN is a [limitation](#known-limitations-for-010) |
| M5 frontend, first release | `partial` | [platform notes](platform-notes.md): B14 joint acceptance on ARM64, 35.0.2, six of nine steps passed; the B14 findings fixed in SyncD and setup | Steps 6 and 8 again through the interface; upgrade between versions with an account |
| M6 cloud-file frameworks | `partial` | [on-demand](on-demand.md), [platform notes](platform-notes.md): Windows on-demand registration, hydration, free up space, always keep, switch boundary, crash recovery and unregistration on native ARM64 and x64e against server 35.0.2 through the engine runtime; hosts where Cloud Files cannot answer skip instead of passing | Another origin, offline with placeholders, the shipped installers against a server and the uninstall with a real on-demand folder; the 0.1.1 enablement and conversion evidence is under [0.1.1 on-demand evidence](#011-on-demand-evidence); the macOS half is outside 0.1.0 by the user's decision of 2026-11-11 |

## Joint release acceptance (B14)

Native Windows 11 ARM64, installed package, real window driven by UI Automation, Nextcloud 35.0.2,
2026-11-12; details, findings and capability evidence in [platform notes](platform-notes.md).

| Step | Status | Checked by |
|---|---|---|
| 1 install, setup, sign in, folder, first sync | `passed` | interface; the setup's Start step not exercised |
| 2 local and remote changes, Activity | `passed` | interface, WebDAV and local files, again on 2026-11-13 through the installed package ([platform notes](platform-notes.md#activity-through-the-installed-interface)): a local rename and deletion and a server rename and deletion each listed in Activity with its direction and local time |
| 3 bulk deletion review | `passed` | interface and server |
| 4 conflict | `passed` | interface and files |
| 5 folder pause through pause all and resume all | `passed` | interface and server |
| 6 addresses and switching | `partial` | interface; fixed/automatic not offered; a switch held past its drain window now ends as `switchFailed` (`switch-quota`, ARM64, 35.0.2), not run again in the interface |
| 7 close, reopen, quit from the tray | `passed` | interface and processes |
| 8 reinstall over the running copy | `partial` | interface and processes; since then setup calls `daemon.prepareUpdate`, the self-check no longer fails on `CANCELLED` (`SyncDFolderErrorContracts`) and a 0.1.0 → 0.1.1 test upgrade passed in the package smoke; not run again in the interface |
| 9 remove the account, uninstall | `passed` | interface, registry, server token list |
| sleep and resume | `passed (simulated)` | `sleep-resume` against the backend, not through the interface or the installed package (R15); a real sleep was ruled out for this machine by the user |

## Capability evidence

Only the user decides to enable a capability. For engine builds `endpointAutoSwitch` is `true` since
2026-11-12 and `fullSync` since 2026-11-13, each by the user's decision once the cases below passed
on real runs; `virtualFiles` is `true` since 0.1.1 (2026-11-14) where Cloud Files runs. What exists
and what is missing for each:

| Capability | Evidence | Missing to enable it |
|---|---|---|
| `fullSync` (`true`) | S01–S08, S11, S12, S20 passed on a real server (ARM64 and x64e for most); conflicts, deletion review, pause and resume through the installed interface (B14); a real cycle's local and server renames and deletions in Activity (B14 step 2, 2026-11-13); two accounts with two folders each (R05 partial); the daemon survives the UI (R01, 23.1-3); the accelerated soak with no rename undone (R21) | E2EE folders (S18, S21), a [limitation](#known-limitations-for-010) |
| `endpointAutoSwitch` (`true`) | Manual and fixed switching real, deferred to a safe point (N11, B14 step 6); failover with drainage and a fresh epoch (N12); every switch stage killed and recovered (R03); uncertain writes recovered (S07, S08); the automatic policy on real runs: N03–N06, N08–N11, N13–N20 and N22 ([platform notes](platform-notes.md#automatic-switching-acceptance)); P01–P06, P08 and P09; the Network page offers Automatic and Fixed exactly when the capability is true (`TestAutomaticChoiceFollowsCapability`, a native screenshot against the Mock) | Strict VPN egress (N07, N23, not advertised); a real sleep (R15, simulated only); the automatic choice through the installed interface against a real server |
| `virtualFiles` (`true` since 0.1.1) | Windows on-demand folders through the engine runtime on a real server: placeholders, hydration, free up space, always keep, a switch waiting for a hydration, a kill during a hydration and unregistration, ARM64 and x64e (M6); on ARM64 and server 35.0.3 (2026-11-14) also the conversion of a full-file folder and the real pipe at protocol 1.11 ([0.1.1 on-demand evidence](#011-on-demand-evidence)) | the shipped installers against a server (an install over a real 0.1.0, the real uninstaller); SyncD's own navigation entry and icon in Explorer; behavior where Cloud Files is unavailable; another origin; an online-only file opened offline; the shell extension for thumbnails and overlays. Done since ([further evidence](#further-evidence)): x64e, the upgrade through a test host, local deletions, a directory move and deletion protection with placeholders, sparse files |

## 0.1.1 on-demand evidence

Native ARM64, Windows 11 build 26300, Qt 6.12.0, a disposable local server 35.0.3, 2026-11-14, on the first
0.1.1 build (before patches 0020 to 0023)
([platform notes](platform-notes.md#on-demand-files-and-the-automatic-conversion-011)):

| Evidence | Result |
|---|---|
| SyncD CTest, the full set | 24/24 passed, three runs |
| `tools/test-ondemand.ps1`, including the conversion steps | passed twice, about 85 s: a conversion pass of 26 entries in 80 ms with one file held delete-pending, the journal digest unchanged, in-sync hydrated placeholders carrying the journal's file IDs, the first on-demand cycle repairing the file left with zero downloads, free up space then re-opening returned the original content |
| `go test ./internal/syncd` against the real SyncD | passed at protocol 1.11 |
| `SyncDOnDemandContracts` on real Cloud Files | passed without `SKIP` |
| Conversion timing (local fixture of 10,000 files of 200 B, hand-written journal, not a real sync) | about 13.4 s for a complete pass, about 1.3 ms per entry |

Not verified in this first run (what was run later and what is still unverified: [further evidence](#further-evidence)):
x64; the upgrade from an installed 0.1.0; Explorer showing the navigation entry and the icon; the uninstall
with a real on-demand folder; behavior where Cloud Files is unavailable; timing at 100,000 files.

### Further evidence

Evidence recorded after the table above, with its machine, architecture and date: one Windows 11 ARM64
host (build 26300, Qt 6.12.0), 2026-11-14; x64 is emulated on the same host; disposable servers 35.0.3 and
34.0.6. Acceptance found two bugs, fixed by patches 0020 and 0021 ([on-demand](on-demand.md#local-deletions-and-sparse-files)).
Details are in [platform notes](platform-notes.md#further-011-evidence).

Build with 21 patches (patches 0001–0021, tree `1D235B1119EB`), SyncD 0.1.1, protocol 1.11, Control schema 12. Every figure in this table was measured on this build, not on the 23-patch build:

| Evidence | Result |
|---|---|
| SyncD CTest | 24/24: ARM64 40.05 s, x64 56.78 s |
| Upstream tests through CTest (`UtilityTest` and `SyncCfApiTest` run separately) | 96/96: ARM64 441.0 s, x64 504.0 s |
| `SyncCfApiTest` through `tools/test-cfapi-upstream.ps1` | 53 passed, 2 skipped, 0 failed: ARM64 92.0 s, x64 93.6 s |
| `tools/test-ondemand.ps1` | server 35.0.3: ARM64 39.7 s, x64 44.2 s; server 34.0.6: ARM64 40.0 s, x64 41.9 s. Single deletion of an online-only file reaching the server in the automatic follow-up (S12 with placeholders); a directory move as one rename, 0 uploads, 0 deletions (S04 with placeholders); 13 deletions held as `DELETION_CONFIRMATION_REQUIRED`, every file still on the server, deleted after the approval (S11 and INV-13 with placeholders); sparse files: a new file, an edit, a server change, a conflict on both sides, journal type unchanged; conversion of a 0.1.0 folder with a sparse file |
| `tools/test-all.ps1`, fast tier | ARM64 58.9 s of 120 s; x64 failed once without a named failing test, then passed four reruns (54.3–56.9 s), cause not diagnosed |
| `tools/test-all.ps1`, server tier, ARM64, server 35.0.3 | passed, 517.0 s of 600 s: the `PLAN.md` §16.5 re-check for the new patch set. The x64 server tier was not re-run on the final build |

Without the fixes: with the flag carry forced off, the on-demand run stopped at the first deletion with
`SYNC_FAILED`; upstream's sparse tests failed without patch 0021.

Earlier the same day, on the previous build (19 patches, tree `9AD005FA79FF`; packages not rebuilt on the
21-patch build):

| Evidence | Result |
|---|---|
| Packages (`tools/pkg`) | ARM64 29 PE files, all native ARM64; x64 30 PE files, all native x64; version 0.1.1, schema 12, unsigned test packages; SyncD carries an icon resource on both, and `pkg check` fails one without |
| Real pipe, `go test ./internal/syncd` | 52/52 on ARM64 and x64 |
| Upgrade 0.1.0 to 0.1.1 through test hosts (not the shipped executables) | ARM64 on 35.0.3 and 34.0.6, x64 on 35.0.3; 302 server files plus 20 uploaded by 0.1.0: backup at `user_version` 11 present, the folder `onDemand` and `converted`, 0 content GETs, 0 of 323 hashes changed, journal digest unchanged; new server file online-only and hydrated on open; free up space and re-open identical; a local edit uploaded; an offline start kept hydrated files readable |
| A folder of 5,000 files | the first start answered after about 3.3 s; 5,050 of 5,050 in sync, 0 hashes changed; a kill right after the `converting` row, then a restart: converted, journal digest and hashes unchanged, 0 content GETs |
| Folder removal over the pipe | hydrated files stay with identical content, online-only placeholders removed locally and kept on the server, no placeholders or reparse points left; 228 ms for 336 entries, 2,281 ms for 5,050 |
| Explorer | the Status column shows the cloud and green-check icons in a converted folder (screenshot inspected) |

Still unverified for 0.1.1: the shipped installers and executables against a server (installing over a
real 0.1.0, the real uninstaller's release of on-demand folders); `tools/package-smoke.ps1` not run;
SyncD's own navigation entry (name, icon) in Explorer; already-open Explorer windows; a kill in the middle
of file conversion; an online-only file opened while offline; switching address with placeholders; folders
that cannot convert (Cloud Files unavailable, a nested root) on a real profile; conversion timing at 10,000
and 100,000 files in the real daemon; the x64 upgrade on 34.0.6; the interface for the restarted-cycle
state; a journal record already rewritten to
a virtual-file type by a pre-fix development build is not migrated (no released version could produce
it). Row 23.3-8 stays `partial` for these reasons.

Later runs of the same day, on the 21-patch code (patches 0001–0021, server 35.0.3), on the same ARM64 host,
for intermittent failures of the on-demand acceptance outside the packaged host (real user registry,
Explorer seeing the root). Details: [platform notes](platform-notes.md#further-011-evidence).

| Evidence | Result |
|---|---|
| Before any fix | outside the packaged host 4/5, 6/8, 7/8, 5/6; inside 4/5 |
| After the fixes, ARM64 native, outside | 8/8 consecutive passes with an Explorer window open, 37.9–41.9 s each, one first-cycle race hit and handled; 1/1 with the 30 s hold (68.7 s); `tools/test-ondemand.ps1` 1/1 (fixture 38.3 s) |
| After the fixes, ARM64 native, inside the packaged host | `tools/test-ondemand.ps1` 2/2 (fixture 38.9 s and 38.8 s) |
| After the fixes, x64 emulated, outside | 2/2 (43.7 s and 43.0 s) |
| SyncD CTest, fast tier | 24/24 on ARM64 and x64; fast tier ARM64 54.6 s, x64 57.5 s |
| Navigation pane, outside | a new Explorer window showed "Nextcloud MyGo - root" between Gallery and OneDrive (screenshot inspected, not kept); the fixture's executable has no icon resource, so the icon was generic; 0 sync roots, namespace entries and CLSID keys of the product left |

Causes: a first-cycle listing race (a product defect, fixed), the server keeping an ETag within the same
second (a fixture assumption, plus a risk that patch 0022 handles later) and a re-entrant `hold` command
(a fixture defect; INV-05 held). Known risks: [on-demand](on-demand.md#known-risks). Not covered by these
runs: the once-in-a-row bound of the restart excuse, which was added after them (`SyncDEngineContracts`
tests it, see below); SyncD's own icon in the navigation pane; already-open Explorer windows; the
same-second case on Nextcloud 34 and in the versions app.

Patches 0022 and 0023 and the restart tests (2026-11-14 to 2026-11-15, native ARM64, same host; new tests
only; the results of the full run are in the next paragraph):

| Evidence | Result |
|---|---|
| Patch 0022: `testServerSizeChangeUnderSameEtag` (unchanged locally: downloaded, a second sync does nothing; edited locally: conflict, the server's content kept, the conflict copy holds the local edit), `testEncryptedFileSizeIsNotComparedWithTheServer`, `testServerSizeChangeUnderSameEtagUpdatesPlaceholder` | pass on ARM64, run singly. Before the patch the same tests showed no change, an upload and no change |
| Patch 0023: `testOnDemandListingStoresTheFileIdentity` (another process lists a virtual folder; the stored identity equals the file ID) | passes on ARM64. Before the patch the identity was the ID followed by bytes read past the buffer |
| On-demand acceptance fixture | keeps its 1.1 s waits; no real-server same-second step was added |
| Re-entrancy of pipe commands | a code-reading audit, no run: no product defect found, no code change, no test exercises re-entrant delivery ([on-demand](on-demand.md#known-risks)) |
| `SyncDEngineContracts` (`failureFilter`, `excusedCycles`, `restartFollowUp`, `pausedFollowUp`) | added; deterministic, no server; SyncD CTest is now 25 targets. Its result is part of the final run below, not recorded separately |

Final run on the 23-patch build (2026-11-15):

All runs inside the packaged host, on the same ARM64 host, server 35.0.3; x64 is emulated. Upstream suite
(`UtilityTest` and `SyncCfApiTest` left out, as the build script does): 95/96 on ARM64 (463.4 s) and 95/96
on x64 (499.6 s) when both architectures ran at the same time; the failed target on both was
`PushNotificationsTest`, which listens on a fixed local port; run alone afterwards it passed 2/2 on ARM64
and 3/3 on x64. `SyncCfApiTest`: 55 passed, 2 skipped, 0 failed on ARM64 (90.9 s) and on x64 (94.2 s).
Fast tier (Go checks, Go unit and real-pipe tests, SyncD CTest with all 25 targets,
`SyncDEngineContracts` among them): ARM64 73.0 s, x64 64.3 s. `tools/test-ondemand.ps1`: ARM64 fixture
43.9 s, x64 45.7 s. Server tier: ARM64 496.5 s of 600 s, all 24 scenarios passed, which is also the
re-check required after an upstream change; x64 198.5 s, its one smoke scenario (switch-kill committed).
Packages rebuilt from these trees: `tools/pkg check` reports 29 PE files, all native ARM64, and 30 PE
files, all native x64, every import resolved; version 0.1.1 agrees in the manifest, SyncD and the
interface. Not run on this build: anything outside the packaged host; `tools/package-smoke.ps1` (refused
by the tool permission check, twice); the installers themselves; the upgrade from 0.1.0; server 34.0.6.

Every figure above this paragraph (24/24, 96/96, 53 passed and 2 skipped, the tier timings, the packages)
was measured on the 21-patch build or an earlier one and is not evidence for the 23-patch build.

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
| 24-hour run (R21, 23.3-4) | Replaced by an accelerated 1-hour soak ([platform notes](platform-notes.md#accelerated-soak-r21)): it cannot show midnight rollover, real token expiry or leaks below its measured bounds |
| End-to-end encrypted folders and other providers' Cloud Files placeholders (S18, S21) | The disposable server has no `end_to_end_encryption` app, and installing it from the app store was not done; a genuine placeholder of another provider needs that provider's sync root. Junctions and offline files take the same refusal path and are refused for real |
| Drive-letter reuse by another volume (S15) | Needs a second fixed NTFS volume on this machine; a replaced folder and a drive alias are refused for real |
| A locked key store (R07) | Windows Credential Manager is not locked in an interactive session on this machine; a read failure other than "not found" is mapped to `CREDENTIALS_LOCKED` |
| A journal restored by hand (R18) | SyncD makes no journal backup and has no restore; detecting an older journal copied over the current one would need a durable journal marker in Control, a design decision left to the user (`PLAN.md` §16.3) |
