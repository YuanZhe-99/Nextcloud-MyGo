# Interface

The interface is a native MyGo application (`cmd/nextcloud-mygo`, `internal/gui`) for the person
using Nextcloud MyGo, built from the screens in `design/`. It holds no sync state of its own: SyncD
owns accounts, folders, addresses and decisions; the interface shows the snapshot SyncD reports
and sends it commands through `internal/syncd`. Until SyncD exists it runs against `syncd.Mock`,
which shows sample data and says so in a line at the top of every page.

## Threads and state

Every field of `gui.App` is read and written on the main thread. Views run there; a command to
SyncD runs on its own goroutine (`App.do`) and hands its result back through `App.later`, which
redraws the window and the tray. A command keeps its button disabled until it returns, and a
failure shows as a toast in words, never as a code. The interface's own settings are in
`gui.json` in `%AppData%\Nextcloud-MyGo`, written atomically; nothing about sync is kept there.

## Main window

- **Title bar:** drawn by the view: the mark, the name, a handle that moves the window, and
  Windows 11's caption buttons.
- **Sidebar:** the first account (opens Accounts & folders), the four pages with the count of
  what waits on Activity, Settings, and the overall state as a dot and a word with the address
  in use (opens Network). The overall state is the first that applies: paused, needs you
  (a deletion review, a folder waiting for review or sign-in), syncing, offline, up to date.
- **Overview:** Pause sync / Resume sync and Check now; what sync does now, with the phase and
  the files so far rather than an invented total; the address in use and why; every item that
  needs the user, each with its one button; the folders.
- **Accounts & folders:** per account its sign-in state, Add a folder or Sign in in browser, and
  its folders with their paths, state and Open, Pause or Resume, Review, and Stop syncing in a
  menu. Removing a folder or an account asks first and says that the files stay on both sides.
- **Network:** Automatic or Always use one address; the account's addresses, the one in use
  first and blocked ones last, each with its two trust steps (approved, same server verified);
  why the address in use was chosen; the recent switches.
- **Activity:** the deletion review (how many files, which way the deletion would go, what it
  covers, Restore them on this PC first, Delete on the server only after a second question,
  Decide later); what else needs the user; the recent history, filtered.

## Dialogs

- **Add an address:** step 1 looks at the address without sending any credential; step 2 shows
  the certificate and the server and asks for the user's explicit permission; step 3, the
  verification, is SyncD's after approval. An invalid certificate cannot be approved.
- **Questions:** removing, stopping and deleting on the server use an alert with Cancel first.

## First-run setup

Shown instead of the pages while there is no account, and for Add an account: Your server,
Sign in (Login Flow v2 in the browser; the interface never sees a password), Folders (encrypted
folders cannot be chosen), Start (Start when I sign in to Windows and notifications, both on;
an optional other address). Nothing syncs before Start syncing. Add a folder opens the Folders
step for an account that exists.

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
| About & updates | the update, check daily, install automatically (never, when idle, every day at 03:00), version, source code, licenses |
| Diagnostics | the logs folder; the support bundle, which SyncD makes |

## Tray and closing

- A click on the tray icon opens the flyout (state, what needs the user, the folders, Open,
  Pause, Check now, Close window only, Quit and stop syncing); a right click opens the same
  actions as a menu. Both call what the window calls.
- The close button keeps the app in the tray (said once) or, as General chooses, closes the
  window; either way SyncD keeps syncing. Only Quit and stop syncing stops it.
- The main window exists only while shown. Opened at sign-in with Start in the tray on, the app
  shows only the tray icon.

## Updates

Only signed release builds update themselves (`mygo.Updater.Enabled`); others say so. The
check runs daily while switched on. Installing first asks SyncD for a safe point
(`PrepareUpdate`); while files move or a deletion waits for the user it is postponed. An
automatic install runs when the computer has been idle for 15 minutes or within 03:00, as
chosen; one that failed is not tried again in the same run.

## Languages

Strings are in `locales/en.json` (the source) and `locales/zh-Hans.json`; `go tool mygo
generate` writes `locales/catalog_gen.go`. Traditional Chinese and Japanese follow.

## Tests

`go test ./internal/gui` renders every page and Settings page in both appearances and both
languages and checks the deletion review, the question before deleting on the server, and that
resuming everything keeps a folder's own pause. With `SCREENSHOT_DIR` set it writes each window
as a PNG to compare with `design/`.
