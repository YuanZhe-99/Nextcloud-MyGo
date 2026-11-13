# 验收矩阵

`PLAN.md` §20.6 要求的验收矩阵。每一行的状态只来自概念文档中已记录的证据；没有记录的运行一律为
`not run`。它随每次结果更新，不是发布声明。

## 阅读说明

| 状态 | 含义 |
|---|---|
| `passed` | 已记录该用例的真实运行，且满足计划的通过条件 |
| `passed (simulated)` | 用户认可的模拟代替真实条件，其真实运行满足通过条件；“缺少”一栏写明只有真实条件才能证明的部分 |
| `partial` | 只有部分条件有证据，或对计划要求在真实服务器上完成的用例只有模拟、脚本化传输或单元测试 |
| `not run` | 没有运行记录 |
| `limitation` | 0.1.0 不证明也不宣称；原因见 [0.1.0 已知限制](#010-已知限制) |

状态值保持英文原样，不翻译。缩写：**ARM64** 为原生 Windows 11 ARM64；**x64e** 为在同一台 ARM64
主机上仿真运行的 x64 二进制，按用户决定计为 x64 证据；**35.0.2** 是最新的 Nextcloud 服务器版本，所有
用例都针对它运行（服务器层还针对上一个主版本 34.0.5 通过过一次，[开发](development.md#测试分层)）。
**服务器层**指在 ARM64 上针对一次性 35.0.2 容器运行的 `tools/test-all.ps1 -Tier server`；**真实管道**
指经 Named Pipe 针对已编译守护进程运行的 Go 测试。证据链接指向记录该次运行的文档。计划中只以列表
给出的性质（§20.5）在此按计划顺序编为 `P` 号。

## 基础同步和文件安全（§20.2）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| S01 | `passed` | [平台说明](platform-notes.md)：CLI、适配器和守护进程夹具的上传、下载及无变化周期（零次 DAV 内容请求，正对照通过）；ARM64 与 x64e，35.0.2 | 1,000 文件 manifest 数据集 |
| S02 | `passed` | [平台说明](platform-notes.md)：双向修改且 journal 复用；ARM64 与 x64e，35.0.2 | 大数据集的 manifest 内容核验 |
| S03 | `passed` | [平台说明](platform-notes.md)：两侧冲突内容均保留；ARM64 与 x64e，35.0.2；B14：在已安装的界面中显示并解决，ARM64 | 真正独立的第二客户端 |
| S04 | `passed` | [平台说明](platform-notes.md)：`file-safety` 在真实服务器上双向重命名和移动目录，保留服务器文件 ID，不重新传输内容，不触发删除审核；隔离服务器上 ARM64 1 分 6 秒、x64e 59 秒，共享服务器上两种架构均通过；此后每次 ARM64 服务器层均通过；B14：在已安装的界面中完成本地重命名 | 无 |
| S05 | `partial` | [平台说明](platform-notes.md)：经自有中继中断的传输恢复后哈希一致（`network-active` 的上传、下载和丢失的 PUT 响应；ARM64 与 x64e） | 中断期间修改已有文件；无故障转移的单纯断网 |
| S06 | `passed` | [平台说明](platform-notes.md)：`network-chunked` 的 112 MiB 上传在第二块中断后续传，不重发第一块（ARM64 2 分 55 秒，x64e 2 分 58 秒）；`network-large` 的 4,296 MiB 分块会话在 4 GiB 之后中断，续传后流式哈希一致（ARM64 28 分 35 秒）；`network-active` 的下载被中断后恢复；上游 `ChunkingNgTest` 22/22 | x64e 上的 `network-large`；4 GiB 以上的下载 |
| S07 | `passed` | [平台说明](platform-notes.md)：服务器已提交但响应被扣留的 PUT 及分块会话的最终 MOVE 经重新发现恢复，哈希、服务器文件 ID、ETag 和原 journal 一致；ARM64 与 x64e | 响应之外的阶段性故障注入 |
| S08 | `passed` | [平台说明](platform-notes.md)：`network-uncertain` 中已有条目的 MOVE 和 DELETE 已提交但响应丢失，另一客户端重新创建了路径，恢复时不再发送第二次 MOVE 或 DELETE，重新创建的对象保持不变（文件 ID 与 ETag 相同）；ARM64 12 分 31 秒，x64e 10 分 27 秒；此后每次 ARM64 服务器层均通过 | 无 |
| S09 | `partial` | [平台说明](platform-notes.md)：真实文件系统通知，周期内的 dirty 工作被保留（ARM64 1.27 秒，x64e 2.75 秒） | 真实服务器上、活动引擎在周期结束边界的事件 |
| S10 | `partial` | [平台说明](platform-notes.md)：真实缓冲区溢出请求完整核对；[引擎](engine.md) | 真实服务器上由核对发现遗漏的变化 |
| S11 | `partial` | [平台说明](platform-notes.md)：`file-safety` 在传播前把含十二个后代的单个目录作为 13 个远程删除搁置（比例阈值），只在批准后执行，ARM64 与 x64e；B14：在界面中审核 30 个文件中的 25 个；阈值计算见 `SyncDDeletionContracts`；100 个删除的用例已加入 `file-safety`（[平台说明](platform-notes.md#010-验收审计)），尚未运行 | 在服务器层运行 100 个删除的用例（只触发绝对阈值、本地方向） |
| S12 | `passed` | [平台说明](platform-notes.md)：`file-safety` 在本地删除含十二个后代的单个目录：提议为 13 个远程删除而不是 1 个，决定之前没有删除任何内容；ARM64 与 x64e，隔离服务器与共享服务器 | 无 |
| S13 | `partial` | [平台说明](platform-notes.md)：`file-safety` 中被租约持有的映射根无法被重命名移走（真实服务器，ARM64 与 x64e）；根不可用的契约测试保持工作待处理，根变化被拒绝 | 针对真实服务器、权限被拒绝的根，且无任何内容被传播 |
| S14 | `partial` | [平台说明](platform-notes.md)：`file-safety` 中返回 404 的远程根报告为 `REMOTE_ROOT_UNAVAILABLE`，所有本地文件保留；ARM64 与 x64e；契约中的 403/423/429 分类 | 真实服务器的维护页和部分 207 |
| S15 | `partial` | [引擎](engine.md)：每个周期前重新检查身份；根变化契约（[平台说明](platform-notes.md)） | 盘符复用、目录被替换、SUBST |
| S16 | `partial` | [平台说明](platform-notes.md)、[引擎](engine.md)：针对粘滞提交失败和保留损坏字节的上游回归（补丁 0011、0013）；损坏的 Control 数据库逐字节保留，并在真实管道上拒绝修改（`TestRealPipeUnavailableStopsINV11INV12`） | 守护进程在同步 journal 损坏时停止受影响写入；journal 错误注入 |
| S17 | `not run` | 没有运行记录 | 全部 |
| S18 | `not run` | [引擎](engine.md)描述了预检拒绝；没有运行记录 | 全部 |
| S19 | `partial` | [平台说明](platform-notes.md)：分层根准入，3 次重复/144 次竞争；ARM64 18.27 秒，x64e 26.27 秒 | 非 NTFS 拒绝、SUBST、发布中崩溃；跨会话是一项[限制](#010-已知限制) |
| S20 | `passed` | [平台说明](platform-notes.md)：`file-safety` 在真实服务器上再删除一个本地文件之后批准删除：过期的批准不删除任何内容，变化后的集合被重新提议，只在它自己的批准之后执行；ARM64 与 x64e；契约中旧 epoch 提案被拒绝 | 无 |
| S21 | `not run` | 没有运行记录 | 全部 |

## 网络、信任和切换（§20.3）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| N01 | `passed` | [平台说明](platform-notes.md)：同一服务器的两个地址完成登记；ARM64 与 x64e，35.0.2；B14 第 6 步：在已安装的界面中添加备用地址，发送任何凭据之前先显示证书和服务器，显示两者的应答时间，每次切换都带原因；评分见确定性契约 | 无 |
| N02 | `passed` | [平台说明](platform-notes.md)：两个真实隔离实例，UID 相同、命名空间不同，被拒绝并隔离；ARM64 与 x64e，35.0.2 | 无 |
| N03 | `partial` | [平台说明](platform-notes.md)：认可前拒绝核验；负向认可契约 | 抓包下可达的未登记端点；同名 SSID |
| N04 | `partial` | [引擎](engine.md)：TLS/身份硬失败被隔离；已连接回归 | 真实的过期、错误主机名和未知 CA 端点 |
| N05 | `partial` | [URL 使用审计](url-usage-audit.md)、[账号](accounts.md)：登录和元数据客户端从不跟随重定向；跨 origin 登录地址被拒绝 | 引擎路径上重定向到未知域名或 HTTP |
| N06 | `not run` | [后端进度](backend-progress.md)：未记录不同安装子路径 | 全部 |
| N07 | `limitation` | [引擎](engine.md)：不宣称严格 VPN 出口 | 0.1.0 不宣称 |
| N08 | `partial` | [引擎](engine.md)：绑定 generation 的探测；契约；固定 seed 的性质运行（P04） | 真实的快速网卡切换和晚到的 DNS |
| N09 | `partial` | [IPC](ipc.md)、[引擎](engine.md)：由调用方时钟驱动、带防抖的确定性策略 | 逐项列出的 margin、dwell 和 cooldown 用例 |
| N10 | `partial` | [引擎](engine.md)：三次独立失败后断路器打开；已连接回归；`network` 等待两个真实的断路器打开窗口 | 断言三次独立失败的真实运行 |
| N11 | `partial` | [平台说明](platform-notes.md)：健康传输延后契约，11/11；ARM64 与 x64e；B14：20 MiB 上传期间选定的切换等到传输结束；ARM64，35.0.2 | 健康大文件传输中出现更优地址（自动切换未开启） |
| N12 | `passed` | [平台说明](platform-notes.md)：中断的传输在排空后以新 epoch 故障转移到另一 origin；ARM64 与 x64e，35.0.2 | 无 |
| N13 | `partial` | [引擎](engine.md)：Control 回归中忽略旧 epoch 的完成结果 | 来自旧 epoch 的真实晚到请求 |
| N14 | `partial` | [平台说明](platform-notes.md)：两个架构上目标核验失败及重新核验后回到原 origin | 核验成功后首次业务失败 |
| N15 | `partial` | [引擎](engine.md)：全部不可用时等待、固定等待，不在坏地址间交替 | 真实的全部地址失效运行 |
| N16 | `not run` | 没有运行记录 | 全部 |
| N17 | `not run` | 没有运行记录 | 全部 |
| N18 | `partial` | [账号](accounts.md)、[引擎](engine.md)：限流存储；仅有脚本化传输回归 | 真实 429/Retry-After；运行中引擎的消费方 |
| N19 | `partial` | [IPC](ipc.md)、[平台说明](platform-notes.md)：根映射变化使备用端点核验失效 | 真实的新增目录运行 |
| N20 | `not run` | 没有运行记录 | 全部 |
| N21 | `passed` | [平台说明](platform-notes.md)：主/备/主切换保持原生 journal 文件，无变化周期请求检查；ARM64 与 x64e，35.0.2 | 无 |
| N22 | `partial` | [IPC](ipc.md)：策略契约中半开探测受限 | 真实运行中的多次网络事件 |
| N23 | `limitation` | [引擎](engine.md)：不宣称严格 VPN 出口 | 0.1.0 不宣称 |

## 生命周期、IPC 与升级（§20.4）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| R01 | `passed` | [平台说明](platform-notes.md)：客户端断开后守护进程继续运行，真实 Go↔C++ 管道，ARM64 与 x64e；B14：强制结束已安装的窗口，SyncD 继续运行，新窗口正确显示账号；ARM64，35.0.2 | x64e 下的强制结束 |
| R02 | `partial` | [平台说明](platform-notes.md)：未完成上传和下载期间终止进程；ARM64 | 发现和结束阶段；x64e |
| R03 | `passed` | [平台说明](platform-notes.md)：`switch-kill` 在每个持久切换阶段（`prepared`、`quiescing`、`verifying`、`committed`、`reconciling`）结束进程，并在同一 profile 上重启：前三个回到原绑定，后两个回到已提交的绑定，新 epoch，凭据引用、目录行和 journal 不变，已协调的内容不再发送；ARM64 与 x64e，隔离服务器与共享服务器；此后每次服务器层均通过 | 无 |
| R04 | `passed` | [平台说明](platform-notes.md)：真实守护进程运行中暂停在切换和恢复后仍有效；ARM64 与 x64e，35.0.2 | 无 |
| R05 | `partial` | [平台说明](platform-notes.md)：`multi-account`，一个守护进程上两个账号各两个目录，四个目录双向传输内容，一个账号切换时另一个继续同步且未被暂停；服务器层 ARM64；串行调度契约（[引擎](engine.md)） | 在持续变化下断言同一时间只有一个引擎周期的真实运行 |
| R06 | `limitation` | [平台说明](platform-notes.md)：跨会话检查尚未运行 | 第二个 Windows 登录会话 |
| R07 | `partial` | [账号](accounts.md)：重新认证与撤销的真实运行，35.0.2 | 密钥库锁定；服务器撤销 token |
| R08 | `passed` | [平台说明](platform-notes.md#010-验收审计)：真实管道上的 `TestRealPipeFramingR08R12`：在前缀和正文内部拆开的帧、一次写入中的两帧，以及六种畸形帧（零长度、超过 1 MiB、非法 UTF-8、重复键、截断的 JSON、超过 64 层的嵌套）各自只结束该连接，下一个客户端照常得到服务且状态保持，永远不完整的半帧在 15.0 秒后被断开；ARM64 16.2 秒，x64e 17.1 秒 | 无 |
| R09 | `partial` | [平台说明](platform-notes.md#010-验收审计)：`TestRealPipeImpostorR09`：先占用管道名的进程被客户端拒绝（可执行文件不符），并使守护进程无法以该名称启动；经由 `\\127.0.0.1\pipe\` 的客户端作为远程客户端被拒绝（拒绝访问）；意外的服务端路径被拒绝（`TestRealPipeINV12INV13`）；ARM64 与 x64e | 跨用户和跨会话：一项[限制](#010-已知限制) |
| R10 | `passed` | [平台说明](platform-notes.md#010-验收审计)：`TestRealPipeReplayR10R11R14`：响应从未被读取的暂停只提交了一次；同一个键在重连之后、之后的恢复之后以及守护进程重启之后再次发送，都返回同一个回执，且从未再次暂停；ARM64 1.2 秒，x64e 2.3 秒 | 无 |
| R11 | `passed` | [平台说明](platform-notes.md#010-验收审计)：同一个键配不同输入返回 `OPERATION_KEY_CONFLICT`，状态和修订号不变，真实管道；ARM64 与 x64e | 无 |
| R12 | `partial` | [IPC](ipc.md)：原子 bootstrap 与订阅；`TestRealPipeFramingR08R12`：bootstrap 之后立即提交的变化以更晚的 `seq` 到达订阅，真实管道 | 与 bootstrap 期间来自另一来源的状态变化之间的竞态 |
| R13 | `passed` | [平台说明](platform-notes.md#010-验收审计)：`TestRealPipeSlowClientR13`：从不读取的客户端因有界输出队列被断开（ARM64 在 2.3 秒后，x64e 在 8.6 秒后），下一个客户端在 5 秒内得到服务，`events.resume` 返回 `RESYNC_REQUIRED`；重启后重新 bootstrap（`TestRealPipeINV12INV13`） | 无 |
| R14 | `passed` | [平台说明](platform-notes.md#010-验收审计)：`TestRealPipeReplayR10R11R14`：主版本为 2 的 `session.hello` 返回 `PROTOCOL_MISMATCH`，协商之前的修改命令和 bootstrap 同样被拒绝且不执行任何操作；ARM64 与 x64e | 无 |
| R15 | `passed (simulated)` | [平台说明](platform-notes.md)：`sleep-resume`（[开发](development.md)），在一次被扣住的上传期间把整个 SyncD 进程挂起 35 秒，唤醒后只有备用地址可达：失效连接被判为失败，没有任何内容被当成已删除，在目录空闲时以原因 `unreachable` 切换，上传只完成一次，journal 与目录身份不变；ARM64，35.0.2。[引擎](engine.md)：唤醒通知使用可注入时钟测试；UTC 回拨不缩短限流截止时间（`syncd_constraint_tests`）。之后在空闲机器上的一次运行在“切换没有等到目录空闲”处失败，正在另一个会话中调查 | 真实休眠：电源状态切换及其通知到达运行中的服务进程、单调时钟是否计入休眠时间、网卡与地址变化、未收到重置就失效的连接、数小时的休眠、休眠期间卷的变化、现代待机（Modern Standby）；x64e |
| R16 | `partial` | [打包](packaging.md#签名更新)：被拒绝的下载（错误的密钥、改过的归档或清单、其他架构、版本不更新）以及在归档传输到一半时断开的连接（`TestUpdateInterruptedDownload`）都不留暂存内容，也不启动安装程序（单元测试）；应用内路径经 setup 与 `daemon.prepareUpdate` 在运行中的 0.1.0 上安装了签名的 0.1.1，配置状态保留，ARM64 | 真实安装中应用内路径的 setup 失败；由已安装的界面发起 |
| R17 | `partial` | [平台说明](platform-notes.md#010-验收审计)：`TestRealNewerControlSchemaR17`：给已编译的守护进程一个新一个 schema 的 Control 数据库，它只提供 `databaseUnavailable`，拒绝修改，并让文件逐字节保持不变（ARM64 2.3 秒，x64e 2.6 秒）；迁移备份拒绝更新的 schema（`SyncDStartupContracts`）；安装程序拒绝降级（单元测试） | 由更新的 Desktop 版本升级过的同步 journal 被旧版本打开 |
| R18 | `not run` | 没有运行记录 | 全部 |
| R19 | `partial` | [打包](packaging.md)：ARM64 当前用户安装、重装与卸载；开机启动项、快捷方式、应用列表条目与安装目录均被删除；官方客户端注册未变；B14：移除账号并卸载后同步文件保留，ARM64 | 账号仍登录时卸载；x64 包是一项[限制](#010-已知限制) |
| R20 | `passed` | [平台说明](platform-notes.md)：ARM64 真实运行与 B14：已安装的 ARM64 包（30 个 PE 文件均为原生 ARM64）通过公共受信 TLS 完成登录、同步、切换、重装和卸载，35.0.2；[打包](packaging.md)：依赖闭包；休眠与唤醒（§15.2 第 7 项）仅以模拟通过（R15） | 真实的休眠与唤醒（§15.2 第 7 项） |
| R21 | `not run` | 按用户决定由代替 24 小时运行的加速浸泡运行完成 | 全部 |
| R22 | `partial` | [IPC](ipc.md)：支持包只含脱敏字段；B14：导出的支持包不含密码、用户、服务器、路径或文件名 | 对日志做秘密扫描 |

## 性质（§20.5）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| P01 | `partial` | [平台说明](platform-notes.md)：S19 准入竞争，ARM64 与 x64e | 随机事件序列测试 |
| P02 | `partial` | [URL 使用审计](url-usage-audit.md)、[引擎](engine.md)：契约和真实运行中的 origin 门槛 | 随机测试 |
| P03 | `partial` | [引擎](engine.md)：绑定 epoch 的完成契约 | 随机测试 |
| P04 | `passed` | [平台说明](platform-notes.md#010-验收审计)：`SyncDPropertyContracts`，500 个固定 seed、每个 400 个随机策略事件：来自旧网络 generation、旧配置、硬失败或复核之前的票据，以及已使用的票据，都被拒绝且不改变任何健康字段；ARM64 0.18 秒，x64e 0.40 秒 | 无 |
| P05 | `partial` | [平台说明](platform-notes.md)：契约和真实运行中暂停仍有效 | 随机测试 |
| P06 | `partial` | [引擎](engine.md)：旧 epoch 提案被拒绝 | 随机测试 |
| P07 | `not run` | 没有运行记录 | 全部 |
| P08 | `passed` | [平台说明](platform-notes.md#010-验收审计)：同一次运行：任何自动或固定提议都从未指向被禁用、未认可、未核验、TLS 无效、在当前网络不允许、被隔离或因认证被阻止的端点，固定目标也从不回退到另一个端点；故意写错的模型在 seed 1 失败并缩减为两步 | 无 |
| P09 | `partial` | [IPC](ipc.md)：持久回执；真实运行中重启后仍保留（`TestRealPipeReplayR10R11R14`） | 随机测试 |

性质运行在失败时打印其 seed 和最小失败序列；`SYNCD_PROPERTY_SEED` 可重放单个 seed。

## 最终检查表（§23）

| 项目 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| 23.1-1 最新上游、依赖、补丁 | `passed` | [平台说明](platform-notes.md)、[后端进度](backend-progress.md)：Desktop v35.0.3 为最新版本（2026-11-12 核对），`build-lock.json` 中记录十七个补丁；带这些补丁的上游套件在 ARM64 上 95/96（失败的目标单独运行 3/3 通过），在 x64e 上 96/96，`SyncCfApiTest` 50 个通过、2 个跳过 | 无 |
| 23.1-2 原生 ARM64 引擎及依赖 | `passed` | [平台说明](platform-notes.md)：ARM64 真实运行、PE 扫描；B14：经检查的已安装包完成完整同步 | ARM64 上无 |
| 23.1-3 界面退出后继续同步 | `passed` | [平台说明](platform-notes.md)：断开后守护进程继续运行；B14：窗口关闭时及强制结束已安装窗口后继续同步 | 无 |
| 23.1-4 身份、凭据引用、journal 稳定 | `passed` | [平台说明](platform-notes.md)：每个 `switch-kill` 阶段、故障转移、模拟休眠和凭据更新前后，账号、凭据引用、目录行和原生 journal 文件均核对未变，ARM64 与 x64e（N21、R03） | 无 |
| 23.1-5 调度与阻塞语义 | `partial` | [引擎](engine.md)：调度契约；两个账号各两个目录的真实运行（R05） | 大规模公平性 |
| 23.1-6 origin、命名空间、TLS、重定向 | `partial` | [平台说明](platform-notes.md)：N02 已通过 | N04、N05、N06 |
| 23.1-7 评分、防抖、断路器、原因 | `partial` | [引擎](engine.md)：确定性契约；P04 与 P08 已通过；界面中显示切换原因（B14） | 由评分和防抖驱动的真实切换（自动切换未开启） |
| 23.1-8 故障注入下的切换状态机 | `passed` | [平台说明](platform-notes.md)：在全部五个持久切换阶段终止进程（R03），已提交的 PUT、MOVE、DELETE 和最终 MOVE 的响应丢失（S07、S08），中断的传输与故障转移（N12），均为真实运行 | 无 |
| 23.1-9 冲突、删除保护、根身份 | `partial` | [平台说明](platform-notes.md)：冲突、比例阈值下的删除审核、后代和过期批准均已真实运行（S03、S12、S20） | 100 个对象的 S11、S13、S14、S15、S18、S21 |
| 23.1-10 错误分类、有界重试 | `partial` | [引擎](engine.md)：分类与退避；真实的 404 与配额已满 | 真实 429、维护页 |
| 23.2-1 schema、方法、事件、错误、版本 | `partial` | [IPC](ipc.md)：协议 1.10，跨主版本的版本协商在真实管道上被拒绝（R14） | 冻结协议 1.10 的发布 |
| 23.2-2 真实 Go/C++ Named Pipe | `passed` | [平台说明](platform-notes.md)：真实管道测试；ARM64 与 x64e | 无 |
| 23.2-3 重连后幂等命令 | `passed` | [平台说明](platform-notes.md#010-验收审计)：真实管道上的 R10、R11，回执在重连和重启后仍然有效 | 无 |
| 23.2-4 快照与事件竞态、背压 | `partial` | [平台说明](platform-notes.md#010-验收审计)：R13 已通过；R12 为 partial | R12 中与另一来源的竞态 |
| 23.2-5 前端不持有秘密或数据库 | `partial` | [界面](gui.md)：界面只经 IPC | 对已连接界面的扫描 |
| 23.2-6 认可、删除确认、重新认证可完成 | `partial` | [IPC](ipc.md)：后端真实运行；B14（[平台说明](platform-notes.md)）：登录、地址认可、删除审核和冲突在已安装的界面中完成 | 通过界面重新认证 |
| 23.3-1 名称和图标 | `partial` | [打包](packaging.md#与官方客户端共存)：显示名称为 **Nextcloud MyGo**（开始菜单快捷方式、应用列表条目、窗口标题、界面文字），标识保持为 `Nextcloud-MyGo`（目录、注册表键、可执行文件、管道名）；可执行文件图标取自上游图标；B14：显示了托盘图标 | 商标核对 |
| 23.3-2 包架构和依赖 | `partial` | [打包](packaging.md)：两个包均已检查（ARM64 28 个、x64 29 个 PE 文件，均为原生）；B14：ARM64 包经检查并运行 | 安装 x64 包：一项[限制](#010-已知限制) |
| 23.3-3 安装、升级、降级、卸载、共存 | `partial` | [打包](packaging.md)：ARM64 安装、同版本升级、卸载与共存；降级拒绝由单元测试覆盖；B14：在运行中且已登录的副本上同版本重装，账号和目录保留；安装包冒烟测试：在运行中的 SyncD 上经 `daemon.prepareUpdate` 从 0.1.0 升级到测试版本 0.1.1，核对了配置状态与启动报告（上一版本、schema、自检通过），ARM64 | 带已登录账号的版本间升级；x64 是一项[限制](#010-已知限制) |
| 23.3-4 矩阵、内容核验、24 小时报告 | `not run` | 本文档；24 小时运行由加速浸泡运行完成 | 浸泡运行报告 |
| 23.3-5 仅在可信链就绪后自动更新 | `partial` | [打包](packaging.md#签名更新)：只有用保存在 GitHub secret 中的 `mygo keygen` 密钥构建的包才有更新；应用只安装用 `mygo.json` 中公钥对应密钥签名的归档，用户开启之前保持关闭；安装包冒烟测试：用一次性密钥，从环回 HTTP 获取签名的 0.1.1 测试更新并安装到 0.1.0 之上，ARM64 | 用项目密钥签名并从 GitHub 读取的 release；Authenticode 是一项[限制](#010-已知限制) |
| 23.3-6 §17.1 桌面功能 | `partial` | [平台说明](platform-notes.md)：B14：托盘图标与弹出面板、关闭到托盘、从开始菜单重新打开、“退出并停止同步”；安装程序在停止 SyncD 前调用 `daemon.prepareUpdate`（安装包冒烟测试） | 真实删除等待时 `daemon.prepareUpdate` 的拒绝；通知；登录时启动；真实休眠是一项[限制](#010-已知限制) |
| 23.3-7 许可证、源码、限制、诊断说明 | `partial` | [平台说明](platform-notes.md)：已知限制；本页的限制列表 | 用户指南；发布源码材料 |
| 23.3-8 未验证能力不列为已支持 | `partial` | [能力证据](#能力证据)：`fullSync`、`endpointAutoSwitch` 和 `virtualFiles` 保持 `false` | 发布清单 |

## 里程碑退出条件（§18）

| 里程碑 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| M0 可行性与审计 | `partial` | [平台说明](platform-notes.md)：原生 ARM64 真实基本同步，35.0.2；[后端进度](backend-progress.md)称 M0 未完成 | 其余审计；macOS File Provider 审计不在 0.1.0 范围内；原生 x64 是一项[限制](#010-已知限制) |
| M1 契约、账号、单端点 | `partial` | [IPC](ipc.md)、[账号](accounts.md)：两目录同步、登录、IPC 无秘密；真实管道上的 R08、R10、R11、R13、R14 | 整体退出条件尚未确认 |
| M2 可靠性和文件安全 | `partial` | [引擎](engine.md)：调度、根租约、删除否决；S04、S06–S08、S12、S20 已通过 | S05、S09–S11、S13–S18、S21 |
| M3 端点登记与只读策略 | `partial` | [IPC](ipc.md)：认可、命名空间核验、策略；N01、N02 已通过 | N04、N06；N07 是一项[限制](#010-已知限制) |
| M4 安全自动切换 | `partial` | [平台说明](platform-notes.md)：手动切换和故障转移已真实运行，不确定的写入得到恢复（S07、S08），每个切换阶段被终止后都能恢复（R03）；自动切换能力为 `false` | 自动切换的证据（[能力证据](#能力证据)）；VPN 是一项[限制](#010-已知限制) |
| M5 前端接入与首版发布 | `partial` | [平台说明](platform-notes.md)：ARM64 上的 B14 联合验收，35.0.2，九步中六步通过；B14 发现的问题已在 SyncD 和安装程序中修复 | 通过界面重新运行第 2、6、8 步；带账号的版本间升级；浸泡运行 |
| M6 现代网盘框架 | `partial` | [按需文件](on-demand.md)、[平台说明](platform-notes.md)：Windows 按需注册、按需下载、释放空间、始终保留、切换边界、崩溃恢复和注销，在原生 ARM64 与 x64e 上通过引擎运行时针对服务器 35.0.2 运行；Cloud Files 无法应答的主机跳过而不是通过 | 配对守护进程与界面、另一个 origin、占位文件下的删除保护与离线、卸载时注销；`virtualFiles` 为 false；macOS 部分按用户 2026-11-11 的决定不在 0.1.0 范围内 |

## 联合发布验收（B14）

原生 Windows 11 ARM64、已安装的包、由 UI Automation 驱动的真实窗口、Nextcloud 35.0.2，
2026-11-12；细节、发现的问题和能力证据见[平台说明](platform-notes.md)。

| 步骤 | 状态 | 核对方式 |
|---|---|---|
| 1 安装、设置、登录、目录、首次同步 | `passed` | 界面；设置的“开始”一步未经过 |
| 2 本地和远端改动、“活动” | `partial` | 界面和状态；时间为 UTC；协议 1.10 增加 `deletedLocal`、`renamedRemote` 与 `renamedLocal`（`testCycleActivity`），界面已能对照 Mock 显示（`TestActivityKinds110`），尚未在真实周期中显示 |
| 3 批量删除审核 | `passed` | 界面和服务器 |
| 4 冲突 | `passed` | 界面和文件 |
| 5 目录暂停经过全部暂停和全部恢复 | `passed` | 界面和服务器 |
| 6 地址与切换 | `partial` | 界面；不提供固定/自动；超过排空窗口的切换现在以 `switchFailed` 结束（`switch-quota`，ARM64，35.0.2），未在界面中重新运行 |
| 7 关闭、重新打开、从托盘退出 | `passed` | 界面和进程 |
| 8 在运行中的副本上重装 | `partial` | 界面和进程；此后安装程序调用 `daemon.prepareUpdate`，自检不再因 `CANCELLED` 失败（`SyncDFolderErrorContracts`），安装包冒烟测试中 0.1.0 → 0.1.1 测试升级通过；未在界面中重新运行 |
| 9 移除账号、卸载 | `passed` | 界面、注册表、服务器令牌列表 |
| 休眠与唤醒 | `passed (simulated)` | 针对后端运行 `sleep-resume`，未经过界面或已安装的包（R15）；用户已决定不让这台机器真实休眠 |

## 能力证据

三项能力都保持 `false`；只有用户能决定开启其中某一项。每一项已有的证据和开启前缺少的内容：

| 能力 | 证据 | 开启前缺少 |
|---|---|---|
| `fullSync` | S01–S04、S06–S08、S12、S20 在真实服务器上通过（多数为 ARM64 与 x64e）；冲突、删除审核、暂停和恢复经过已安装的界面（B14）；两个账号各两个目录（R05 为 partial）；守护进程在界面退出后继续运行（R01、23.1-3） | S05、S09–S11、S13–S18 和 S21；浸泡运行（R21）；真实周期中本地删除和重命名的“活动”（B14 第 2 步） |
| `endpointAutoSwitch` | 手动和固定切换已真实运行，延后到安全点（N11、B14 第 6 步）；带排空和新 epoch 的故障转移（N12）；每个切换阶段被终止后都能恢复（R03）；不确定的写入得到恢复（S07、S08）；策略上的 P04 与 P08；模拟休眠以原因 `unreachable` 切换（R15，调查中） | 自动策略的真实运行：由评分、margin、dwell 和 cooldown 驱动切换的 N08–N11、N13–N17、N19、N20 和 N22；界面提供自动模式；真实端点上的 N04 与 N05 |
| `virtualFiles` | Windows 按需目录经引擎运行时在真实服务器上运行：占位文件、按需下载、释放空间、始终保留、等待按需下载的切换、按需下载期间终止进程以及注销，ARM64 与 x64e（M6） | 配对守护进程与界面选择模式；另一个 origin；占位文件下的删除保护与离线行为；卸载时注销；用于缩略图和叠加图标的 Shell 扩展 |

## 0.1.0 已知限制

本次发布不证明也不宣称的用例或用例的一部分，原因是用户的决定，或者仅有的这台机器无法证明：

| 项目 | 原因 |
|---|---|
| N07、N23 严格 VPN 出口 | 0.1.0 不宣称：按路由排除候选需要真实的 VPN 路径，而没有记录过；任何端点都不会凭接口名称被接受 |
| R06 以及 R09、S19 中跨会话的部分 | 需要这台机器上的第二个 Windows 登录会话或用户账户，本次运行不创建它们 |
| 真实休眠与唤醒（R15、R20、§15.2 第 7 项） | 用户已决定不让这台机器真实休眠；`sleep-resume` 是认可的模拟 |
| 原生 x64 硬件与安装 x64 包（23.3-2、23.3-3、R19） | 只有一台 ARM64 机器：x64 二进制在仿真下运行并计为 x64 证据，但 x64 安装程序按设计拒绝在 ARM64 上运行 |
| Authenticode（23.3-5） | 没有代码签名证书：首次安装时 SmartScreen 会警告；更新仍使用 MyGo 更新密钥签名 |
| macOS（M0 审计、M6 File Provider） | 按用户 2026-11-11 的决定不在 0.1.0 范围内 |
| 24 小时运行（R21、23.3-4） | 由约一小时的加速浸泡运行代替，单独记录 |
