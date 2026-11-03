# Nextcloud-MyGo

**Nextcloud file sync that follows you across networks**

Nextcloud-MyGo is a Windows desktop client for Nextcloud file sync. It reuses the proven sync engine
of the official Nextcloud Desktop client and adds what a home or small-office server needs: one
account reachable through several trusted addresses of the same server — public, LAN, VPN — and
safe, automatic switching between them, without rebuilding the sync database or re-transferring
unchanged files. It runs natively on Windows 11 x64 and ARM64.

> **Status:** early development. The interface runs on Windows 11 against sample data; the sync
> service (SyncD) is not built yet, so nothing syncs. See [PLAN.md](PLAN.md) (in Chinese) and the
> [platform notes](doc/en-us/platform-notes.md).

## Features

| Feature | Status |
|---|---|
| Sign in with the system browser (Login Flow v2, app password kept in Windows' credential store) | planned |
| Two-way sync of ordinary local folders: add, change, rename, delete, conflicts | planned |
| Sync continues in the background after the window closes; pause, resume, check now | planned |
| Trusted alternative addresses of the same server (WAN, LAN, VPN), each acknowledged and verified | planned |
| Automatic, explainable endpoint choice and switching at safe points | planned |
| Deletion protection and clear conflict handling | planned |
| Native Windows 11 ARM64 and x64 | planned |

Virtual files (CFAPI), Explorer integration, end-to-end encrypted folders, Linux and macOS come
after the first release.

## Architecture

Two processes released as one verified combination:

- **SyncD** — a C++/Qt background process of the current user. It links the upstream Nextcloud
  Desktop sync engine and owns sync, credentials, endpoint choice and recovery.
- **The interface** — a Go + [MyGo](https://github.com/egoist/mygo) application, talking to SyncD
  over a Named Pipe with JSON-RPC 2.0. It never touches sync data or credentials directly.

Nextcloud-MyGo keeps its own configuration, identity and update channel, separate from the official
client.

## Building from Source

Builds are not available yet. Requirements and commands: [doc/en-us/development.md](doc/en-us/development.md).

## Documentation

- [English documentation](doc/en-us/README.md) · [中文文档](doc/zh-cn/README.md)

## Acknowledgements

- **[Nextcloud Desktop](https://github.com/nextcloud/desktop)** — for its open-source sync engine.
- **[MyGo](https://github.com/egoist/mygo)** — for its cross-platform desktop application framework.

## Open Source Licenses

Nextcloud-MyGo is licensed under [GPL-3.0](LICENSE). Third-party components and their licenses are
listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
[doc/en-us/third-party.md](doc/en-us/third-party.md). Nextcloud and the Nextcloud logo are
trademarks of Nextcloud GmbH; this project is not affiliated with or endorsed by Nextcloud GmbH.
