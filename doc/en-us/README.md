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
| [version-history.md](version-history.md) | Releases and the upstream versions they pin |
| [functions/INDEX.md](functions/INDEX.md) | Per-package and per-module roles and key declarations |

## Pinned versions

| Component | Version | Source of truth |
|---|---|---|
| Go | 1.27.3 | `go.mod` |
| MyGo | v0.7.4 | `go.mod` |
| Nextcloud Desktop | chosen in M0 (release tag and commit) | `build-lock.json` |
| Qt, QtKeychain, TLS backend, SQLite | chosen in M0 | `build-lock.json` |
| MSVC, Windows SDK, CMake, Ninja | chosen in M0 | `build-lock.json` |
