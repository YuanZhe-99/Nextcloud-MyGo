# Interface design

The screens the MyGo interface is built from, exported from the design canvas as standalone HTML:
open any file in a browser; the links between screens work. They are a reference for layout,
wording and states, not code: the interface is native MyGo, and its behavior follows `PLAN.md`
§13 and §17. Sample names, addresses and numbers are illustrations.

| Screen | File |
|---|---|
| Overview | [Main.html](Main.html) |
| Overview, dark theme | [OverviewDark.html](OverviewDark.html) |
| Network: addresses, trust steps, why this address, recent switches | [Network.html](Network.html) |
| Accounts & folders | [Folders.html](Folders.html) |
| Activity and the deletion review | [Activity.html](Activity.html) |
| Add an address (approve, then verify) | [AddAddress.html](AddAddress.html) |
| Tray flyout | [Tray.html](Tray.html) |
| First-run setup: welcome, sign in, folders, start | [SetupWelcome.html](SetupWelcome.html), [SetupSignIn.html](SetupSignIn.html), [SetupFolders.html](SetupFolders.html), [SetupFinish.html](SetupFinish.html) |
| Settings: General, About & updates | [SettingsGeneral.html](SettingsGeneral.html), [SettingsUpdates.html](SettingsUpdates.html) |

## Tokens

| Token | Light | Dark |
|---|---|---|
| Window base (Mica) | `#E9EEF3` | `#1A1C21` |
| Pane | `#FFFFFF` | `#23262C` |
| Border | `#DCE2E8` | `#33363E` |
| Text | `#1B1D21` | `#EDEFF2` |
| Secondary text | `#4A5260` | `#A9AFBA` |
| Accent (fills with white text) | `#0072B5` | `#4DAEE8` (with dark text) |
| Accent text and links | `#0069A6` | `#7CC6F2` |
| Success | `#0F7B3F` | `#4CC38A` |
| Needs attention | `#8A4B00` on `#FFF6E5` | `#F0B65A` on `#352812` |
| Danger | `#B42318` | — |

Type is Segoe UI Variable (Windows' own); sizes 12 / 13 / 14 body, 15 section titles, 20–28
page titles. Radii: 6 controls, 8 panes and cards, 10 dialogs. The window keeps an 8-point margin
around its pane, like Kyokai and Easytier-MyGo. The logo drawn here is a placeholder for the
Nextcloud icon, which comes from the upstream release.
