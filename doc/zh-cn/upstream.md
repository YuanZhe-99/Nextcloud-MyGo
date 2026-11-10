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
| `Account::setCapabilities` 在 `create(false)` 后仍会重新进入状态/推送设置 | 补丁 0015 在适配器安装已验证同步能力元数据前保留无头生命周期 | 窄回归在修复前失败；集成构建待完成 |
| 文件带有 GPL-2.0-or-later 及其他文件级许可证 | 见 [third-party.md](third-party.md) | 已阅读 |

### 发行版审计：Desktop v35.0.2

M0 检出为 `third_party/nextcloud` 中的提交
`07761410a90a2f27007932fcb61afc23ddda5998`。以下源码发现取代上方对应的 master 分支
初步观察；均不代表运行时验收结果。

- `src/cmd/cmd.cpp:427` 使用 `QCoreApplication`；`--confdir` 路径通过
  `ConfigFile::setConfDir` 应用。686–697 行从 URL 派生 journal 名称，因此原 CLI 可用于
  隔离的单地址 M0 检查，不能证明身份独立于端点。

- `SyncEngine::finalize` 在清理 `_propagator` 前发出 `finished`，并通过 `deleteLater`
  安排 discovery 销毁。适配器必须等待信号之后的清理并追踪剩余任务，才能确认静止
  （INV-01、INV-05、INV-06）。
- `aboutToPropagate` 后直接构造 propagator 并调用 `start`，没有拒绝分支。
  连接观察者不能实现产品的删除保护。
- `promptUserBeforePropagation` 将 `bool cancel` 传给 `cancelSyncOrContinue`：**true 表示取消**。
  回调使用 QPointer 守卫及延迟销毁；适配器必须自行保证回答仅消费一次，不能依赖延迟销毁前
  第二次调用必然被拒绝。
- `handleMassDeletion` 在命令行同步模式关闭提示。M0 命令行运行必须局限于可丢弃的测试数据；
  SyncD 不得继承此绕过行为。
- `AbstractCredentials` 提供 `createQNAM`、`fetchFromKeychain`、`persist`、`invalidateToken`
  与 `forgetSensitiveData`。原 HTTP 实现用运行时 URL 构造凭据库键，并可能迁移或删除旧键。
  SyncD 需要稳定引用适配器，不能使用这些持久化路径（INV-02、INV-03）。
- `HttpCredentialsAccessManager::createRequest` 在未设置 `DontAddCredentialsAttribute` 时
  添加 Basic Authorization；自身不检查已认可 origin。产品边界必须在注入凭据前验证目标
  （INV-03）。
- `AccessManager::createRequest` 默认手动处理重定向，但接受已指定的重定向策略。
  它还记录请求 URL 和响应头。服务必须在认证测试前强制执行重定向策略并抑制或脱敏上游网络日志。
- `AccessManager::createRequest` 默认禁用 HTTP/2（`accessmanager.cpp:106–110`）。SyncD 的
  origin 绑定管理器必须保留该稳定 HTTP/1.1 默认值，而非静默继承 Qt 的 HTTP/2 默认值。
  扣留 HTTP/2 加密下行流量也可能扣留流控帧，因此 PUT 阻塞并不能证明成功响应丢失。
- `SyncJournalDb` 接受显式数据库路径。这是固定 journal 的接入点；跨端点上传、下载恢复
  尚未证明（INV-02、INV-08、INV-09）。

### 现代云文件框架

Windows 实现在 `src/libsync/vfs/cfapi`（`vfs_cfapi`、`hydrationjob`、`cfapiwrapper`）及其
`shellext` 子树。`CfApiWrapper` 提供注册与注销操作；`VfsCfApi` 在生命周期中调用它们。
这些会真实改变系统，因此 M0 必须保持插件未激活。产品专属注册标识、下载静止边界及完整清理
仍属 M6 工作；此处尚未运行上游实现。

macOS 扩展目标在 `shell_integration/MacOSX/NextcloudIntegration/FileProviderExt`。
其桩文件将实际实现指定为 `shell_integration/MacOSX/NextcloudFileProviderKit` 中的
`NextcloudFileProviderKit.FileProviderExtension`；GUI 侧桥接位于 `src/gui/macOS`。
未经网络和 journal 所有权审计而复用目标，不足以证明计划中的单写者 XPC 边界。
签名、App Groups、GUI 耦合及平台验收仍未验证。这些源码保留自身 SPDX 文件头及随附许可证文本。

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
