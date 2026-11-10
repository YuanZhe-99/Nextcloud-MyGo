# Third-party code

Nextcloud-MyGo is licensed under GPL-3.0. This inventory lists every upstream it reuses or embeds,
what it uses, and what that obliges. `THIRD_PARTY_NOTICES.md` holds the notices themselves.

## Inventory

| Component | Repository and version | Used for | License | Modifications | Obligations |
|---|---|---|---|---|---|
| MyGo | `egoist/mygo`, latest tag | UI framework, packaging | MIT | none | keep the copyright notice; its embedded Kage compiler and MingCute icon are Apache-2.0 |
| Kyokai and Easytier-MyGo files | `YuanZhe-99/Kyokai`, `YuanZhe-99/Easytier-MyGo` | `tools/checkdocs`, the `internal/version` pattern, the MyGo maintenance skill, the conventions | AGPL-3.0 / GPL-3.0, relicensed under GPL-3.0 by their author | adapted names and comments | none beyond Nextcloud-MyGo's own license |
| Nextcloud Desktop | `nextcloud/desktop`, latest release tag | the sync engine (`libsync`, `common`) linked into SyncD; the application icon | GPL-2.0-or-later and other file-level licenses | patches in `patches/nextcloud/` only | keep file headers; ship the license texts, the corresponding source and the build patches |

## Trademarks

The unmodified Desktop v35.0.3 snapshot is the Git submodule at `third_party/nextcloud`,
commit `809fadb79180eeacb3fd5b83c804765822e2cd09`. Its original license files remain there.
`build-lock.json` records this audit input; it does not yet describe a verified SyncD build.

The Nextcloud name and icon are trademarks of Nextcloud GmbH. The product name and the reuse of the
icon follow the user's decision; the trademark terms are checked before the first release.

## Planned components

Qt, QtKeychain, the TLS backend and SQLite enter this inventory with their exact versions and
licenses when M0 chooses them and records them in `build-lock.json`.
