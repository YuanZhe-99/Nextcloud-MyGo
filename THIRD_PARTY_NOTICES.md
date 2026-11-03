# Third-party notices

Nextcloud-MyGo is licensed under GPL-3.0 (`LICENSE`). It includes or links the components below.
The inventory of reused code (repository, commit, files, modifications, obligations) is
[doc/en-us/third-party.md](doc/en-us/third-party.md); release builds regenerate the full list of
Go and native dependencies with their license texts.

## Go modules (interface)

| Component | License | Source |
|---|---|---|
| MyGo | MIT | https://github.com/egoist/mygo |
| purego (through MyGo) | Apache-2.0 | https://github.com/ebitengine/purego |
| go-text/typesetting (through MyGo) | Unlicense OR BSD-3-Clause | https://github.com/go-text/typesetting |
| golang.org/x/image, x/net, x/text | BSD-3-Clause | https://go.googlesource.com |
| Kage shader compiler, inside MyGo | Apache-2.0 | https://github.com/hajimehoshi/ebiten |
| MingCute icon, inside MyGo | Apache-2.0 | https://github.com/Richard9394/MingCute |

## Planned components (SyncD)

| Component | License | Used for |
|---|---|---|
| Nextcloud Desktop (pinned in M0) | GPL-2.0-or-later and other file-level licenses | sync engine, application icon |
| Qt, QtKeychain, TLS backend, SQLite | recorded when pinned in M0 | runtime of SyncD |
