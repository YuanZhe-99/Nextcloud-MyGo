# URL usage audit

## Scope

M0/B01 source audit of Desktop v35.0.2, commit
`07761410a90a2f27007932fcb61afc23ddda5998`. Source locations are relative to
`third_party/nextcloud`. The original checklist below is historical; current bounded runtime evidence
is recorded in [platform notes](platform-notes.md), with remaining switch blockers below.

## Request surfaces

| Surface | Source observation | Required proof before enabling |
|---|---|---|
| Credential injection | `src/libsync/creds/httpcredentials.cpp:51–74` injects credentials in the QNAM request override | Approved exact HTTPS origin and installation path checked before injection; no inherited userinfo credentials (INV-03) |
| Redirects | `src/libsync/abstractnetworkjob.cpp:240–304` resolves relative locations, rejects downgrade, removes Authorization and sets DontAddCredentials on host/port changes | Maximum three hops, canonical port comparison, no unapproved body/token forwarding, no cross-origin DAV write replay (N05) |
| Request policy/logging | `src/libsync/accessmanager.cpp:103–128` logs URLs and response headers and only defaults an unspecified redirect policy to manual | Service-enforced policy, redaction and zero-secret log/IPC scan (R22) |
| MOVE | `src/libsync/propagateremotemove.cpp` builds Destination from runtime DAV path and the rename target | Subpath and Unicode encoding, source/target same runtime, no prefix escape (N06) |
| Chunked upload | `src/libsync/propagateuploadng.cpp:257–280` stores transfer metadata, then sends MKCOL with Destination | Same namespace, upload collection and final MOVE survive cancellation/rediscovery without blind replay (S06–S08) |
| Direct download | `src/libsync/propagatedownload.cpp:738–754` branches to server-provided URL and optional direct-download Cookie | Disable cross-origin optimization for first release and verify no credential leakage (INV-03) |
| Recovery | `src/common/syncjournaldb.h:104–134` exposes DownloadInfo, UploadInfo and PollInfo; PollInfo contains a URL | Audit all recovery consumers, reject unsupported endpoint-bound recovery, never rewrite journal URLs (INV-08, INV-09) |
| DAV multistatus | `src/libsync/networkjobs.cpp` parses server properties including direct-download information | Verify relative/absolute href, per-property failures and allowed prefixes on a real server (S14, N06) |
| Login / OCS / capabilities | Product Login Flow and runtime mapping not implemented | Canonical/runtime responsibility, authorized poll origin and stable DAV UID (B04/B09) |
| Push / WebSocket | Product runtime lifecycle not implemented | Keep inactive until request ownership and epoch isolation are proven (INV-06) |
| User links / signed queries | Product browser/diagnostic routing not implemented | Never derive credential authorization from a server-returned link; do not rewrite or log signed queries |

## Integration blockers

- Upstream credential injection is a seam, not the product's origin authorization policy.
- Removing Authorization on a redirect does not establish that forwarding the request body is safe.
- UploadInfo lacking an explicit URL does not prove all recovery state is endpoint-independent:
  PollInfo explicitly contains a URL, and consumers need review.
- Full URL, credential, quiescence and journal-recovery acceptance remains incomplete; bounded real
  login, deletion-protection and fixed-journal sync checks are recorded in the platform notes.

## Current switch blockers

2026-11-11 real acceptance against server 35.0.2: the MOVE and chunked-upload rows of the table
above (S06–S08) were run with the reply of a committed request lost and the origin changed before
recovery. No request was replayed: a committed MOVE and a committed DELETE of existing entries,
with the path created again by another client, and the committed final MOVE of a chunk session all
converged by rediscovery, with the server file ID and ETag of the committed result unchanged
([platform notes](platform-notes.md)). Destination encoding under an installation subpath or with
non-ASCII names (N06) was not run.

2026-11-09 portable-resume source audit of Desktop v35.0.3: `propagateuploadv1.cpp:51–68`
persists single-PUT uncertainty with zero transfer ID/chunk and a content checksum; `discovery.cpp:1952–2003`
compares that checksum after remote discovery. `propagatedownload.cpp:662–742` uses relative local
temporary paths and ETags; `GETFileJob` checks reply ETag, content length and range before accepting
resumed bytes. The read-only admission gate now allows those two bounded classes and requires fresh
target capability/root verification and forced upstream rediscovery. Real cross-origin interruption
acceptance is pending. `propagateuploadng.cpp:34–38` derives chunk URLs relative to the current account,
but this alone is not session-portability proof: chunked-upload and all pending poll rows stay blocked.

Follow-up the same day: `propagateuploadng.cpp:90–123` reuses a stored session only when size and
mtime still match, then lists the session collection (`141–193`) and continues after the last
consecutive chunk; a missing collection starts a new upload (`195–209`), and discovery adopts a
committed final MOVE by checksum. Patch 0016 proves both after an origin change, and the gate now
admits checksum-qualified chunking-v2 rows. Poll rows and legacy v1 chunk indexes stay blocked.

2026-11-07 source recheck against Desktop v35.0.3; subsequent remediation is in progress:

- `src/common/syncjournaldb.cpp:277–288` returns no commit result; a failed SQLite commit is logged
  and leaves the transaction marker set. `close()` at lines 706–716 calls that method and then closes
  the database without reporting whether commit succeeded. Neither a closed connection nor an engine
  completion is sufficient evidence for `SwitchCoordinator::Proof::journalFlushed`.
  Patch 0011 now exposes sticky commit failure; the adapter checks it after close. New-version
  upstream acceptance remains pending until recorded in the platform notes.
- `src/libsync/account.cpp:309` owns QNAM through a shared pointer with `deleteLater` destruction.
  Releasing the account alone does not prove that replies and the old network manager are destroyed.
  The adapter now observes active/retained replies, unconfirmed writes and actual manager destruction.
  `ClientSideEncryption::_account` also holds a strong account back-reference; retirement clears that
  unused E2EE reference after engine teardown. This must pass real lifetime acceptance before switching.
- `src/libsync/propagateupload.cpp:651–668` stores the poll URL in `PollInfo` before starting its job;
  `src/libsync/syncengine.cpp:493–505` consumes pending polls before normal discovery. A later discovery
  therefore cannot be assumed to sanitize an endpoint-bound poll URL before its first use. Unsupported
  cross-endpoint recovery must block activation without rewriting or deleting the journal.
- The standalone switch guard requires recovered uncertainty before commit, while PLAN §9.3 S5
  performs rediscovery after publishing the new epoch with admission still controlled. Runtime wiring
  must distinguish drained resources from completed reconciliation rather than assert both at `finished`.
