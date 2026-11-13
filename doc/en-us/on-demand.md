# On-demand files (Windows Cloud Files)

An on-demand folder keeps every remote file as a Cloud Files placeholder: it is listed with its name,
size and date, and its content is downloaded only when an application opens it. This is the Windows
half of M6 (`PLAN.md` §2.3). A folder chooses full-file or on-demand mode when it is added; an
existing folder never changes mode, because upstream's mode switch deletes and rebuilds the journal.

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

The shell extension (thumbnails, lock and share overlays) is not built. Explorer's own Cloud Files
integration provides the status column and icons, transfer progress and the "Always keep on this
device" and "Free up space" commands.

## Registration

`OnDemandRoot` (`backend/platform/windows/`) registers and connects one folder's sync root for one
binding epoch. The registration is
`Nextcloud-MyGo!<user SID>!<account ID>!<folder ID>` under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager`, written by the current
user without elevation, with the provider name `Nextcloud-MyGo` and the product version. The
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

## Removal

Removing an on-demand folder ends its registration after its runtime is gone. Files whose content is
on disk become ordinary files again, so a later full-file mapping of the folder is possible;
online-only placeholders are deleted, since nothing could open them without the provider and the
server keeps their content. Other local content and the journal stay. SyncD never calls upstream's
`unregisterFolder`, which deletes shell-extension classes when no root of its provider remains.

## Verification

`syncd_ondemand_tests` (CTest `SyncDOnDemandContracts`) runs on temporary NTFS folders against the
real Cloud Files API, without a server: registration identity, refused foreign and nested roots, the
request gate a switch waits on, and complete unregistration. It first classifies simulated Cloud Files
answers (`OnDemandRoot::ownershipReason`) on every machine. Where Cloud Files cannot answer for its
temporary folder, it prints `SKIP` with the query's HRESULT and the filter state and exits 77, which
CTest reports as skipped, never as passed. `tools/test-ondemand.ps1` runs
`syncd_ondemand_acceptance` against a disposable local server: placeholders, hydration on open,
population, free up space, always keep on this device, a switch that waits for a running hydration,
refusal while closed and service on the next epoch, a kill during a hydration and restart, and
unregistration. `tools/test-cfapi-upstream.ps1` runs upstream's `SyncCfApiTest`. Results are in
[platform notes](platform-notes.md).
