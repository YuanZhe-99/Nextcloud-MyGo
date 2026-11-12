# Function index

One row per Go package and backend module: its role and the declarations to read first.
The connected SyncD foundation is implemented; full sync remains unfinished. See [IPC](../ipc.md) and [backend progress](../backend-progress.md).
Declarations carry the Function Explanation Layer in their doc comments (first sentence: purpose;
then inputs, returns, side effects and notes where non-trivial).

| Package / module | Role | Start with |
|---|---|---|
| `cmd/nextcloud-mygo` | Single instance, tray, window, paired SyncD and explicit demo mode | `main` |
| `internal/gui` | Window, pages, setup, Settings, tray, updates ([gui.md](../gui.md)) | `App`, `App.view`, `App.do`, `Prefs`, `overallOf` |
| `internal/syncd` | Authenticated pipe client, snapshot projection, command retry, cancellation-aware operation waits and explicit Mock | `Client`, `Remote`, `dialPipe`, `Snapshot`, `Mock` |
| `backend/` | Control-thread daemon, durable accounts/settings/receipts, native credentials, login, Win32 transport and standalone switch guard | `Control`, `LoginFlow`, `writeCredential`, `PipeServer`, `SwitchCoordinator`, `decodeMessage` |
| `backend/control_reauth.cpp` | Account-preserving credential replacement, native-vault rollback/cleanup and same-origin runtime reload | `reauthenticate`, `installReauthentication`, `recoverReauthentication`, `reloadCredentials` |
| `backend/engine*`, `backend/scheduler.*`, `backend/root*` | In-process engine/runtime, drained account rebinding, on-demand quiescence, retry/safety blockers, deterministic serial queue and root leases ([engine](../engine.md)) | `EngineAdapter`, `rebindError`, `rebind`, `EngineRuntime`, `quiesceAccount`, `Scheduler`, `RootLease` |
| `backend/control_folders.cpp`, `backend/namespace.*`, `backend/deletion.*` | Durable mappings with their fixed file mode, fail-closed completion, bounded DAV metadata and distinct permission/lock/throttle outcomes, pre-propagation proposals and one-use approval | `folderMutation`, `restoreFolders`, `cycleFinished`, `MetadataError`, `checkMetadataReply`, `parseCollections`, `readCollections`, `assessDeletion`, `decisionMutation` |
| `backend/control_endpoints.cpp`, `backend/control_switch.cpp` | Durable origin consent, read-only UID/all-root verification and gated manual activation | `endpointMutation`, `probeEndpoint`, `switchMutation`, `advanceSwitch`, `verifySwitchTarget` |
| `backend/control_update.cpp` | The update safe point (`daemon.prepareUpdate`), the post-start report and self-check, and snapshot events without a configuration edit | `prepareUpdate`, `advanceUpdate`, `releaseUpdate`, `startupReport`, `selfCheck`, `publishState` |
| `backend/endpoint_policy.*` | Deterministic proposal-only scoring, generation isolation, debounce, quarantine and circuit recovery | `EndpointPolicy`, `beginProbe`, `observe`, `choose` |
| `backend/control_network.cpp`, `backend/network_*` | Native notification drainage, bounded health sequences, durable policy, frozen switch evidence and fresh-epoch recovery proposals | `NetworkMonitor`, `evaluateNetwork`, `networkOutcome`, `policyMutation`, `switchEvidence`, `switchFailed` |
| `backend/network_tunnel.*`, `backend/network_acceptance.h`, `backend/network_transfer_acceptance.cpp` | Acceptance-only bounded encrypted relay (throttle, held reply, withheld reply), real mapped-transfer faults, lost-reply MOVE/DELETE/final-MOVE cases, switch-stage kill and restart checks, file-safety cases, with parent-owned remote cleanup | `NetworkTunnel`, `TransferFaults`, `SwitchFacts`, `networkTransferAcceptance`, `switchKillAcceptance`, `switchRecoverAcceptance`, `cleanupNetworkContent` |
| `backend/multi_account_acceptance.cpp` | Acceptance-only 2 accounts × 2 folders run on the disposable server: duplicate refusal, per-account bindings, a switch that pauses no other account, removal of one account, with the second server user cleaned by the parent | `multiAccountAcceptance`, `AcceptanceCalls`, `cleanupOwnedUser` |
| `backend/control_diagnostics.cpp` | Path/credential-free runtime counters and atomic bounded local support bundles | `diagnosticsProjection`, `exportDiagnostics` |
| `backend/startup*` | Profile ownership, monotonic crash/backoff admission, diagnostic IPC, deferred activation and validated migration backups | `Startup`, `engineAllowed`, `recordStopIntent`, `finishClean`, `backupControlBeforeMigration`, `Control::activate`, `startupDispatch` |
| `backend/control_activity.cpp` | Bounded, cursor-filtered confirmed-item aggregates and committed switch history | `activityProjection`, `recordActivity` |
| `backend/watcher*` | Recursive Windows change notifications, overflow audits and drained cancellation | `RootWatcher`, `ready`, `arm` |
| `backend/request_lifetime.*` | Reply completion/destruction accounting and sticky uncertain-write observations | `RequestLifetime`, `observe`, `Counts` |
| `backend/resume_context.*` | Read-only ETag-download, checksummed single-PUT and chunking-v2 session admission; preserved blocked polls/v1 chunk indexes and corrupt journals | `resumeContextError` |
| `backend/switch_state.*` | Transactional switch intent/epoch storage, committed binding resolution and restart gates | `SwitchState`, `begin`, `commit`, `reconciled`, `interrupt` |
| `backend/platform/windows/ondemand*` | Cloud Files sync-root registration, ownership, the on-demand request gate a switch waits on, unregistration, and the real-server on-demand fixture ([on-demand](../on-demand.md)) | `OnDemandRoot`, `ownershipError`, `close`, `busy`, `unregister`, `EngineAdapter::quiescent` |
| `protocol/` | Protocol 1.8 manifest, schemas and shared hostile-input fixtures | `contract.json`, `message.schema.json`, `folders.schema.json`, `endpoints.schema.json`, `switch.schema.json`, `reauthentication.schema.json`, `policy.schema.json`, `startup.examples.json`, `constraints.examples.json`, `fixtures.json` |
| `tools/build-syncd.ps1` | ARM64/x64 SyncD builds and core contract tests | Script parameters |
| `tools/pkg`, `tools/package.ps1`, `tools/package-smoke.ps1` | DLL-closure staging, package manifest, native-architecture PE check and the real install test ([packaging](../packaging.md)) | `stageCommand`, `check`, `inspect`, `native` |
| `packaging/windows/setup` | Per-user install into versioned directories, coordinated two-process stop, uninstall and optional user-data removal | `install`, `stopApp`, `shutdown`, `uninstallApp`, `admit`, `extract` |
| `internal/release` | The package manifest `tools/pkg` writes and the installer verifies | `Manifest`, `Parse`, `Verify` |
| `locales` | Interface strings (en, zh-Hans) and their generated accessors | `en.json`, `In` |
| `internal/version` | Product version | `Version`, `Older` |
| `tools/checkdocs` | Documentation mirror check | `main` |
| `tools/build-upstream.ps1` | M0 engine/test builds of the recorded clean upstream snapshot | Script parameters and dependency-prefix checks |
| `patches/nextcloud/` | Recorded upstream build, lifetime, UTC-date and test-isolation corrections | Ordered hashes in `build-lock.json`; evidence in backend progress |
| `tools/test-upstream.ps1` | Isolated real-server M0 fixture and cleanup | `Invoke-TestCycle`, `Invoke-TestDav` |
| `tools/test-discovery-lifetime.ps1` | M0 regression for discovery workers surviving timeout fixture teardown | Bounded subprocess repetition of upstream timeout and missing-data cases |
| `tools/test-login/`, `tools/test-login.ps1` | Real paired-daemon consent, policy, switching, renewal, active-content crash/cancellation, journal-identity and owned-resource acceptance in four parallel lanes | `mapFolders`, `exerciseLocal`, `exerciseEndpoints`, `exercisePolicy`, `awaitRemote`, `exerciseActiveCrash`, `exerciseActiveSwitch`, `exerciseReauthentication`, `Approve` |
| `tools/test-reauth-lifetime.ps1` | Parent of the read-only lifetime regression and of every real network scenario: one fresh profile per run, the kill-and-restart sequence of `switch-kill`, native credential and remote cleanup after crashes | `replacedDuringVerification`, `cleanupManifest`, `RunOwned`, `RunProfile` |
| `tools/test-all.ps1` | The one test entry point: fast tier (no server) or server tier (disposable container, parallel lanes) per architecture, one line per scenario with its duration, a hard total budget | `Step`, `Lanes`, `Acceptance` |
| `tools/test-server.ps1` | Disposable local Nextcloud server for the server tier: one labelled container, fixture-local TLS root, removal by state file | `Runtime`, `-Start`, `-Stop` |
| `tools/test-job.ps1` | Kill-on-close Job Object for every fixture wrapper, so no daemon, probe or test executable outlives its script | `Enter-OwnedJob`, `Invoke-InOwnedJob` |
