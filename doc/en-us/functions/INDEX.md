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
| `backend/engine*`, `backend/scheduler.*`, `backend/root*` | In-process engine/runtime, drained account rebinding, retry/safety blockers, deterministic serial queue and root leases ([engine](../engine.md)) | `EngineAdapter`, `rebindError`, `rebind`, `EngineRuntime`, `quiesceAccount`, `Scheduler`, `RootLease` |
| `backend/control_folders.cpp`, `backend/namespace.*`, `backend/deletion.*` | Durable mappings, fail-closed completion, bounded DAV metadata and distinct permission/lock/throttle outcomes, pre-propagation proposals and one-use approval | `folderMutation`, `restoreFolders`, `cycleFinished`, `MetadataError`, `checkMetadataReply`, `parseCollections`, `readCollections`, `assessDeletion`, `decisionMutation` |
| `backend/control_endpoints.cpp`, `backend/control_switch.cpp` | Durable origin consent, read-only UID/all-root verification and gated manual activation | `endpointMutation`, `probeEndpoint`, `switchMutation`, `advanceSwitch`, `verifySwitchTarget` |
| `backend/endpoint_policy.*` | Deterministic proposal-only scoring, generation isolation, debounce, quarantine and circuit recovery | `EndpointPolicy`, `beginProbe`, `observe`, `choose` |
| `backend/control_network.cpp`, `backend/network_*` | Native notification drainage, bounded health sequences, durable policy, frozen switch evidence and fresh-epoch recovery proposals | `NetworkMonitor`, `evaluateNetwork`, `networkOutcome`, `policyMutation`, `switchEvidence`, `switchFailed` |
| `backend/network_tunnel.*`, `backend/network_transfer_acceptance.cpp` | Acceptance-only bounded encrypted relay and real mapped-transfer faults with parent-owned remote cleanup | `NetworkTunnel`, `networkTransferAcceptance`, `cleanupNetworkContent` |
| `backend/control_diagnostics.cpp` | Path/credential-free runtime counters and atomic bounded local support bundles | `diagnosticsProjection`, `exportDiagnostics` |
| `backend/startup*` | Profile ownership, monotonic crash/backoff admission, diagnostic IPC, deferred activation and validated migration backups | `Startup`, `engineAllowed`, `recordStopIntent`, `finishClean`, `backupControlBeforeMigration`, `Control::activate`, `startupDispatch` |
| `backend/control_activity.cpp` | Bounded, cursor-filtered confirmed-item aggregates and committed switch history | `activityProjection`, `recordActivity` |
| `backend/watcher*` | Recursive Windows change notifications, overflow audits and drained cancellation | `RootWatcher`, `ready`, `arm` |
| `backend/request_lifetime.*` | Reply completion/destruction accounting and sticky uncertain-write observations | `RequestLifetime`, `observe`, `Counts` |
| `backend/resume_context.*` | Read-only ETag-download, checksummed single-PUT and chunking-v2 session admission; preserved blocked polls/v1 chunk indexes and corrupt journals | `resumeContextError` |
| `backend/switch_state.*` | Transactional switch intent/epoch storage, committed binding resolution and restart gates | `SwitchState`, `begin`, `commit`, `reconciled`, `interrupt` |
| `protocol/` | Protocol 1.7 manifest, schemas and shared hostile-input fixtures | `contract.json`, `message.schema.json`, `folders.schema.json`, `endpoints.schema.json`, `switch.schema.json`, `reauthentication.schema.json`, `policy.schema.json`, `startup.examples.json`, `constraints.examples.json`, `fixtures.json` |
| `tools/build-syncd.ps1` | ARM64/x64 SyncD builds and core contract tests | Script parameters |
| `locales` | Interface strings (en, zh-Hans) and their generated accessors | `en.json`, `In` |
| `internal/version` | Product version | `Version`, `Older` |
| `tools/checkdocs` | Documentation mirror check | `main` |
| `tools/build-upstream.ps1` | M0 engine/test builds of the recorded clean upstream snapshot | Script parameters and dependency-prefix checks |
| `patches/nextcloud/` | Recorded upstream build, lifetime, UTC-date and test-isolation corrections | Ordered hashes in `build-lock.json`; evidence in backend progress |
| `tools/test-upstream.ps1` | Isolated real-server M0 fixture and cleanup | `Invoke-TestCycle`, `Invoke-TestDav` |
| `tools/test-discovery-lifetime.ps1` | M0 regression for discovery workers surviving timeout fixture teardown | Bounded subprocess repetition of upstream timeout and missing-data cases |
| `tools/test-login/`, `tools/test-login.ps1` | Real paired-daemon consent, policy, switching, renewal, active-content crash/cancellation, journal-identity and owned-resource acceptance | `exerciseSync`, `exercisePolicy`, `exerciseActiveCrash`, `exerciseActiveSwitch`, `exerciseReauthentication`, `Approve` |
| `tools/test-reauth-lifetime.ps1` | Read-only cancel/replacement lifetime regression with parent-owned native credential cleanup after crashes | `replacedDuringVerification`, `cleanupManifest`, `RunOwned` |
