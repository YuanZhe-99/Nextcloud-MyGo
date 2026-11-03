# 上游契约

Nextcloud-MyGo 依赖上游的哪些行为，以及每一点核对到什么程度。Nextcloud-MyGo 跟随各上游的最新发行版，
因此上游每次变化都要重新核对每一点。下列 Nextcloud Desktop 要点是 2026-11-03 从 `master` 分支和 `latest`
文档中阅读得出的，没有构建或运行任何东西；M0 会对照最新发行版逐条复核，并把“已阅读”替换为实际结果。

## Nextcloud Desktop

| 要点 | 影响 | 核对 |
|---|---|---|
| 根 CMake 构建提供 `BUILD_GUI` 与 `BUILD_LIBRARIES_ONLY` 开关 | 可以构建没有可见 GUI 的同步路径，但 `src/CMakeLists.txt` 仍会查找 Core 和 Network 之外的 Qt 模块 | 已阅读 |
| 关闭 GUI 时，根构建可能跳过上游测试目标 | 上游测试构建与服务构建分别验证 | 已阅读 |
| `nextcloudcmd`（`src/cmd/cmd.cpp`）直接创建 `SyncEngine` | 可参考初始化顺序，但它不是服务 | 已阅读 |
| `SyncJournalDb` 声明于 `src/common/syncjournaldb.h`；其命名函数的输入包含远程 URL | journal 路径必须独立于活跃端点而固定 | 已阅读 |
| `SyncEngine` 检查 `s_anySyncRunning` | 每个进程同一时刻只运行一个完整同步周期 | 已阅读 |
| `Folder` 与 `FolderMan`（`src/gui/`）引用 GUI 代码 | 需抽取其业务逻辑并替换交互点 | 已阅读 |
| 账号 URL 与凭据存储相互耦合（`account.cpp`、`httpcredentials.cpp`） | 调用 `Account::setUrl()` 不等于端点切换 | 已阅读 |
| 文件带有 GPL-2.0-or-later 及其他文件级许可证 | 见 [third-party.md](third-party.md) | 已阅读 |

## Nextcloud 服务器 API

| 要点 | 影响 | 核对 |
|---|---|---|
| Login Flow v2 打开系统浏览器并轮询取得应用密码 | 由 SyncD 执行登录；界面永远拿不到应用密码 | 已阅读 |
| WebDAV `oc:id` 带有实例命名空间；`oc:fileid` 只在实例内唯一 | 身份校验比较完整的 `oc:id`，从不只比较 `fileid` | 已阅读 |

## Qt

| 要点 | 影响 | 核对 |
|---|---|---|
| Qt 6 文档列出 Windows ARM64 配置 | 存在原生路径；完整依赖链只有在 ARM64 真机上才算得到证明 | 已阅读 |

## MyGo

| 要点 | 影响 | 核对 |
|---|---|---|
| MyGo 的发布文档列出 `windows/arm64` | 界面可以是原生 ARM64 | 已阅读；Go 模块在 Windows 11 ARM64 上构建并通过测试 |
| MyGo 是 Go 工具依赖（`go tool mygo …`） | 其版本随 `go.mod` 变化 | 已构建 |
