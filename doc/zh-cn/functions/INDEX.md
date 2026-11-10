# 函数索引

SyncD 连通基础已实现；完整同步尚未完成。见 [IPC](../ipc.md) 和[后端进度](../backend-progress.md)。

每个 Go 包与后端模块一行：其职责及应先阅读的声明。各声明的文档注释带有函数说明层（第一句说明用途；
较复杂的声明再写输入、返回、副作用与注意事项）。

| 包 / 模块 | 职责 | 先读 |
|---|---|---|
| `cmd/nextcloud-mygo` | 单实例、托盘、窗口、配套 SyncD 及显式演示模式 | `main` |
| `internal/gui` | 窗口、页面、首次设置、设置、托盘、更新（[gui.md](../gui.md)） | `App`、`App.view`、`App.do`、`Prefs`、`overallOf` |
| `internal/syncd` | 经过身份验证的管道客户端、快照投影、命令重试、支持取消的操作等待及显式 Mock | `Client`、`Remote`、`dialPipe`、`Snapshot`、`Mock` |
| `backend/` | 控制线程守护进程、持久账号/设置/回执、原生凭据、登录、Win32 传输及独立切换守卫 | `Control`、`LoginFlow`、`writeCredential`、`PipeServer`、`SwitchCoordinator`、`decodeMessage` |
| `backend/control_reauth.cpp` | 保留账号的凭据替换、原生凭据库回滚/清理及同 origin 运行时重新加载 | `reauthenticate`、`installReauthentication`、`recoverReauthentication`、`reloadCredentials` |
| `backend/engine*`, `backend/scheduler.*`, `backend/root*` | 进程内引擎/运行时、排空后的账号重新绑定、重试/安全阻塞、确定性串行队列及根租约（[引擎](../engine.md)） | `EngineAdapter`、`rebindError`、`rebind`、`EngineRuntime`、`quiesceAccount`、`Scheduler`、`RootLease` |
| `backend/control_folders.cpp`, `backend/namespace.*`, `backend/deletion.*` | 持久映射、完成持久化失败时安全关闭、有界 DAV 元数据及独立权限/锁定/限流结果、传播前提案及单次批准 | `folderMutation`、`restoreFolders`、`cycleFinished`、`MetadataError`、`checkMetadataReply`、`parseCollections`、`readCollections`、`assessDeletion`、`decisionMutation` |
| `backend/control_endpoints.cpp`, `backend/control_switch.cpp` | 持久 origin 许可、只读 UID/全部根验证及门槛保护的手动激活 | `endpointMutation`、`probeEndpoint`、`switchMutation`、`advanceSwitch`、`verifySwitchTarget` |
| `backend/endpoint_policy.*` | 确定性的仅提议评分、代次隔离、去抖、隔离及熔断恢复 | `EndpointPolicy`、`beginProbe`、`observe`、`choose` |
| `backend/control_network.cpp`, `backend/network_*` | 原生通知排空、有界健康序列、持久策略、冻结切换证据及新 epoch 恢复提案 | `NetworkMonitor`、`evaluateNetwork`、`networkOutcome`、`policyMutation`、`switchEvidence`、`switchFailed` |
| `backend/network_tunnel.*`, `backend/network_transfer_acceptance.cpp` | 仅验收使用的有界加密中继及真实映射传输故障，父进程负责远程清理 | `NetworkTunnel`、`networkTransferAcceptance`、`cleanupNetworkContent` |
| `backend/control_diagnostics.cpp` | 不含路径/凭据的运行时计数及原子、有界本地支持包 | `diagnosticsProjection`、`exportDiagnostics` |
| `backend/startup*` | 配置所有权、单调时钟崩溃/退避准入、诊断 IPC、延迟激活及已验证迁移备份 | `Startup`、`engineAllowed`、`recordStopIntent`、`finishClean`、`backupControlBeforeMigration`、`Control::activate`、`startupDispatch` |
| `backend/control_activity.cpp` | 有界、按游标过滤的已确认条目汇总及已提交切换历史 | `activityProjection`、`recordActivity` |
| `backend/watcher*` | Windows 递归变化通知、溢出核对及取消排空 | `RootWatcher`、`ready`、`arm` |
| `backend/request_lifetime.*` | 回复完成/销毁计数及粘滞的结果不明写入观察 | `RequestLifetime`、`observe`、`Counts` |
| `backend/resume_context.*` | 只读 ETag 下载、带校验和单 PUT 及分块 v2 会话准入；保留被阻塞轮询/v1 分块索引及损坏 journal | `resumeContextError` |
| `backend/switch_state.*` | 事务化切换意图/epoch 存储、已提交绑定解析及重启门槛 | `SwitchState`、`begin`、`commit`、`reconciled`、`interrupt` |
| `protocol/` | 协议 1.7 清单、schema 及共享恶意输入夹具 | `contract.json`、`message.schema.json`、`folders.schema.json`、`endpoints.schema.json`、`switch.schema.json`、`reauthentication.schema.json`、`policy.schema.json`、`startup.examples.json`、`constraints.examples.json`、`fixtures.json` |
| `tools/build-syncd.ps1` | ARM64/x64 SyncD 构建与核心契约测试 | 脚本参数 |
| `locales` | 界面字符串（en、zh-Hans）及生成的访问函数 | `en.json`、`In` |
| `internal/version` | 产品版本 | `Version`、`Older` |
| `tools/checkdocs` | 文档镜像检查 | `main` |
| `tools/build-upstream.ps1` | 对记录中的干净上游快照执行 M0 引擎与测试构建 | 脚本参数与依赖前缀检查 |
| `patches/nextcloud/` | 记录上游构建、生命周期、UTC 日期及测试隔离修正 | `build-lock.json` 中的有序哈希；后端进度中的证据 |
| `tools/test-upstream.ps1` | 隔离的真实服务器 M0 测试与清理 | `Invoke-TestCycle`、`Invoke-TestDav` |
| `tools/test-discovery-lifetime.ps1` | 发现工作线程存活超过超时夹具销毁阶段的 M0 回归测试 | 有界子进程重复执行上游超时和缺失数据用例 |
| `tools/test-login/`, `tools/test-login.ps1` | 真实配对守护进程授权、策略、切换、重新认证、活动内容传输崩溃/取消、journal 身份及自有资源验收 | `exerciseSync`、`exercisePolicy`、`exerciseActiveCrash`、`exerciseActiveSwitch`、`exerciseReauthentication`、`Approve` |
| `tools/test-reauth-lifetime.ps1` | 带父进程崩溃后原生凭据清理的只读取消/替换生命周期回归 | `replacedDuringVerification`、`cleanupManifest`、`RunOwned` |
