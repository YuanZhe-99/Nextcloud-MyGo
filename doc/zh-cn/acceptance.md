# 验收矩阵

`PLAN.md` §20.6 要求的验收矩阵。每一行的状态只来自概念文档中已记录的证据；没有记录的运行一律为
`not run`。它随每次结果更新，不是发布声明。

## 阅读说明

| 状态 | 含义 |
|---|---|
| `passed` | 已记录该用例的真实运行，且满足计划的通过条件 |
| `partial` | 只有部分条件有证据，或对计划要求在真实服务器上完成的用例只有模拟、脚本化传输或单元测试 |
| `not run` | 没有运行记录 |
| `not possible on this hardware` | 仅有的这台机器无法证明 |

状态值保持英文原样，不翻译。缩写：**ARM64** 为原生 Windows 11 ARM64；**x64e** 为在同一台 ARM64
主机上仿真运行的 x64 二进制；**35.0.2** 是唯一测试过的 Nextcloud 服务器版本。证据链接指向记录该次
运行的文档。计划中只以列表给出的性质（§20.5）在此按计划顺序编为 `P` 号。

## 基础同步和文件安全（§20.2）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| S01 | `passed` | [平台说明](platform-notes.md)：CLI、适配器和守护进程夹具的上传、下载及无变化周期（零次 DAV 内容请求，正对照通过）；ARM64 与 x64e，35.0.2 | 1,000 文件 manifest 数据集；原生 x64 |
| S02 | `passed` | [平台说明](platform-notes.md)：双向修改且 journal 复用；ARM64 与 x64e，35.0.2 | 大数据集的 manifest 内容核验 |
| S03 | `passed` | [平台说明](platform-notes.md)：两侧冲突内容均保留；ARM64 与 x64e，35.0.2 | 真正独立的第二客户端 |
| S04 | `not run` | 未记录真实服务器上的重命名或目录移动（[平台说明](platform-notes.md)）；上游套件运行在模拟服务器上 | 真实服务器上的两个方向 |
| S05 | `partial` | [平台说明](platform-notes.md)：经自有中继中断的传输恢复后哈希一致（ARM64 12 分 26 秒，x64e 14 分 28 秒） | 已有文件的修改；无故障转移的单纯断网 |
| S06 | `partial` | [平台说明](platform-notes.md)：112 MiB 上传在第二块中断后续传，哈希一致（ARM64 2 分 52 秒，x64e 2 分 56 秒）；上游 `ChunkingNgTest` 22/22 | 其他中断点、下载、4 GiB 以上文件 |
| S07 | `passed` | [平台说明](platform-notes.md)：服务器已提交但确认被扣留的 PUT 经重新发现恢复，哈希和原 journal 一致；ARM64 与 x64e | 被扣留的最终 MOVE；阶段性故障注入 |
| S08 | `not run` | [后端进度](backend-progress.md)将不确定的 MOVE/DELETE 列为未完成 | 全部 |
| S09 | `partial` | [平台说明](platform-notes.md)：真实文件系统通知，周期内的 dirty 工作被保留（ARM64 1.27 秒，x64e 2.75 秒） | 真实服务器上、活动引擎在周期结束边界的事件 |
| S10 | `partial` | [平台说明](platform-notes.md)：真实缓冲区溢出请求完整核对；[引擎](engine.md) | 真实服务器上由核对发现遗漏的变化 |
| S11 | `partial` | [平台说明](platform-notes.md)：传播前阻断删除、一次性批准、已使用的决定被拒绝；ARM64 与 x64e，35.0.2 | 大规模下 100 个及 10 个/20% 阈值；方向和数量核对 |
| S12 | `not run` | [引擎](engine.md)描述了后代计数；没有运行记录 | 全部 |
| S13 | `partial` | [引擎](engine.md)、[平台说明](platform-notes.md)：根不可用的契约测试保持工作待处理，根变化被拒绝 | 针对真实服务器移除或拒绝访问根，且无任何内容被传播 |
| S14 | `not run` | 仅有合成的 403/423/429 分类（[平台说明](platform-notes.md)） | 真实服务器上的 404、维护页、部分 207 |
| S15 | `partial` | [引擎](engine.md)：每个周期前重新检查身份；根变化契约（[平台说明](platform-notes.md)） | 盘符复用、目录被替换、SUBST |
| S16 | `partial` | [平台说明](platform-notes.md)、[引擎](engine.md)：针对粘滞提交失败和保留损坏字节的上游回归（补丁 0011、0013；96/96） | 守护进程在 journal 损坏时停止受影响写入；journal 错误注入 |
| S17 | `not run` | 没有运行记录 | 全部 |
| S18 | `not run` | [引擎](engine.md)描述了预检拒绝；没有运行记录 | 全部 |
| S19 | `partial` | [平台说明](platform-notes.md)：分层根准入，3 次重复/144 次竞争；ARM64 18.27 秒，x64e 26.27 秒 | 非 NTFS 拒绝、跨会话、SUBST、发布中崩溃 |
| S20 | `partial` | [平台说明](platform-notes.md)、[引擎](engine.md)：契约和守护进程夹具中旧 epoch 与已使用的提案被拒绝 | 真实服务器上确认后数据再变化 |
| S21 | `not run` | 没有运行记录 | 全部 |

## 网络、信任和切换（§20.3）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| N01 | `partial` | [平台说明](platform-notes.md)：同一服务器的两个地址完成登记；ARM64 与 x64e，35.0.2；评分见确定性契约 | 通过已连接界面展示可解释的评分 |
| N02 | `passed` | [平台说明](platform-notes.md)：两个真实隔离实例，UID 相同、命名空间不同，被拒绝并隔离；ARM64 与 x64e，35.0.2 | 无 |
| N03 | `partial` | [平台说明](platform-notes.md)：认可前拒绝核验；负向认可契约 | 抓包下可达的未登记端点；同名 SSID |
| N04 | `partial` | [引擎](engine.md)：TLS/身份硬失败被隔离；已连接回归 | 真实的过期、错误主机名和未知 CA 端点 |
| N05 | `partial` | [URL 使用审计](url-usage-audit.md)、[账号](accounts.md)：登录和元数据客户端从不跟随重定向；跨 origin 登录地址被拒绝 | 引擎路径上重定向到未知域名或 HTTP |
| N06 | `not run` | [后端进度](backend-progress.md)：未记录不同安装子路径 | 全部 |
| N07 | `not run` | [引擎](engine.md)：不宣称严格 VPN 出口 | 全部 |
| N08 | `partial` | [引擎](engine.md)：绑定 generation 的探测；契约 | 真实的快速网卡切换和晚到的 DNS |
| N09 | `partial` | [IPC](ipc.md)、[引擎](engine.md)：由调用方时钟驱动、带防抖的确定性策略 | 逐项列出的 margin、dwell 和 cooldown 用例 |
| N10 | `partial` | [引擎](engine.md)：三次独立失败后断路器打开；已连接回归 | 真实的三次失败运行 |
| N11 | `partial` | [平台说明](platform-notes.md)：健康传输延后契约，11/11；ARM64 与 x64e | 健康大文件传输中出现更优地址 |
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
| N23 | `not run` | [引擎](engine.md)：不宣称严格 VPN 出口 | 全部 |

## 生命周期、IPC 与升级（§20.4）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| R01 | `partial` | [平台说明](platform-notes.md)：客户端断开后守护进程继续运行，真实 Go↔C++ 管道，ARM64 与 x64e | 强制结束真实 MyGo 窗口 |
| R02 | `partial` | [平台说明](platform-notes.md)：未完成上传和下载期间终止进程；ARM64 | 发现和结束阶段；x64e |
| R03 | `partial` | [平台说明](platform-notes.md)：绑定发布后终止，Control 契约中的 prepared 与 committed 崩溃 | 每个持久阶段都终止进程 |
| R04 | `passed` | [平台说明](platform-notes.md)：真实守护进程运行中暂停在切换和恢复后仍有效；ARM64 与 x64e，35.0.2 | 无 |
| R05 | `partial` | [引擎](engine.md)：串行调度契约；真实运行为一个账号两个目录 | 两个账号；持续变化 |
| R06 | `not run` | [平台说明](platform-notes.md)：跨会话检查尚未运行 | 全部 |
| R07 | `partial` | [账号](accounts.md)：重新认证与撤销的真实运行，35.0.2 | 密钥库锁定；服务器撤销 token |
| R08 | `partial` | [IPC](ipc.md)：Go/C++ 共享帧夹具 | 在真实管道上逐项记录畸形输入 |
| R09 | `partial` | [IPC](ipc.md)：真实管道上拒绝意外的服务端路径 | 跨用户、跨会话、远程和冒充 |
| R10 | `partial` | [IPC](ipc.md)：持久化与幂等契约；ARM64 与 x64e | 真实管道上注入丢失响应 |
| R11 | `partial` | [IPC](ipc.md)：冲突是契约的一部分 | 在真实管道上运行 |
| R12 | `partial` | [IPC](ipc.md)：原子 bootstrap 与订阅 | 并发更新的竞态测试 |
| R13 | `partial` | [IPC](ipc.md)：重连重新 bootstrap；有界队列 | 慢客户端；事件窗口过期 |
| R14 | `not run` | 没有运行记录 | 全部 |
| R15 | `not run` | [后端进度](backend-progress.md)：未覆盖休眠/唤醒 | 全部 |
| R16 | `not run` | [后端进度](backend-progress.md)：没有安装器或更新通道 | 全部 |
| R17 | `not run` | 没有运行记录 | 全部 |
| R18 | `not run` | 没有运行记录 | 全部 |
| R19 | `partial` | [打包](packaging.md)：ARM64 当前用户安装、重装与卸载；开机启动项、快捷方式、应用列表条目与安装目录均被删除；官方客户端注册未变 | x64 硬件；已登录账号下保留同步文件 |
| R20 | `partial` | [平台说明](platform-notes.md)：ARM64 真实运行；[打包](packaging.md)：已检查打包后的依赖闭包，安装后的界面与 SyncD 以 ARM64 运行 | 完整守护进程同步；运行时的 TLS 后端 |
| R21 | `not run` | [后端进度](backend-progress.md)：没有 24 小时运行 | 全部 |
| R22 | `partial` | [IPC](ipc.md)：支持包只含脱敏字段 | 对日志和支持包做秘密扫描 |

## 性质（§20.5）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| P01 | `partial` | [平台说明](platform-notes.md)：S19 准入竞争，ARM64 与 x64e | 随机事件序列测试 |
| P02 | `partial` | [URL 使用审计](url-usage-audit.md)、[引擎](engine.md)：契约和真实运行中的 origin 门槛 | 随机测试 |
| P03 | `partial` | [引擎](engine.md)：绑定 epoch 的完成契约 | 随机测试 |
| P04 | `partial` | [引擎](engine.md)：绑定 generation 的探测和结果 | 随机测试 |
| P05 | `partial` | [平台说明](platform-notes.md)：契约和真实运行中暂停仍有效 | 随机测试 |
| P06 | `partial` | [引擎](engine.md)：旧 epoch 提案被拒绝 | 随机测试 |
| P07 | `not run` | 没有运行记录 | 全部 |
| P08 | `partial` | [引擎](engine.md)：评分之前先应用信任过滤 | 随机测试 |
| P09 | `partial` | [IPC](ipc.md)：持久回执；真实运行中重启后仍保留 | 随机测试 |

没有任何性质记录了随机状态机运行、保存的 seed 或最小失败序列，因此都不是 `passed`。

## 最终检查表（§23）

| 项目 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| 23.1-1 最新上游、依赖、补丁 | `partial` | [后端进度](backend-progress.md)：Desktop v35.0.3，十六个补丁 | 全新的十六补丁构建和完整上游套件 |
| 23.1-2 原生 ARM64 引擎及依赖 | `partial` | [平台说明](platform-notes.md)：ARM64 真实运行、PE 扫描 | 完整守护进程同步；依赖闭包 |
| 23.1-3 界面退出后继续同步 | `partial` | [平台说明](platform-notes.md)：断开后守护进程继续运行 | 强制结束真实窗口 |
| 23.1-4 身份、凭据引用、journal 稳定 | `partial` | [平台说明](platform-notes.md)：切换和更新凭据后 journal 文件保持不变 | 所有切换中的凭据引用 |
| 23.1-5 调度与阻塞语义 | `partial` | [引擎](engine.md)：调度契约 | 大规模公平性；两个账号 |
| 23.1-6 origin、命名空间、TLS、重定向 | `partial` | [平台说明](platform-notes.md)：N02 已通过 | N04、N05、N06 |
| 23.1-7 评分、防抖、断路器、原因 | `partial` | [引擎](engine.md)：确定性契约 | 界面展示；真实运行 |
| 23.1-8 故障注入下的切换状态机 | `partial` | [平台说明](platform-notes.md)：若干真实故障运行 | 每个阶段；不确定的 MOVE/DELETE |
| 23.1-9 冲突、删除保护、根身份 | `partial` | [平台说明](platform-notes.md)：冲突和删除否决已真实运行 | S12–S15、S18、S21 |
| 23.1-10 错误分类、有界重试 | `partial` | [引擎](engine.md)：分类与退避 | 真实 429、404、维护页 |
| 23.2-1 schema、方法、事件、错误、版本 | `partial` | [IPC](ipc.md)：协议 1.8 尚未发布 | 冻结；R14 |
| 23.2-2 真实 Go/C++ Named Pipe | `passed` | [平台说明](platform-notes.md)：真实管道测试；ARM64 与 x64e | 无 |
| 23.2-3 重连后幂等命令 | `partial` | [IPC](ipc.md)：契约 | 真实管道上的 R10、R11 |
| 23.2-4 快照与事件竞态、背压 | `partial` | [IPC](ipc.md)：设计和契约 | R12、R13 |
| 23.2-5 前端不持有秘密或数据库 | `partial` | [界面](gui.md)：界面只经 IPC | 对已连接界面的扫描 |
| 23.2-6 认可、删除确认、重新认证可完成 | `partial` | [IPC](ipc.md)：后端真实运行；[界面](gui.md)：仅 Mock | 删除决定的界面控件 |
| 23.3-1 名称和图标 | `partial` | [打包](packaging.md)：安装器、快捷方式与应用列表条目名为 Nextcloud-MyGo；可执行文件图标取自上游图标 | 商标核对；托盘与窗口图标 |
| 23.3-2 包架构和依赖 | `not run` | [后端进度](backend-progress.md)：没有打包 | 全部 |
| 23.3-3 安装、升级、降级、卸载、共存 | `partial` | [打包](packaging.md)：ARM64 安装、同版本升级、卸载与共存；降级拒绝由单元测试覆盖 | x64；不同版本间升级；`daemon.prepareUpdate` |
| 23.3-4 矩阵、内容核验、24 小时报告 | `not run` | 本文档 | 全部 |
| 23.3-5 仅在可信链就绪后自动更新 | `not possible on this hardware` | [界面](gui.md)：未签名构建保持更新关闭 | 可信签名链 |
| 23.3-6 §17.1 桌面功能 | `partial` | [平台说明](platform-notes.md)：托盘、通知、登录时启动未手动验证 | `daemon.prepareUpdate`；手动运行 |
| 23.3-7 许可证、源码、限制、诊断说明 | `partial` | [平台说明](platform-notes.md)：已知限制 | 用户指南；发布源码材料 |
| 23.3-8 未验证能力不列为已支持 | `partial` | [后端进度](backend-progress.md)：`fullSync` 和 `endpointAutoSwitch` 保持 `false` | 发布清单 |

## 里程碑退出条件（§18）

| 里程碑 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| M0 可行性与审计 | `partial` | [平台说明](platform-notes.md)：原生 ARM64 真实基本同步，35.0.2；[后端进度](backend-progress.md)称 M0 未完成 | 其余审计；CFAPI 与 File Provider 审计；原生 x64 |
| M1 契约、账号、单端点 | `partial` | [IPC](ipc.md)、[账号](accounts.md)：两目录同步、登录、IPC 无秘密 | 整体退出条件尚未确认 |
| M2 可靠性和文件安全 | `partial` | [引擎](engine.md)：调度、根租约、删除否决 | §20.2 核心用例未全部通过 |
| M3 端点登记与只读策略 | `partial` | [IPC](ipc.md)：认可、命名空间核验、策略 | N04、N06、N07 |
| M4 安全自动切换 | `partial` | [平台说明](platform-notes.md)：手动切换和故障转移已真实运行；自动切换能力为 `false` | VPN；不确定的写入 |
| M5 前端接入与首版发布 | `not run` | [后端进度](backend-progress.md)：没有打包，没有 24 小时运行 | 全部 |
| M6 现代网盘框架 | `partial` | [按需文件](on-demand.md)、[平台说明](platform-notes.md)：Windows 按需注册、按需下载、释放空间、始终保留、切换边界、崩溃恢复和注销，在原生 ARM64 上通过引擎运行时针对服务器 35.0.2 运行 | 配对守护进程与界面、另一个 origin、占位文件下的删除保护与离线、卸载时注销；`virtualFiles` 为 false；macOS 部分按用户 2026-11-11 的决定不在 0.1.0 范围内 |

## 硬件和环境限制

- 只有一台机器：Windows 11 ARM64。x64 结果来自其上的仿真，不是原生 x64。
- 没有 macOS 硬件：M6 的 macOS File Provider 无法在此证明，且不属于 0.1.0。
- 没有可信签名链：签名更新、签名安装包和公证无法证明。
- 只有一个服务器版本（35.0.2）：没有上一个主版本服务器、第二个账号，也没有交互式浏览器、SSO 或
  MFA 登录。
- 故障夹具经自有本地中继访问服务器；没有记录真实的 VPN 路径。
- 因缺少夹具或安装包而未运行、与硬件无关的项目：休眠/唤醒、24 小时运行、4 GiB 以上文件和
  clang-tidy。
