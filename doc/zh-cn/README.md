# Nextcloud-MyGo 文档

**这些文档是代码的权威描述。** `doc/en-us/` 是源；`doc/zh-cn/` 逐文件镜像它。计划与未完成的工作不写在这里；
计划进行期间只存在于仓库的 `PLAN.md` 中。

Nextcloud-MyGo 是一款 Windows 桌面 Nextcloud 文件同步客户端。它在后台进程（SyncD）中复用 Nextcloud Desktop
同步引擎，并为同一台服务器增加受信任的备用地址及其间的安全切换；MyGo 界面通过 Named Pipe 控制它。
目标平台为 x64 与原生 ARM64 的 Windows 11。

| 文档 | 内容 |
|---|---|
| [upstream.md](upstream.md) | 上游契约与特殊行为：Nextcloud Desktop、Nextcloud 服务器 API、Qt、MyGo |
| [third-party.md](third-party.md) | 复用的代码与依赖、许可证与义务 |
| [platform-notes.md](platform-notes.md) | 各平台已实现与已验证的内容 |
| [development.md](development.md) | 环境、命令、开发流程 |
| [translation-guide.md](translation-guide.md) | 镜像规则与术语表 |
| [version-history.md](version-history.md) | 各版本及其固定的上游版本 |
| [functions/INDEX.md](functions/INDEX.md) | 各包与各模块的职责和关键声明 |

## 固定版本

| 组件 | 版本 | 依据 |
|---|---|---|
| Go | 1.27.3 | `go.mod` |
| MyGo | v0.7.4 | `go.mod` |
| Nextcloud Desktop | 在 M0 选定（发行标签与提交） | `build-lock.json` |
| Qt、QtKeychain、TLS 后端、SQLite | 在 M0 选定 | `build-lock.json` |
| MSVC、Windows SDK、CMake、Ninja | 在 M0 选定 | `build-lock.json` |
