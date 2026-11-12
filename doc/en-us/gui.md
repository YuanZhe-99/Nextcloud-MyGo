# Interface

The interface is a native MyGo application (`cmd/nextcloud-mygo`, `internal/gui`) for the person
using Nextcloud MyGo, built from the screens in `design/`. It holds no sync state of its own: SyncD
owns accounts, folders, addresses and decisions; the interface shows the snapshot SyncD reports
and sends it commands through `internal/syncd.Remote` (protocol 1.9, see [IPC](ipc.md)). The paired
SyncD executable is the default; `NEXTCLOUD_MYGO_MOCK=1` explicitly selects `syncd.Mock`, which
labels its sample data, declares the capabilities it simulates and changes nothing on this PC.

## Threads and state

Every field of `gui.App` is read and written on the main thread. Views run there; a command to
SyncD runs on its own goroutine (`App.do`) and hands its result back through `App.later`, which
redraws the window and the tray. A command keeps its button disabled until it returns; a failure
shows as a toast in words taken from SyncD's stable reason code, never the code itself. An
accepted command is not shown as done: the next snapshot shows the outcome. The interface's own
settings are in `gui.json` in `%AppData%\Nextcloud-MyGo`, written atomically; nothing about sync is
kept there.

## Connection and capabilities

- **Unknown is not empty.** Before the first snapshot, and while SyncD runs without its state
  (`stateAvailable` false: backoff, diagnostic, stopped or database unavailable), the window shows
  a status card instead of the pages or first-run setup, with Diagnostics and Settings. Lists SyncD
  did not send are unknown: no "no folders", no "nothing yet".
- **Disconnected is not current.** After the pipe drops, the last known lists stay under a banner
  that says so; commands are disabled until SyncD is back, and nothing reads as healthy.
- **Capabilities gate features.** Sign-in needs `login`. While `fullSync` is false, adding folders
  first says that file sync is still being verified, and no state is ever green: a folder SyncD
  reports up to date reads "Last sync finished". While `endpointAutoSwitch` is false, Automatic is
  not offered; each account uses the one address chosen. A further account needs `multiAccount`
  (the first never does). Full files or On demand is offered only with `virtualFiles`.
- **Signing in again.** An account needs it as its `auth` says: `needsSignIn` (the saved sign-in
  cannot be used on this PC, or a renewal waits for the browser) or `revoked` (the server refused
  the app password), each in its own words. Only a daemon before 1.9, which sends no `auth`, is
  inferred from a signed-out account or a folder reporting `AUTH_REQUIRED`.
- **A full server.** A folder reporting `REMOTE_QUOTA_EXCEEDED` finished its cycle, but its
  uploads were refused: it reads "Server storage full", its account gets a notice with what that
  means, and the overall state needs the user.

## Main window

- **Title bar:** drawn by the view: the mark, the name, a handle that moves the window, and
  Windows 11's caption buttons.
- **Sidebar:** the accounts, each with its server or Sign in again (up to four, then the rest
  counted; each opens Accounts & folders), the four pages with the count of what waits on
  Activity, Settings, and the overall state as a dot and a word with the address the chosen
  account uses (opens Network). The overall state is the first that applies: unknown, paused,
  needs you (a deletion review, an account to sign in again, a full server, a folder waiting or
  failing), syncing, offline, last sync finished (without `fullSync`), up to date.
- **Overview:** Pause sync / Resume sync and Check now; what sync does now, with the phase and
  the files so far (`phase`, `filesDone`) rather than an invented total, also on each syncing
  folder's card; the address the chosen account uses and why; every item that needs the user,
  each with its one button (a full server has none: only freeing space there helps); the folders.
- **Accounts & folders:** Add an account; per account its sign-in state, Add a folder or Sign in
  in browser (then Open the browser again and Cancel while it waits), and in a menu Sign in again
  and Remove this account; its folders with their mode (when `virtualFiles` is on or the folder is
  on demand), paths, state, the reason in words, and Open, Pause or Resume, Review, and in a menu
  Check now and Stop syncing. Removing a folder or an account asks first and says that the files
  stay on both sides; removing an account is one `accounts.remove`, which stops its folders itself
  after a running cycle.
- **Network:** Automatic or Always use one address (only with `endpointAutoSwitch`); the account's
  addresses, the one in use first and those that failed verification last, each with where it
  leads (its kind, when the name does not say it), its two trust steps (approved, same server
  verified), why it is blocked in words (its certificate, a different server, an address never
  allowed, a sign-in, another network; a verification under way is its trust step), how fast it
  answers (`latencyMs`), Verify now, Use this one, a pending switch, and Remove this address; why
  the address in use was chosen (`network.why`: verified, faster with both answer times, stable,
  your choice, the network joined, the others not answering, a postponed switch) and when and why
  the last switch happened; the recent switches. A switch is requested (`endpoints.setPolicy`) and
  shown pending until SyncD reaches a safe point.
- **Activity:** every deletion review (how many files, each direction with its count, the groups
  SyncD names with their file counts, when the proposal expires; Decide later first, Open folder,
  and Allow the deletions only after a second question; nothing else answers a review); conflicts,
  each closed with Mark as resolved (`resolved`, both copies stay); what else needs the user; the
  recent history, filtered. Its times, like the Network page's recent switches, are on the local
  clock (SyncD reports UTC): today as a time, then Yesterday, then a date.

## Dialogs

- **Add an address:** step 1 looks at the address, with where it leads (Internet, Home network,
  VPN; it never affects trust), without sending any credential; step 2 shows
  the certificate and the server and asks for the user's explicit permission; step 3, the
  verification, is SyncD's after approval. An invalid certificate cannot be approved; a refused
  address says why.
- **Questions:** removing, stopping and allowing deletions use an alert with Cancel first.

## First-run setup

Shown instead of the pages while SyncD reports no account, and for Add an account: Your server,
Sign in (Login Flow v2 in the browser; the interface never sees a password; an account already
added says so plainly and setup stays at Your server), Folders (the server's folders are listed
again each time the step is shown and on Refresh, so a folder made on the server since appears; a
listing that fails keeps the folders listed before and says why; the first listing chooses
Documents and Photos where the server has them, as `design/SetupFolders.html` shows, and later
listings keep the user's choices and drop any folder the server no longer has; encrypted folders
cannot be chosen; with `virtualFiles`, Full files or On demand for the folders chosen, fixed for
each folder's life; SyncD creates a local folder that does not exist yet), Start (Start when I
sign in to Windows and notifications, both on; an optional other address). Nothing syncs before
Start syncing. Add a folder opens the Folders step for an account that exists.

## Settings

A sheet over the dimmed window, opened from the sidebar's gear: search and the pages on the
left (General, Notifications, Sync and storage, Network, About & updates, Diagnostics), the page
on the right as sections of rows, each an icon tile, a title over one sentence, and its
control.

| Page | Settings |
|---|---|
| General | language (Follow Windows, English, 简体中文), appearance, open at sign-in, start in the tray, the close button, add another account |
| Notifications | deletions to review, signing in again, conflicts, updates |
| Sync and storage | pause every folder; deletion protection, always on |
| Network | a link to the Network page |
| About & updates | the update, check daily, install automatically (never, when idle, every day at 03:00), version with SyncD's version and protocol, source code, licenses |
| Diagnostics | SyncD's start report (previous version and schema, schema, self-check and its reason codes) and why the pipe last ended (`Remote.Disconnect`), shown nowhere else; SyncD's runtime reading (`diagnostics.snapshot`: versions, startup, epochs, counters); the support bundle SyncD makes (`diagnostics.export`), its path, Open folder and Copy path |

## Tray and closing

- A click on the tray icon opens the flyout (state, what needs the user, the folders, Open,
  Pause, Check now, Close window only, Quit and stop syncing); a right click opens the same
  actions as a menu. Both call what the window calls.
- The close button keeps the app in the tray (said once) or, as General chooses, closes the
  window; either way SyncD keeps syncing. Only Quit and stop syncing stops it (`daemon.shutdown`);
  if SyncD does not stop, the interface stays and says so.
- The main window exists only while shown. Opened at sign-in with Start in the tray on, the app
  shows only the tray icon. A second start only raises the running window.

## Updates

Only signed release builds update themselves (`mygo.Updater.Enabled`); others say so and their
update controls are disabled. The check runs daily while switched on. Installing first asks SyncD
for a safe point (`PrepareUpdate`); a refusal postpones it and says which: a deletion waiting for
the user (`DECISION_PENDING`, with Review), or sync not yet at a safe point (`QUIESCE_STALLED`),
told apart by `syncd.ReasonOf`. An automatic install runs when the computer has been idle for 15 minutes or within
03:00, as chosen; one that failed is not tried again in the same run.

## Languages

Strings are in `locales/en.json` (the source) and `locales/zh-Hans.json`; `go tool mygo
generate` writes `locales/catalog_gen.go`. Traditional Chinese and Japanese follow.

## Tests

`go test ./internal/gui` renders every page and Settings page in both appearances and both
languages; checks the deletion review (Decide later keeps it, allowing asks twice), the Settings
pause and that resuming everything keeps a folder's own pause, that unknown state is neither empty
nor healthy, SyncD's real capabilities (no Automatic, no green, the file-sync notice), signing in
again for a refused password, that every protocol reason code has words, times on the local clock
in a fixed non-UTC zone, the whole first-run setup against the Mock through Start syncing (a
folder made on the server after sign-in, a failed listing, a folder gone from the server), and
that Notifications' switches survive a restart. The protocol 1.9
states are table-driven (`protocol19_test.go`): each account `auth`, several accounts in the
sidebar, `multiAccount`, a duplicate account, account removal as one command, phases and a full
server, the folder mode with and without `virtualFiles`, every blocked reason, the reasons for
the address in use, several reviews and a conflict closed, the update's postponement, and the
start report only in Diagnostics. The Mock's sample shows each of these states. With
`SYNCD_TEST_EXE` set, `TestRealDaemonInterface` drives the interface against the compiled SyncD
over the real pipe (setup on a fresh profile, pause, Diagnostics and its bundle, SyncD running
after the interface closes; with `NEXTCLOUD_TEST_URL`, a browser sign-in started and cancelled).
With `SCREENSHOT_DIR` set it writes each window as a PNG to compare with `design/`.
