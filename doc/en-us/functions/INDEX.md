# Function index

One row per Go package and backend module: its role and the declarations to read first.
Declarations carry the Function Explanation Layer in their doc comments (first sentence: purpose;
then inputs, returns, side effects and notes where non-trivial).

| Package / module | Role | Start with |
|---|---|---|
| `cmd/nextcloud-mygo` | The interface's program: single instance, tray, the window, the Mock until SyncD exists | `main` |
| `internal/gui` | Window, pages, setup, Settings, tray, updates ([gui.md](../gui.md)) | `App`, `App.view`, `App.do`, `Prefs`, `overallOf` |
| `internal/syncd` | The contract with SyncD as the interface sees it, and the Mock | `Client`, `Snapshot`, `Mock` |
| `locales` | Interface strings (en, zh-Hans) and their generated accessors | `en.json`, `In` |
| `internal/version` | Product version | `Version`, `Older` |
| `tools/checkdocs` | Documentation mirror check | `main` |
