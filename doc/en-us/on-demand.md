# On-demand files (Windows Cloud Files)

An on-demand folder keeps every remote file as a Cloud Files placeholder: it is listed with its name,
size and date, and its content is downloaded only when an application opens it. This is the Windows
half of M6 (`PLAN.md` §2.3), enabled from 0.1.1: the `virtualFiles` capability is true when SyncD is
an engine build, its state is available and `OnDemandRoot::available()` holds (computed once per
process). A folder chooses full-file or on-demand mode when it is added, on demand by default, and
never changes mode afterwards, except that a folder added by 0.1.0 (always full files) is converted
once, automatically ([below](#automatic-conversion-of-full-file-folders)).

## Reused upstream code

SyncD loads upstream's CFAPI plugin (`src/libsync/vfs/cfapi`, `nextcloudsync_vfs_cfapi.dll`) and gives
it to the engine as the folder's VFS. Discovery, placeholder creation, hydration, dehydration, pin
states and directory population are upstream's. Patch 0017 adapts the plugin to a service that owns
its accounts ([upstream](upstream.md)):

- `BUILD_CFAPI_VFS` builds the plugin without its Explorer shell extension, which needs the GUI.
- `VfsSetupParams::syncRootAccountId` replaces the account's display name (`user@host`) in the sync
  root ID, so the registration does not depend on the server address.
- A sync root names no thumbnail or custom-state handler class and no navigation-pane class that was
  not registered, so it never points Explorer at another client's shell extension.
- `Vfs::refuseNewRequests` and `Vfs::hasRequestsInFlight` let the owner close a root and see when no
  hydration or population callback is running before it disconnects.

The shell extension (thumbnails, lock and share overlays) is still not built. Explorer's own Cloud Files
integration provides the status column and icons, transfer progress and the "Always keep on this
device" and "Free up space" commands.

## Registration

`OnDemandRoot` (`backend/platform/windows/`) registers and connects one folder's sync root for one
binding epoch. The registration is
`Nextcloud-MyGo!<user SID>!<account ID>!<folder ID>` under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager`, written by the current
user without elevation, with the provider name `Nextcloud-MyGo` and the product version. The
root's display name is `Nextcloud MyGo - <folder name>`; the provider ID stays `Nextcloud-MyGo`. The
official client registers as `Nextcloud!…`; the two never share a key.

A folder may become an on-demand root only if it is no sync root and lies inside none. A path that
Cloud Files reports as another provider's, another folder's or inside a root is `FOLDER_OWNED`. On-demand
also needs the Cloud Files minifilter (service `CldFlt`) to be running; Windows Server, including
GitHub's runner images, may not run it. Without it `folders.add` with `onDemand` answers
`UNSUPPORTED_CAPABILITY`, and an existing on-demand folder fails to start with `UNSUPPORTED_FOLDER_MODE`.
A Cloud Files query that fails because Cloud Files cannot answer (provider not running, not supported,
invalid function, or any failure while the filter is stopped) is never read as `FOLDER_OWNED`. Any other
failed query still refuses as owned. Full-file folders do not ask Cloud Files and work either way. The
root lease admits a cloud reparse tag on the root itself and Cloud Files placeholders in the tree only
for an on-demand folder; any other reparse point, and every placeholder in a full-file folder, still
blocks the folder. The tree check never lists an unpopulated directory, which would ask SyncD's own
provider to populate it.

## Requests and the switch boundary

A hydration is a request on the current endpoint epoch: it uses that epoch's account and its
origin-bound network manager, so credentials still go only to the approved origin. When an account
quiesces for a switch or a re-authentication, every on-demand root of the account is closed: requests
that arrive from then on fail at once, and a request already running continues like a transfer. The
account is quiescent only when no cycle and no on-demand request is running; retirement then
disconnects and destroys the root, which releases the old account. Rebinding connects a new root to
the new account under the same registration. A quiesce that ends without rebinding reconnects the
root before the next cycle. While a root is disconnected, Windows reports the provider as unavailable
to an application that opens an online-only file; it is never shown an empty file.

The user-session lifetime follows SyncD's: a per-user process, never elevated, never a service. When
SyncD is not running, online-only files cannot be opened and hydrated files stay readable. The next
start reconnects the registration.

## Crash during a hydration

Cloud Files fails an open request when its provider process ends. The placeholder keeps the content
that had arrived and stays online-only; its journal record stays virtual, because upstream records a
hydration only when it has completed. The next open after a restart downloads the file again from the
start.

## Automatic conversion of full-file folders

Folders added by 0.1.0 hold full files. By the user's decision of 2026-11-14, SyncD converts each of
them to on-demand files once, without downloading or deleting anything. Upstream's own mode switch
(`Folder::setVirtualFilesEnabled`, `SyncEngine::switchToVirtualFiles`) does not delete or rebuild the
journal; it only turns local files into placeholders, and it passes an empty item to the conversion,
so SyncD has its own pass ([upstream](upstream.md#modern-cloud-file-frameworks)).

Control schema 12 adds `folders.conversion` (`''`, `pending`, `converting`, `converted`) and
`folders.conversion_reason`. Migrating a schema 1–11 database marks every `full` row `pending`; a
folder added under 0.1.1 has `''` and is never converted, whichever mode it has. Protocol 1.11 projects
the state ([IPC](ipc.md)): `converting` reads as `pending`.

The state machine lives in `Control::attachFolder`, which runs at start and whenever a folder is
attached, for a folder of a ready account that has no runtime:

1. Eligibility is `OnDemandRoot::conversionError`: `UNSUPPORTED_FOLDER_MODE` when on-demand is
   unavailable here or upstream refuses the location (a drive root, a volume that is not NTFS, a
   network drive), otherwise the ownership check of the registration section (`FOLDER_OWNED`). An
   ineligible folder keeps full files, stores the code as `conversion_reason` and attaches as full;
   the next attach tries again.
2. An eligible folder becomes `onDemand` with `converting` in one transaction, before any file is
   touched. A full-file root lease refuses placeholders and an on-demand lease admits a tree with
   both, so a crash from here on restarts as an on-demand conversion.
3. The folder attaches with `Binding::convertExisting`: the `EngineAdapter` constructor registers and
   connects the root, then runs `OnDemandRoot::convertExisting` on the control thread, before the
   first cycle ([engine](engine.md)).
4. When the attach succeeds the row becomes `converted`.

### What the pass converts

The pass reads the folder's journal and never writes it; it never deletes, dehydrates or downloads,
and leaves pin states as they are. For each record:

- A directory is always converted. Upstream has no repair for directories (below), so they get no
  budget.
- A file is converted within `EngineAdapter::conversionBudgetMs` (3 s of file work), and only while
  its size and modification time still equal its journal record: content that was never uploaded is
  never marked in sync, since it could then be freed up without a copy on the server.
- The placeholder's identity is the journal's file ID, and it is marked in sync and hydrated.
- Entries that are already placeholders (an earlier, interrupted pass) are skipped, as are missing
  entries, excluded names and anything that is neither a plain file nor a directory. A sparse file
  converts like any other: it reads as hydrated afterwards, so unregistering turns it back into an
  ordinary file and never deletes it ([sparse files](#sparse-files)).
- A file the pass cannot open, for example one marked for deletion, stays an ordinary file. An
  exclusively open file converts, because upstream's conversion opens it with no access rights.

Files left by the budget, a failure or a change become placeholders in the first on-demand cycle
through upstream's own repair: in a Cloud Files root discovery reads a plain file as metadata-missing
and issues `UPDATE_METADATA` for the unchanged file, without a download. `.lnk` files left over
stay plain. About 2,000 small files fit the budget on the ARM64 test machine; a complete
pass over 10,000 files took about 13.4 s there ([platform notes](platform-notes.md)).

### Crash and rollback

| Where the process ends | State on restart | What happens |
|---|---|---|
| before the mode flips | `full`, `pending` | the whole attach is retried |
| between the flip and `converted` | `onDemand`, `converting` | the pass repeats; it is idempotent |
| after `converted` | `onDemand`, `converted` | an ordinary on-demand folder |

If the adapter constructor throws while the row is `converting` (a journal that cannot be read, an
account that needs signing in, a root that moved), no on-demand cycle has run, so no placeholder is
online-only. `OnDemandRoot::unregister` turns the placeholders back into ordinary files and deletes
nothing. On success the row returns to `full` with `pending` and the reason code, and the folder
attaches as full. If the unregistration fails, the row stays `converting`, the error is thrown and the
next start retries.

A converted folder is never converted again, and a folder whose conversion is `''` is never touched.

## Local deletions and sparse files

### Local deletions

When discovery finds a local removal in a Cloud Files folder, upstream's `SyncEngine::shouldRestartSync`
ends that cycle once as failed, so that the next one discovers again before anything is removed on the
server, and lets the next one through. The "once" is a flag of the engine object. SyncD creates one engine
per cycle, so the flag was lost: every such cycle restarted, no local deletion in an on-demand folder
ever reached the server, and the folder stayed in `SYNC_FAILED` with deletion protection never reached.
The defect was in the on-demand code from the start and unreachable in 0.1.0, where the capability was
off. Patch 0020 adds `SyncEngine::restartedSyncAfterDiscovery()` and `setRestartedSyncAfterDiscovery(bool)`;
the `EngineAdapter` keeps the flag between cycles, sets it into each new engine and reads it back
([engine](engine.md#local-deletions-in-on-demand-folders)).

What the person sees: the restarted cycle is no failure. The folder shows `syncing` with no reason, no
failure is counted, no blocker is set, the work stays pending, and the follow-up runs about 2 s later
(`EngineRuntime::restartDelayMs`) without backoff. Every cycle with a local removal in an on-demand folder
is still restarted once, as upstream does, including the one after the user approves a held deletion; the
approval survives it.

A second cause of a restarted cycle is a listing request during a folder's first cycle. Registering the sync
root makes the shell list it at once, so another process can ask Cloud Files for the directory listing
(FETCH_PLACEHOLDERS) while the cycle runs. The process was not identified; Explorer is inferred. Upstream's
listing callback then creates the placeholders, and the engine's own bulk placeholder creation fails with
"Cannot create a file when that file already exists". The items ended in NormalError and the cycle in
`SYNC_FAILED`, so the folder showed an error, although the listing writes the journal records and the next
cycle is clean. This was seen in about 1 of 7 to 8 runs outside the packaged host with Explorer windows open,
never inside it. `OnDemandRoot::holds(relativePath, fileId)` reads a placeholder's identity without
requesting content. The `EngineAdapter` records failed items (`EngineAdapter::failure`) and decides the
excuse (`EngineAdapter::excused`). When the only failures of a cycle are new
items in the downloaded direction, virtual files, in NormalError, whose path now holds a placeholder of the
same server file ID, the cycle is reported as restarted: no reason, a follow-up 2 s later, no backoff, the
folder shown as `syncing`. This is excused once in a row: if the next cycle fails the same way, it is
reported as `SYNC_FAILED` as before. Any other failed item in the cycle disables it. Data handling is
unchanged; only the classification changes. `holds` accepts the identity in both forms that exist: the exact file ID, which placeholder creation and,
since patch 0023, the listing path store, and the UTF-16 string with its terminator that conversion stores
([upstream](upstream.md#nextcloud-desktop)).

### Sparse files

Upstream read every NTFS sparse file as an online-only placeholder. A new local sparse file was deleted
locally and never uploaded, and edits of an existing sparse file were not uploaded and later diverged
silently. Patch 0021 makes `VfsCfApi::isDehydratedPlaceholder` and `statTypeVirtualFile` treat a file as
dehydrated only when it has the cloud reparse tag and `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` and is not a
directory; sparse alone no longer counts ([upstream](upstream.md#nextcloud-desktop) has the attribute
table). `OnDemandRoot::convertExisting` no longer skips sparse files: a converted sparse file reads as
hydrated.

## Navigation pane entry and icon

On-demand folders share one Explorer navigation pane entry per folder that holds them (from 0.1.2;
0.1.1 wrote one entry per folder). Setup places every folder of an account below one base folder
(`%USERPROFILE%\Nextcloud` by default), so normally there is exactly one entry, and it opens that base
folder (`OnDemandRoot::navigationTarget`). The target is the root itself when the folder maps the whole
account (remote path `/`) or when its parent is a drive root. The entry is named "Nextcloud MyGo",
followed by " - <target folder name>" unless that name is `Nextcloud` (`OnDemandRoot::navigationTitle`).
Its CLSID is a name-based UUID of the target path, compared case-insensitively
(`OnDemandRoot::navigationClsid`), so every root below one folder names the same entry, across starts,
profiles and accounts. Each sync root's own display name stays `Nextcloud MyGo - <folder name>`.

The entry is written under HKCU only, modelled on upstream's `src/gui/navigationpanehelper.cpp` (GUI code
that cannot be linked): `Software\Classes\CLSID\{clsid}` and its `Wow6432Node` copy,
`Explorer\Desktop\NameSpace\{clsid}` and the `HideDesktopIcons\NewStartPanel` value. The in-process
server is the system's `shell32.dll`, so no DLL of this product is loaded into Explorer. Every key carries
`ApplicationName=Nextcloud-MyGo` (the official client writes `Nextcloud`), which lets the uninstaller
recognize what is left. The CLSID is also passed as `VfsSetupParams::navigationPaneClsid`, so patch 0017
writes it as the sync root's `NamespaceCLSID`; that value is how SyncD and the uninstaller know which
registrations still use an entry.

The entry is rewritten at every start of a root, because its icon is the running SyncD, which lies in a
versioned directory, and every start removes the folder's 0.1.1 entry (`{<folder ID>}`). Ending a
registration removes the entry only when no other sync root of this user still names it, and removes the
folder's 0.1.1 entry in any case; a removal that finds no registration checks the entries of both
location rules the same way. A registration that fails the first time leaves the entry only if another
root names it. A failed write of the entry only costs the sidebar item: registration goes on.

New Explorer windows pick up an added or removed entry without a restart and without `SHChangeNotify`
(observed with a hand-written entry of the same layout on build 26300, ARM64); already-open windows were
not tested. A new Explorer window opened while the acceptance's root was registered, outside the packaged
host, showed the entry "Nextcloud MyGo - root" between Gallery and OneDrive (screenshot inspected, not kept
in the repository). That was the fixture's executable, which has no icon resource, so the entry showed a
generic folder icon. SyncD's own icon in the pane is unverified, as are already-open windows. After the
runs the real registry held no sync root, namespace entry or CLSID key of the product.

`nextcloud-mygo-syncd.exe` carries the Nextcloud icon as icon 0 (`backend/platform/windows/syncd.ico`,
see [third-party](third-party.md)), which Explorer shows for the sync root and its entry.
`tools/pkg check` and `stage` refuse a package whose SyncD has no icon resource.

## Removal

Removing an on-demand folder ends its registration after its runtime is gone. Files whose content is
on disk become ordinary files again, so a later full-file mapping of the folder is possible;
online-only placeholders are deleted, since nothing could open them without the provider and the
server keeps their content. Other local content and the journal stay. The folder's navigation pane
entry goes with its registration. SyncD never calls upstream's `unregisterFolder`, which deletes
shell-extension classes when no root of its provider remains.

A plain uninstall removes every on-demand folder first, so no registration or online-only file
outlives the program ([packaging](packaging.md#uninstall)).

## Verification

Shell-integration checks (the HKCU and HKLM keys, the navigation pane) are only valid when SyncD or the
tests run outside a packaged (MSIX) host application: a process started from one writes to that
package's private registry view, which Explorer never reads.

The acceptance is checked in two places: inside the packaged host that the development tools run in, and
outside it, in the real user registry with Explorer seeing the root. Intermittent failures appeared mostly
outside (and once inside), and the runs in [platform notes](platform-notes.md#further-011-evidence) separate
the two. The fixture keeps a ring buffer in memory: upstream's info-level log (with the requesting
process's image name for Cloud Files callbacks), cycle starts and ends, and the root's change
notifications. It prints them as `LOG` lines only after a failed cycle, a failed step (the `dump` command)
or a hold timeout; live printing changed the timing and was removed. `SYNCD_TEST_HOLD=<marker>` keeps the
registered root served for 30 s after the checks, so that Explorer can be inspected. `Dav::change` waits
1.1 s before each of its three server-side rewrites, because the server keeps a file's ETag when it is
rewritten within the same second ([known risks](#known-risks)); without the wait the conflict step
sometimes found no conflict copy. The waits stay since patch 0022; no real-server same-second step was
added. The step "a switch waits for a running hydration (INV-05)" once failed
with `hold waited 30000 ms`: the fixture's `hold` command ran re-entrantly in the nested event loop of the
cycle's preflight collection read and spun there, which kept the cycle from starting, and the late
hydration request was then correctly refused because the root was closing. `hold` now freezes admission and
polls for idle from a timer. The product's INV-05 held.

`syncd_ondemand_tests` (CTest `SyncDOnDemandContracts`) runs on temporary NTFS folders against the
real Cloud Files API, without a server: registration identity, refused foreign and nested roots, the
request gate a switch waits on, and complete unregistration. It first classifies simulated Cloud Files
answers (`OnDemandRoot::ownershipReason`) on every machine. Where Cloud Files cannot answer for its
temporary folder, it prints `SKIP` with the query's HRESULT and the filter state and exits 77, which
CTest reports as skipped, never as passed. `tools/test-ondemand.ps1` runs
`syncd_ondemand_acceptance` against a disposable local server: placeholders, hydration on open,
population, free up space, always keep on this device, a switch that waits for a running hydration,
refusal while closed and service on the next epoch, a kill during a hydration and restart, and
unregistration, then the conversion steps: a folder synced with full files, the conversion pass with one
file held delete-pending (it stays plain), an unchanged journal and in-sync hydrated placeholders that
carry the journal's file IDs, a first on-demand cycle that repairs the file left and downloads nothing,
and free up space followed by re-opening the file. It also covers the local deletion of an online-only
file (it reaches the server in the automatic follow-up), a directory move as one rename, deleting 13
entries held until the user approves, and the sparse cases (a new file, an edit, a change on the server,
a change on both sides; the journal type stays a file); the conversion section converts a sparse file
too. `tools/test-cfapi-upstream.ps1` runs upstream's
`SyncCfApiTest`.

The conversion has two more contract tests, both on real Cloud Files and without a server.
`conversionCase` in `syncd_ondemand_tests` runs `OnDemandRoot::convertExisting` on a hand-written
journal: identities from the journal's file IDs, directories converted with the budget gone, files only
while it lasts, a second pass completing an interrupted first, changed, missing and
delete-pending entries left, a sparse file converted, an exclusively open file converted, the journal unchanged, and the
rollback (`unregister`) leaving every file ordinary and deleting nothing. `conversionStates` in
`folder_error_tests.cpp` drives `Control::attachFolder` through each row of the state table: a folder
added as full is never converted, a pending one inside another provider's root keeps full files with
`FOLDER_OWNED` and converts once that root is gone, an eligible and an interrupted one end `converted`,
one whose pass fails after registering (an unreadable journal) is released and returns to `full` with
its reason and an untouched journal, and a converted one stays. Both print `SKIP` where Cloud Files
cannot answer. Results are in [platform notes](platform-notes.md).

The contract test also has the step "a placeholder is recognized by its file ID in both identity forms"
(`OnDemandRoot::holds`: the raw form, the listing form, and the negatives: another ID, an ID prefix, an
ordinary file, a missing file, an empty ID). It passes on ARM64 and x64. Since patch 0023 the listing path
stores the exact form too; conversion still stores a UTF-16 string with its terminator, so `holds` keeps
accepting both.

The engine-side decisions of the restarted cycle have a deterministic test with no server and no Cloud Files:
`syncd_engine_tests` (CTest `SyncDEngineContracts`, which needs the upstream build). `failureFilter`
classifies real sync items through `EngineAdapter::failure` (none, placeholder, other). `excusedCycles`
feeds `EngineAdapter::excused` back over four cycles and gets excused, not, excused, not: the
once-in-a-row bound. `restartFollowUp` runs `EngineRuntime::settle` on a controllable clock: the follow-up is
not admitted at +1999 ms and is admitted at +2000 ms, the failure count is unchanged, a real failure
afterwards backs off 16 s and a success resets. `pausedFollowUp` shows that a user's pause set during the
cycle survives the follow-up and that a global freeze holds it (INV-13, INV-14). The race itself was hit
once on a real run (ARM64, outside the packaged host), where the fixture printed `STEP first cycle
restarted, its follow-up answers` and passed; it was never hit on x64.

## Known risks

- **Same-second ETag (handled by patch 0022, with limits).** Nextcloud 35.0.3 can keep a file's ETag when
  the file is rewritten twice within one second: a probe got identical ETags for both PUTs in 5 of 15
  back-to-back pairs of 2 MiB uploads. Upstream discovery tests a server-side change of a file only by its
  ETag (`processFileAnalyzeRemoteInfo`), so an equal ETag with a different size counted as "server
  unchanged"; a local edit then uploaded, the upload's If-Match passed because the ETag was the same, and
  the server's newer content was replaced. Patch 0022 treats a file whose server size differs from the
  journal's size, under an equal ETag, as changed on the server, through upstream's existing path: it is
  downloaded; when it was also edited locally it becomes a conflict copy and the server's content is kept;
  a dehydrated placeholder takes the new size. Excluded: folders (their size is the tree's),
  end-to-end-encrypted entries (the journal holds the plaintext size, the server reports ciphertext) and
  journal records with a mangled name. Virtual files and live photos are not excluded on purpose. The mtime
  and the server checksum are not compared: an mtime-only change stays on upstream's metadata branch, and
  the server omits checksums for clients that upload without one. Still undetectable: a same-size rewrite
  within one second, and a rewrite on a server whose parent folder also keeps its ETag (an unchanged
  folder is not listed; Nextcloud gives the parent a fresh ETag on every write, as observed). Residual
  risk, recorded and not guarded: a server whose reported size permanently differs from the downloaded
  body length, with no Content-Length, would be downloaded again on each sync (no data loss); a guard would
  need journal state. Not tested: whether the replaced content survived in the server's versions app before
  the patch, and Nextcloud 34 ([upstream](upstream.md#nextcloud-server-apis)).
- **Re-entrant pipe commands (audited, no product defect, no code change).** Control runs three nested
  event loops in product code on the control thread: reading server JSON (bounded to 4 s without
  credentials and 15 s with), reading collections (30 s), and the wait in `~OnDemandRoot` when an adapter is
  destroyed while its root is busy (60 s). Every timer, pipe command and queued callback can be delivered
  inside them. Reading the code, each case is refused, deferred to a timer retry, or turned into a
  cancellation that the suspended frame checks after the read: a second cycle is impossible while one is
  active; removing the running folder is refused; account removal, endpoint switch, failover and
  re-authentication wait for quiescence and retry; pause and stop mark the cycle cancelled; shutdown
  finishes only after the cycle completed; a nested dispatch is refused. No product handler waits or
  spins. So there is no second journal writer and no epoch change under a running cycle. Residual, recorded:
  a read nested in another read delays the outer one, bounded by the read deadlines; a Cloud Files callback
  that arrives between a folder removal's quiescence check and the removal can make the destructor serve it
  for up to 60 s. Not determined: that the Qt Windows dispatcher never re-enters a timer slot was not
  tested here (the guards above do not depend on it); no path was found by which deletions deferred during a
  retire free an object that a suspended frame of another folder uses, but its absence was not proven. This
  is a code-reading audit: no test exercises re-entrant delivery in the product.
