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
| `PropagateRemoteMove::slotMoveJobFinished` 在 MOVE 因任何原因失败时（包括传输失败）把本地文件改回旧名，并在 journal 中记录旧名 | 地址故障期间做的本地重命名会被撤销：两侧都保留旧名，内容不丢失，但用户的重命名丢失；未打补丁（未解决，[验收](acceptance.md) R21） | 已阅读；加速浸泡运行中出现三次，ARM64，35.0.2，2026-11-13 |

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
SyncD 为按需目录复用该插件（[按需文件](on-demand.md)）；Desktop v35.0.3 的以下要点决定了集成方式
（已阅读，并由按需运行覆盖）：

- 同步根 ID 为 `provider!SID!账号显示名!alias`，而显示名是 `user@host[:port]`：端点切换会把同一目录注册到
  第二个注册项下并遗留第一个。补丁 0017 增加 `VfsSetupParams::syncRootAccountId`。
- `createSyncRootRegistryKeys` 总会写入 Shell 扩展的处理类和导航窗格类。不构建扩展时，这些 ID 在本机未注册，
  或属于已安装的官方客户端。补丁 0017 只在扩展已注册时写入它们。
- 当某提供程序的同步根全部移除时，`unregisterFolder` 会删除 HKCU 下的 Shell 扩展类注册项；这些类 ID 是上游
  自己的，因此可能删除另一套安装的注册。SyncD 只调用 `CfApiWrapper::unregisterSyncRoot`。
- 按需下载和目录填充回调运行在 Cloud Files 线程上，在本地事件循环中等待 VFS 线程上排队的工作，并读取
  `params().account`。在回调运行期间断开或销毁 VFS，会让该线程永久等待。补丁 0017 增加由回调自身计数的
  `Vfs::refuseNewRequests` 与 `Vfs::hasRequestsInFlight`。
- 新的远程目录成为按需目录（`postProcessServerNew`）；列举它时会请求提供程序通过 `Account::listRemoteFolder`
  填充，这发生在同步周期之外，并在 VFS 线程上写 journal。来自提供程序自身进程的请求会被拒绝。
- 插件及其 Shell 扩展受 `BUILD_SHELL_INTEGRATION` 控制（补丁 0001）；补丁 0017 为单独构建插件增加
  `BUILD_CFAPI_VFS`。
- `Folder::setVirtualFilesEnabled` 会删除并重建 journal，因此不提供切换目录模式。

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
| MOVE 保留条目的 `oc:fileid`；分块会话的最终 MOVE 在文件组装完成后才应答；已提交的写入即使回复丢失也仍然已提交 | 上游发现按文件 ID 识别已改名的条目、按校验和识别已组装的文件，因此回复丢失通过重新发现恢复，绝不通过第二个请求恢复 | 针对服务器 35.0.2 的真实运行 |
| 替换已有文件的 PUT 直接覆盖写入原文件，不使用 part 文件（当 part 路径不可创建而目标可更新时，`File::put` 放弃 part 文件，而 `Common::isCreatable` 使每个文件都满足这一条件）；正文被截断时文件也被截断，服务器察觉后应答 `400 Expected filesize`，文件缓存在有人读取该文件之前保留旧的大小和 ETag。新文件的 PUT 使用 part 文件，不留下任何内容 | 补丁 0018 把每个非空修改都作为分块上传发送，由最终 MOVE 在服务器上组装（[引擎](engine.md#完成与安全)） | 针对服务器 35.0.2 手工发送的短 PUT：已有的 1 MiB 文件只剩 294,912 字节，新文件未被创建 |
| 后备目录已消失的 `files_external` 存储，在客户端所请求属性的父目录列表中被省略，或以权限 `MG`、大小 0 和空 ETag 列出，其自身列表应答 207 且没有成员 | 若一个周期会在本地移除挂载点或其全部内容，SyncD 会阻止该周期（`DISCOVERY_INCOMPLETE`） | 针对服务器 35.0.2 的真实运行（`partial-listing`）和手工 PROPFIND |

## Qt

| 要点 | 影响 | 核对 |
|---|---|---|
| Qt 6 文档列出 Windows ARM64 配置 | 存在原生路径；完整依赖链只有在 ARM64 真机上才算得到证明 | 已阅读 |
| HTTP 代理在 `CONNECT` 之后不作回答就关闭连接：GET 立即失败（`ProxyConnectionClosedError`），但带请求体的请求（PROPFIND）会等到传输超时，同一管理器之后的每个请求也一样，即使代理已恢复回答；回答 `503` 时两者都立即失败（`ConnectionRefusedError`） | 夹具中继对处于故障中的源回答 `503`（[开发](development.md#测试分层)）；只关闭连接的真实代理会让一个周期等到上游超时，之后下一个周期的新管理器（[引擎](engine.md#完成与安全)）使其恢复 | `build-lock.json` 的 Qt 6，ARM64，2026-11-13，浸泡探测 |

## MyGo

| 要点 | 影响 | 核对 |
|---|---|---|
| MyGo 的发布文档列出 `windows/arm64` | 界面可以是原生 ARM64 | 已阅读；Go 模块在 Windows 11 ARM64 上构建并通过测试 |
| MyGo 是 Go 工具依赖（`go tool mygo …`） | 其版本随 `go.mod` 变化 | 已构建 |
