# Nextcloud-MyGo 文档

**这些文档是代码的权威描述。** `doc/en-us/` 是源；`doc/zh-cn/` 逐文件镜像它。计划与未完成的工作不写在这里；
计划进行期间只存在于仓库的 `PLAN.md` 中。

Nextcloud-MyGo 是一款 Windows 桌面 Nextcloud 文件同步客户端。它在后台进程（SyncD）中复用 Nextcloud Desktop
同步引擎，并为同一台服务器增加受信任的备用地址及其间的安全切换；MyGo 界面通过 Named Pipe 控制它。
目标平台为 x64 与原生 ARM64 的 Windows 11。

| 文档 | 内容 |
|---|---|
| [gui.md](gui.md) | 界面：页面、首次设置、设置、托盘、关闭、更新、语言 |
| [upstream.md](upstream.md) | 上游契约与特殊行为：Nextcloud Desktop、Nextcloud 服务器 API、Qt、MyGo |
| [third-party.md](third-party.md) | 复用的代码与依赖、许可证与义务 |
| [platform-notes.md](platform-notes.md) | 各平台已实现与已验证的内容 |
| [development.md](development.md) | 环境、命令、开发流程 |
| [translation-guide.md](translation-guide.md) | 镜像规则与术语表 |
| [version-history.md](version-history.md) | 各版本及其实际使用的上游版本 |
| [functions/INDEX.md](functions/INDEX.md) | 各包与各模块的职责和关键声明 |

## 上游版本

Go、MyGo 与 Nextcloud 不固定版本，始终跟随各自最新的稳定发行版。下表为上次检查（2026-11-03）时的最新版本；
`build-lock.json` 记录每次构建实际使用的版本。

| 组件 | 策略 | 上次检查时的最新版本 |
|---|---|---|
| Go | 最新发行版 | 1.27.3 |
| MyGo | 最新标签 | v0.8.1 |
| Nextcloud Desktop | 最新发行标签 | v35.0.2 |
| Qt、QtKeychain、TLS 后端、SQLite | 在 M0 选定组合并记录 | 尚未选定 |
| MSVC、Windows SDK、CMake、Ninja | 在 M0 选定组合并记录 | 尚未选定 |
