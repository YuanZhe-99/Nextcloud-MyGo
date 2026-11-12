# Interface

The interface is a native MyGo application (`cmd/nextcloud-mygo`, `internal/gui`) for the person
using Nextcloud MyGo, built from the screens in `design/`. It holds no sync state of its own: SyncD
owns accounts, folders, addresses and decisions; the interface shows the snapshot SyncD reports
and sends it commands through `internal/syncd.Remote` (protocol 1.8, see [IPC](ipc.md)). The paired
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
  not offered; each account uses the one address chosen.
- **Signing in again.** An account needs it when SyncD reports it signed out, a renewal is under
  way, or one of its folders reports `AUTH_REQUIRED`.

## Main window

- **Title bar:** drawn by the view: the mark, the name, a handle that moves the window, and
  Windows 11's caption buttons.
- **Sidebar:** the first account (opens Accounts & folders), the four pages with the count of
  what waits on Activity, Settings, and the overall state as a dot and a word with the address
  in use (opens Network). The overall state is the first that applies: unknown, paused, needs you
  (a deletion review, an account to sign in again, a folder waiting or failing), syncing, offline,
  last sync finished (without `fullSync`), up to date.
- **Overview:** Pause sync / Resume sync and Check now; what sync does now, with the phase and
  the files so far rather than an invented total; the address in use and why; every item that
  needs the user, each with its one button; the folders.
- **Accounts & folders:** per account its sign-in state, Add a folder or Sign in in browser (then
  Open the browser again and Cancel while it waits), and in a menu Sign in again and Remove this
  account; its folders with their paths, state, the reason in words, and Open, Pause or Resume,
  Review, and in a menu Check now and Stop syncing. Removing a folder or an account asks first and
  says that the files stay on both sides; removing an account stops syncing its folders first.
- **Network:** Automatic or Always use one address (only with `endpointAutoSwitch`); the account's
  addresses, the one in use first and those that failed verification last, each with its two
  trust steps (approved, same server verified), Verify now, Use this one, a pending switch, and
  Remove this address; why the address in use was chosen; the recent switches. A switch is
  requested (`endpoints.setPolicy`) and shown pending until SyncD reaches a safe point.
- **Activity:** the deletion review (how many files, each direction with its count, what it covers
  when SyncD names it, when the proposal expires; Decide later first, Open folder, and Allow the
  deletions only after a second question); what else needs the user; the recent history, filtered.

## Dialogs

- **Add an address:** step 1 looks at the address without sending any credential; step 2 shows
  the certificate and the server and asks for the user's explicit permission; step 3, the
  verification, is SyncD's after approval. An invalid certificate cannot be approved; a refused
  address says why.
- **Questions:** removing, stopping and allowing deletions use an alert with Cancel first.

## First-run setup

Shown instead of the pages while SyncD reports no account, and for Add an account: Your server,
Sign in (Login Flow v2 in the browser; the interface never sees a password), Folders (encrypted
folders cannot be chosen; the chosen local folders are created empty, as SyncD inspects existing
ones only), Start (Start when I sign in to Windows and notifications, both on; an optional other
address). Nothing syncs before Start syncing. Add a folder opens the Folders step for an account
that exists.

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
| Diagnostics | SyncD's runtime reading (`diagnostics.snapshot`: versions, startup, epochs, counters); the support bundle SyncD makes (`diagnostics.export`), its path, Open folder and Copy path |

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
for a safe point (`PrepareUpdate`); while files move or a deletion waits for the user it is
postponed. An automatic install runs when the computer has been idle for 15 minutes or within
03:00, as chosen; one that failed is not tried again in the same run.

## Languages

Strings are in `locales/en.json` (the source) and `locales/zh-Hans.json`; `go tool mygo
generate` writes `locales/catalog_gen.go`. Traditional Chinese and Japanese follow.

## Tests

`go test ./internal/gui` renders every page and Settings page in both appearances and both
languages; checks the deletion review (Decide later keeps it, allowing asks twice), the Settings
pause and that resuming everything keeps a folder's own pause, that unknown state is neither empty
nor healthy, SyncD's real capabilities (no Automatic, no green, the file-sync notice), signing in
again for a refused password, and that every protocol reason code has words. With
`SYNCD_TEST_EXE` set, `TestRealDaemonInterface` drives the interface against the compiled SyncD
over the real pipe (setup on a fresh profile, pause, Diagnostics and its bundle, SyncD running
after the interface closes; with `NEXTCLOUD_TEST_URL`, a browser sign-in started and cancelled).
With `SCREENSHOT_DIR` set it writes each window as a PNG to compare with `design/`.
