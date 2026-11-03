# Nextcloud-MyGo documentation

**These docs are the authoritative description of the code.** `doc/en-us/` is the source;
`doc/zh-cn/` mirrors it file for file. Plans and open work are not documented here; while the plan
is active it lives only in the repository's `PLAN.md`.

Nextcloud-MyGo is a Windows desktop client for Nextcloud file sync. It reuses the Nextcloud Desktop
sync engine in a background process (SyncD) and adds trusted alternative addresses of the same
server with safe switching between them; a MyGo interface controls it over a Named Pipe. It targets
Windows 11 on x64 and native ARM64.

| Document | Covers |
|---|---|
| [upstream.md](upstream.md) | Upstream contracts and quirks: Nextcloud Desktop, Nextcloud server APIs, Qt, MyGo |
| [third-party.md](third-party.md) | Reused code and dependencies, licenses and obligations |
| [platform-notes.md](platform-notes.md) | What is implemented and verified on each platform |
| [development.md](development.md) | Environment, commands, the dev loop |
| [translation-guide.md](translation-guide.md) | Mirror rules and the glossary |
| [version-history.md](version-history.md) | Releases and the upstream versions they used |
| [functions/INDEX.md](functions/INDEX.md) | Per-package and per-module roles and key declarations |

## Upstream versions

Go, MyGo and Nextcloud are not pinned: they always follow their latest stable release. The table
shows the latest release at the last check (2026-11-03); `build-lock.json` records what each build
actually used.

| Component | Policy | Latest at last check |
|---|---|---|
| Go | latest release | 1.27.3 |
| MyGo | latest tag | v0.7.4 |
| Nextcloud Desktop | latest release tag | v35.0.2 |
| Qt, QtKeychain, TLS backend, SQLite | combination chosen in M0, recorded | not chosen yet |
| MSVC, Windows SDK, CMake, Ninja | combination chosen in M0, recorded | not chosen yet |
