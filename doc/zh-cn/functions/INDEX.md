# 函数索引

SyncD 连通基础已实现；完整同步尚未完成。见 [IPC](../ipc.md) 和[后端进度](../backend-progress.md)。

每个 Go 包与后端模块一行：其职责及应先阅读的声明。各声明的文档注释带有函数说明层（第一句说明用途；
较复杂的声明再写输入、返回、副作用与注意事项）。

| 包 / 模块 | 职责 | 先读 |
|---|---|---|
| `cmd/nextcloud-mygo` | 单实例、托盘、窗口、配套 SyncD 及显式演示模式 | `main` |
| `internal/gui` | 窗口、页面、首次设置、设置、托盘、更新（[gui.md](../gui.md)） | `App`、`App.view`、`App.do`、`Prefs`、`overallOf` |
| `internal/syncd` | 经过身份验证的管道客户端、快照投影、命令重试、支持取消的操作等待及显式 Mock；针对已编译守护进程、使用恶意原始客户端的真实管道测试 | `Client`、`Remote`、`dialPipe`、`Snapshot`、`Mock`、`rawPipe` |
| `backend/` | 控制线程守护进程、持久账号/设置/回执、原生凭据、登录、Win32 传输及独立切换守卫 | `Control`、`LoginFlow`、`writeCredential`、`PipeServer`、`SwitchCoordinator`、`decodeMessage` |
| `backend/control_reauth.cpp` | 保留账号的凭据替换、原生凭据库回滚/清理及同 origin 运行时重新加载 | `reauthenticate`、`installReauthentication`、`recoverReauthentication`、`reloadCredentials` |
| `backend/engine*`, `backend/scheduler.*`, `backend/root*` | 进程内引擎/运行时、排空后的账号重新绑定、按需请求静止、重试/安全阻塞、确定性串行队列及根租约（[引擎](../engine.md)） | `EngineAdapter`、`rebindError`、`rebind`、`EngineRuntime`、`quiesceAccount`、`Scheduler`、`RootLease` |
| `backend/control_folders.cpp`, `backend/namespace.*`, `backend/deletion.*` | 持久映射及其固定的文件模式、完成持久化失败时安全关闭、按步骤设截止时间的有界 DAV 元数据及独立权限/锁定/限流结果、传播前提案及单次批准、消失的外部存储按未知扣住 | `folderMutation`、`restoreFolders`、`cycleFinished`、`MetadataError`、`checkMetadataReply`、`metadataDeadline`、`parseCollections`、`readCollections`、`assessDeletion`、`emptiesMount`、`decisionMutation` |
| `backend/control_endpoints.cpp`, `backend/control_switch.cpp` | 持久 origin 许可、只读 UID/全部根验证及门槛保护的手动激活 | `endpointMutation`、`probeEndpoint`、`switchMutation`、`advanceSwitch`、`verifySwitchTarget` |
| `backend/control_update.cpp` | 更新安全点（`daemon.prepareUpdate`）、启动后报告与自检，以及不修改配置的快照事件 | `prepareUpdate`、`advanceUpdate`、`releaseUpdate`、`startupReport`、`selfCheck`、`publishState` |
| `backend/endpoint_policy.*`、`backend/property_tests.cpp` | 确定性的仅提议评分、代次隔离、去抖、隔离及熔断恢复；带缩减的固定 seed 随机性质测试（P04、P08） | `EndpointPolicy`、`beginProbe`、`observe`、`choose`、`run`、`shrink` |
| `backend/control_network.cpp`, `backend/network_*` | 原生通知排空、有界健康序列、持久策略、冻结切换证据及新 epoch 恢复提案 | `NetworkMonitor`、`evaluateNetwork`、`networkOutcome`、`policyMutation`、`switchEvidence`、`switchFailed` |
| `backend/network_tunnel.*`, `backend/network_acceptance.h`, `backend/network_transfer_acceptance.cpp` | 仅验收使用的有界加密中继（限速、扣留回复、丢弃回复、延迟回复）、慢速服务器健康用例、真实映射传输故障、回复丢失的 MOVE/DELETE/最终 MOVE 用例、切换阶段杀进程及重启检查、文件安全用例、模拟休眠（由子进程中继挂起整个进程），父进程负责远程清理 | `NetworkTunnel`、`TransferFaults`、`SwitchFacts`、`networkTransferAcceptance`、`switchKillAcceptance`、`switchRecoverAcceptance`、`sleepRelay`、`cleanupNetworkContent` |
| `backend/multi_account_acceptance.cpp` | 仅验收使用、在一次性服务器上运行的 2 个账号 × 2 个目录：拒绝重复账号、各账号独立绑定、一个账号的切换不暂停其他账号、服务器配额已满时守护进程继续服务且固定策略能提交、移除一个账号，第二个服务器用户由父进程清理；以及 switch-quota 运行：上传被挂起超过排空窗口时请求的切换以 `switchFailed` 结束，配额已满时再次请求则完成切换 | `multiAccountAcceptance`、`switchQuotaAcceptance`、`AcceptanceCalls`、`cleanupOwnedUser` |
| `backend/switching_acceptance.cpp` | 仅验收使用、在一次性服务器上运行的自动切换用例（`-Scenario switching`），每个用例独立进程：一个本机回环的 TLS 终止代理，使用只在测试进程内受信任的一次性证书（已过期、主机名错误、未知 CA、另一安装路径、401、429、重定向、拒绝连接、迟到的应答）；挂起上传期间出现更优地址、提交后首次业务失败、切换期间吊销、备用地址无法核验的新目录、被替换的远程根 | `switchingScenario`、`switchingFixture`、`switchingAcceptance`、`SwitchingControls`、`TlsProxy` |
| `backend/data_safety_acceptance.cpp` | 仅验收使用、在一次性服务器上运行的数据安全与恢复用例（`-Scenario data-safety`），每个用例独立进程：两个地址都不可用时被中断的修改、监视器边界与溢出、被拒绝和被替换的根目录、维护模式、消失的外部存储、困难名称、被排除的目录树、吊销、损坏的 journal、发现期间被结束，以及 MOVE 在连接处被切断的重命名 | `dataSafetyAcceptance`、`dataSafetyBeforeRestart`、`dataSafetyRecover`、`renameCut`、`Mapping`、`converge` |
| `backend/switching_tests.cpp` | 调用方时钟上的切换契约（N08 抖动与迟到结果、N09 逐行阈值、N10、N15、N22）以及带缩减的固定 seed 性质测试：单个引擎周期与暂停准入（P01、P05）、epoch（P03、P06）、请求闸门（P02）、跨重启的持久回执（P09） | `margins`、`property`、`schedulerProperties`、`switchProperties`、`originProperty`、`durableProperty` |
| `backend/control_diagnostics.cpp` | 不含路径/凭据的运行时计数及原子、有界本地支持包 | `diagnosticsProjection`、`exportDiagnostics` |
| `backend/startup*` | 配置所有权、单调时钟崩溃/退避准入、诊断 IPC、延迟激活及已验证迁移备份 | `Startup`、`engineAllowed`、`recordStopIntent`、`finishClean`、`backupControlBeforeMigration`、`Control::activate`、`startupDispatch` |
| `backend/control_activity.cpp` | 有界、按游标过滤的已确认条目汇总（双向的传输、删除与重命名）及切换历史：已提交的切换与被拒绝的用户请求 | `activityProjection`、`recordActivity` |
| `backend/watcher*` | Windows 递归变化通知、溢出核对及取消排空 | `RootWatcher`、`ready`、`arm` |
| `backend/request_lifetime.*` | 回复完成/销毁计数及粘滞的结果不明写入观察 | `RequestLifetime`、`observe`、`Counts` |
| `backend/resume_context.*` | 只读 ETag 下载、带校验和单 PUT 及分块 v2 会话准入；保留被阻塞轮询/v1 分块索引及损坏 journal | `resumeContextError` |
| `backend/switch_state.*` | 事务化切换意图/epoch 存储、已提交绑定解析及重启门槛 | `SwitchState`、`begin`、`commit`、`reconciled`、`interrupt` |
| `backend/platform/windows/ondemand*` | Cloud Files 同步根注册、归属检查、切换所等待的按需请求闸门、注销，以及真实服务器按需验收程序（[按需文件](../on-demand.md)） | `OnDemandRoot`、`ownershipError`、`close`、`busy`、`unregister`、`EngineAdapter::quiescent` |
| `protocol/` | 协议 1.10 清单、schema 及共享恶意输入夹具 | `contract.json`、`message.schema.json`、`folders.schema.json`、`endpoints.schema.json`、`switch.schema.json`、`reauthentication.schema.json`、`policy.schema.json`、`startup.examples.json`、`constraints.examples.json`、`fixtures.json` |
| `tools/build-syncd.ps1` | ARM64/x64 SyncD 构建与核心契约测试 | 脚本参数 |
| `tools/pkg`、`tools/package.ps1`、`tools/package-smoke.ps1` | 带必需许可证包的 DLL 闭包暂存、安装包清单、原生架构 PE 检查、运行中 SyncD 的启动报告、测试版本安装包、更新签名以及真实安装、升级与应用内更新测试（[打包](../packaging.md)） | `stageCommand`、`requireNotices`、`reportCommand`、`signUpdateCommand`、`check`、`inspect`、`native` |
| `packaging/windows/setup` | 按版本目录的当前用户安装、替换文件前的更新安全点、协调的双进程停止、卸载及可选的用户数据删除 | `install`、`stopApp`、`prepare`、`prepareUpdate`、`shutdown`、`uninstallApp`、`removeTree`、`admit`、`extract` |
| `internal/release` | `tools/pkg` 写入、安装器校验的安装包清单；签名更新：其归档与清单、检查、下载与签名校验、交给 setup（[打包](../packaging.md#签名更新)） | `Manifest`、`Parse`、`Verify`、`SignUpdate`、`CheckUpdate`、`Update.Download`、`StartSetup` |
| `locales` | 界面字符串（en、zh-Hans）及生成的访问函数 | `en.json`、`In` |
| `internal/version` | 产品版本 | `Version`、`Older` |
| `tools/checkdocs` | 文档镜像检查 | `main` |
| `tools/bootstrap-deps.ps1` | 按记录的版本与提交精确组装原生依赖前缀（Qt、vcpkg 端口、ECM、QtKeychain、KDSingleApplication、KArchive、libp11、pkgconf、许可证文本）（[CI](../ci.md)） | `Get-Source`、`CMakeProject`、标记文件与 `env-<arch>.json` |
| `tools/licenses.ps1` | 根据安装包实际构建所用的前缀和 Go 模块生成其许可证与声明包，并指明对应源代码 | `SOURCE.txt`、`Copy-Into` |
| `.github/workflows/` | 推送与拉取请求的 CI、带缓存的原生 x64 与 ARM64 构建，以及 `v*` 标签的预发布（[CI](../ci.md)） | `build.yml`、`ci.yml`、`release.yml` |
| `tools/build-upstream.ps1` | 对记录中的干净上游快照执行 M0 引擎与测试构建 | 脚本参数与依赖前缀检查 |
| `patches/nextcloud/` | 记录上游构建、生命周期、UTC 日期及测试隔离修正 | `build-lock.json` 中的有序哈希；后端进度中的证据 |
| `tools/test-upstream.ps1` | 隔离的真实服务器 M0 测试与清理 | `Invoke-TestCycle`、`Invoke-TestDav` |
| `tools/test-discovery-lifetime.ps1` | 发现工作线程存活超过超时夹具销毁阶段的 M0 回归测试 | 有界子进程重复执行上游超时和缺失数据用例 |
| `tools/test-login/`, `tools/test-login.ps1` | 真实配对守护进程授权、策略、切换、重新认证、活动内容传输崩溃/取消、journal 身份及自有资源验收，分四个并行通道 | `mapFolders`、`exerciseLocal`、`exerciseEndpoints`、`exercisePolicy`、`awaitRemote`、`exerciseActiveCrash`、`exerciseActiveSwitch`、`exerciseReauthentication`、`Approve` |
| `tools/test-reauth-lifetime.ps1` | 只读生命周期回归及所有真实网络场景的父进程：每次运行使用全新配置目录，执行 `switch-kill` 的杀进程与重启序列，并在崩溃后清理原生凭据和远程内容 | `replacedDuringVerification`、`cleanupManifest`、`RunOwned`、`RunProfile` |
| `tools/test-all.ps1` | 唯一的测试入口：按架构运行快速层（无服务器）或服务器层（一次性容器、并行通道），每个场景一行并给出耗时，总时长有硬性预算 | `Step`、`Lanes`、`Acceptance` |
| `tools/test-server.ps1` | 服务器层使用的一次性本地 Nextcloud 服务器：一个带标签的容器、夹具本地 TLS 根证书，按状态文件删除 | `Runtime`、`-Start`、`-Stop` |
| `backend/soak_acceptance.cpp`、`tools/test-soak/`、`tools/test-soak.ps1` | 加速浸泡运行（R21）：一个守护进程、2 个账户 × 2 个文件夹，计时器加快 24 倍，重复的变更、故障、续期、一次重启和界面管道连接；每个周期的哈希收敛、传输、删除和切换计数；30 秒资源采样和斜率阈值 | `soakAcceptance`、`SoakControls`、`Run::once`、`Run::converge`、`NetworkTestAccess::accelerate`、`NetworkTunnel::totals`、`Half`、`Fit` |
| `tools/test-job.ps1` | 所有夹具包装脚本的关闭即终止 Job Object，守护进程、探针和测试可执行文件都不会比其脚本存活更久 | `Enter-OwnedJob`、`Invoke-InOwnedJob` |
