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
| S04 | `passed` | [平台说明](platform-notes.md)：`file-safety` 在真实服务器上双向重命名和移动目录，保留服务器文件 ID，不重新传输内容，不触发删除审核；隔离服务器上 ARM64 1 分 6 秒、x64e 59 秒，共享服务器上两种架构均通过；此后每次 ARM64 服务器层均通过；B14：在已安装的界面中完成本地重命名；`data-safety -Case rename-cut`：MOVE 被中继拒绝的本地重命名保留了用户的名称，之后的周期完成了该 MOVE（服务器文件 ID 和内容相同，本地文件相同，回收站中没有内容），补丁 0019；ARM64，35.0.2，在服务器层中通过（13.5 秒），并使用补丁的较早修订单独运行三次（12.8–16.1 秒）；没有该补丁时失败（旧名称回来了） | x64e 没有运行 `rename-cut` |
| S05 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`data-safety -Case interrupted-change`：对已有文件的一个本地修改和一个远程修改都在传输中途被切断，两个地址都被拒绝；服务器保留了先前版本（修改作为分块上传发送，补丁 0018：Nextcloud 35.0.2 把替换文件的 PUT 直接覆盖写入原文件），本地保留完整版本，没有冲突副本，网络恢复后两者都在同一地址上收敛；三次运行，ARM64，35.0.2；此前经中继切断的新文件上传以匹配的哈希恢复（`network-active`） | 不支持分块 v2 的服务器仍对每个修改收到一个 PUT；x64e 没有运行该用例 |
| S06 | `passed` | [平台说明](platform-notes.md)：`network-chunked` 的 112 MiB 上传在第二块中断后续传，不重发第一块（ARM64 2 分 55 秒，x64e 2 分 58 秒）；`network-large` 的 4,296 MiB 分块会话在 4 GiB 之后中断，续传后流式哈希一致（ARM64 28 分 35 秒）；`network-active` 的下载被中断后恢复；上游 `ChunkingNgTest` 22/22 | x64e 上的 `network-large`；4 GiB 以上的下载 |
| S07 | `passed` | [平台说明](platform-notes.md)：服务器已提交但响应被扣留的 PUT 及分块会话的最终 MOVE 经重新发现恢复，哈希、服务器文件 ID、ETag 和原 journal 一致；ARM64 与 x64e | 响应之外的阶段性故障注入 |
| S08 | `passed` | [平台说明](platform-notes.md)：`network-uncertain` 中已有条目的 MOVE 和 DELETE 已提交但响应丢失，另一客户端重新创建了路径，恢复时不再发送第二次 MOVE 或 DELETE，重新创建的对象保持不变（文件 ID 与 ETag 相同）；ARM64 12 分 31 秒，x64e 10 分 27 秒；此后每次 ARM64 服务器层均通过 | 无 |
| S09 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`watcher`：上传被扣住时于发现之后写入的变化自行触发下一周期，3 次均在 20 秒内（轮询为 48–72 秒）；ARM64，35.0.2 | 无 |
| S10 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`watcher`：真实的监视缓冲区溢出（控制线程忙碌时写入 300 个长名称、一次重命名和一次修改，由同样大小的探测监视器证实）触发了找到全部变化的核对，2 次均在 30 秒内；ARM64，35.0.2 | 无 |
| S11 | `passed` | [平台说明](platform-notes.md)：`file-safety` 在传播前把含十二个后代的单个目录作为 13 个远程删除搁置（比例阈值），只在批准后执行，ARM64 与 x64e；B14：在界面中审核 30 个文件中的 25 个；阈值计算见 `SyncDDeletionContracts`；`file-safety-bulk`（[平台说明](platform-notes.md#010-验收审计)）：另一个客户端删除 510 个文件中的 100 个，只由绝对阈值搁置为 100 个本地删除，批准前没有删除任何本地文件，批准后执行；2026-11-13 在 ARM64 服务器层五次通过（61.0–69.8 秒），针对 35.0.2 和 34.0.5 | x64e 未运行 `file-safety-bulk` |
| S12 | `passed` | [平台说明](platform-notes.md)：`file-safety` 在本地删除含十二个后代的单个目录：提议为 13 个远程删除而不是 1 个，决定之前没有删除任何内容；ARM64 与 x64e，隔离服务器与共享服务器 | 无 |
| S13 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`root-denied`：用户无权列出的映射根目录报告为 `UNSUPPORTED_FOLDER_MODE`，服务器上没有删除，没有删除提案；该阻断持续到重启；ARM64，35.0.2 | 无 |
| S14 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`maintenance`：维护模式（503）报告为 `NETWORK_UNAVAILABLE`，本地删除被扣住，之后收敛；`file-safety` 的 404 根目录；`partial-listing`：目录消失的 `files_external` 存储（在列表中被省略或列为空）报告为 `DISCOVERY_INCOMPLETE`，本地副本保留，没有提出删除，三次运行；ARM64，35.0.2 | 在服务器上确实被清空或卸载的存储会一直被扣住，直到其本地副本被移除 |
| S15 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`root-replaced`：停止期间被空文件夹替换的根目录报告为 `FOLDER_ROOT_CHANGED`，两个方向都没有传播，两个文件夹均未被改动；`DefineDosDevice` 驱动器别名作为根目录被拒绝；ARM64，35.0.2 | 另一个卷复用驱动器号是一项[限制](#010-已知限制) |
| S16 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`journal`：重启后文件头损坏的 journal 报告为 `JOURNAL_ERROR`，逐字节保留，本地和远程变化都未传播；上游提交失败回归测试（补丁 0011、0013）；ARM64，35.0.2 | 无 |
| S17 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`names`：大小写冲突、三个 Windows 无法容纳的名称、NFD 与非 BMP Unicode、双向超过 300 个字符的路径和一个锁定文件：其他文件都已同步，服务器文件未被改动，锁定的副本保留到释放，没有删除提案；ARM64，35.0.2。发现并修复：传播前扫描拒绝超过 `MAX_PATH` 的路径 | 无 |
| S18 | `partial` | [平台说明](platform-notes.md#数据安全与恢复用例)：`excluded`：含联接点和脱机文件的本地树在首次传播前被阻断为 `UNSUPPORTED_FOLDER_MODE`，没有发送任何内容；ARM64，35.0.2 | E2EE 文件夹和真正的 Cloud Files 占位文件：一项[限制](#010-已知限制) |
| S19 | `partial` | [平台说明](platform-notes.md)：分层根准入，3 次重复/144 次竞争；ARM64 18.27 秒，x64e 26.27 秒 | 非 NTFS 拒绝、SUBST、发布中崩溃；跨会话是一项[限制](#010-已知限制) |
| S20 | `passed` | [平台说明](platform-notes.md)：`file-safety` 在真实服务器上再删除一个本地文件之后批准删除：过期的批准不删除任何内容，变化后的集合被重新提议，只在它自己的批准之后执行；ARM64 与 x64e；契约中旧 epoch 提案被拒绝 | 无 |
| S21 | `partial` | [平台说明](platform-notes.md#数据安全与恢复用例)：`excluded`：已同步映射中出现的联接点在传播前阻断该映射，两个方向都没有发送、下载或删除；ARM64，35.0.2 | E2EE 子文件夹和真正的占位文件：一项[限制](#010-已知限制) |

## 网络、信任和切换（§20.3）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| N01 | `passed` | [平台说明](platform-notes.md)：同一服务器的两个地址完成登记；ARM64 与 x64e，35.0.2；B14 第 6 步：在已安装的界面中添加备用地址，发送任何凭据之前先显示证书和服务器，显示两者的应答时间，每次切换都带原因；评分见确定性契约 | 无 |
| N02 | `passed` | [平台说明](platform-notes.md)：两个真实隔离实例，UID 相同、命名空间不同，被拒绝并隔离；ARM64 与 x64e，35.0.2 | 无 |
| N03 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`tls-trust`，经测试代理可达的未登记地址只收到公开检查，两个已认可地址都不可用时也没有凭据、没有后续请求；没有任何代码读取 SSID，同名网络无可信任之处；ARM64 与 x64e，35.0.2 | none |
| N04 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`tls-trust`，已过期、主机名错误和未知 CA 的证书在登记时被拒绝；出现在已认可的备用地址上时被隔离（`TLS_ERROR`），主地址不可用时也从不使用，只有明确复核才解除；ARM64 与 x64e，35.0.2 | none |
| N05 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`redirect`，正在使用的地址以 302 重定向到未知域名和明文 HTTP：都没有跟随，两个目标都没有收到请求，该地址被隔离为 `ENDPOINT_UNTRUSTED`；登录和元数据客户端从不跟随重定向（[URL 使用审计](url-usage-audit.md)）；ARM64 与 x64e，35.0.2 | none |
| N06 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`subpath`，位于 `/nc/` 下的入口经核验后使用：上传、重命名（MOVE Destination）和下载（PROPFIND href）都落到同一服务器路径；ARM64 与 x64e，35.0.2 | none |
| N07 | `limitation` | [引擎](engine.md)：不宣称严格 VPN 出口 | 0.1.0 不宣称 |
| N08 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`auto-switch`，8 秒内 20 次网卡变化没有移动任何东西；`SyncDSwitchingContracts`：旧代次的结果被拒绝，5 分钟内每 3 秒一次抖动没有引起切换；ARM64 与 x64e，35.0.2 | none |
| N09 | `passed` | `SyncDSwitchingContracts`：分差 12.15 切换、11.7 不切换，驻留差 1 毫秒、稳定 14.999 秒、证据已过 15.001 秒，切换后的驻留（覆盖 60 秒冷却）；`auto-switch`：按守护进程自己的探测，以原因 `faster` 选用家庭地址（优先级 90 对 50）；ARM64 与 x64e | none |
| N10 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`tls-circuit`，12 次被拒连接构成 10.5 秒内 3 次独立失败后熔断打开；契约：同一请求的多次回调以及 5 秒内的一串失败只计一次；ARM64 与 x64e，35.0.2 | none |
| N11 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`auto-switch`，更优的家庭地址为一个被挂起的 12 MiB 上传等待 32 秒，超过 30 秒排空窗口，没有取消它，随后以一个新 epoch 切换，journal 不变；ARM64 与 x64e，35.0.2 | none |
| N12 | `passed` | [平台说明](platform-notes.md)：中断的传输在排空后以新 epoch 故障转移到另一 origin；ARM64 与 x64e，35.0.2 | 无 |
| N13 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`late-epoch`，旧地址的应答迟到 6 秒：切换只在旧 epoch 的每个 DAV 请求都得到应答后提交；迟到的探测应答没有改变新地址、绑定和进度；ARM64 与 x64e，35.0.2 | none |
| N14 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`auto-fallback`，更优地址通过核验、已提交，首次业务请求失败：重新核验原地址并以又一个 epoch 使用它，journal 与凭据不变；ARM64 与 x64e，35.0.2 | none |
| N15 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`tls-circuit`，两个地址都被拒绝 45 秒，其间三次网络通知：没有切换，分别 5 次和 3 次探测；`network`（全部失效时等待、无来回切换）；契约：失效 30 分钟从不提议，探测受熔断约束；ARM64 与 x64e，35.0.2 | none |
| N16 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`tls-auth`，只有备用地址返回 401：该地址被阻止为 `AUTH_REQUIRED`，账号仍已登录，凭据保留并在主地址上使用，没有切换到它；ARM64 与 x64e，35.0.2 | none |
| N17 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`revoke-switch`，切换等待一个被挂起的上传时服务器吊销了应用密码：切换以 `AUTH_REQUIRED` 失败，账号显示需要登录，绑定不变，15 秒内没有尝试其他地址；ARM64 与 x64e，35.0.2 | none |
| N18 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`tls-auth`，正在使用的地址返回 429 及 Retry-After 20：期间两个地址都没有收到请求，没有切换，上传随后在同一地址上继续；ARM64 与 x64e，35.0.2 | none |
| N19 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`new-folder`，无法检查新目录根的备用地址：驻留已计满也没有自动切换，手动切换被拒绝，直到它核验通过；ARM64 与 x64e，35.0.2 | none |
| N20 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`root-change`，映射的远程根被同一路径的新根替换：写入停止（`ENDPOINT_IDENTITY_MISMATCH`），本地文件保留，网络变化没有让写入继续；ARM64 与 x64e，35.0.2 | none |
| N21 | `passed` | [平台说明](platform-notes.md)：主/备/主切换保持原生 journal 文件，无变化周期请求检查；ARM64 与 x64e，35.0.2 | 无 |
| N22 | `passed` | [平台说明](platform-notes.md#自动切换验收)：`tls-circuit`，熔断打开时 3 秒内 30 次网络通知：18 秒内一次半开探测；契约：同时只有一个探测在进行，提前的半开探测相隔至少 10 秒；ARM64 与 x64e，35.0.2 | none |
| N23 | `limitation` | [引擎](engine.md)：不宣称严格 VPN 出口 | 0.1.0 不宣称 |

## 生命周期、IPC 与升级（§20.4）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| R01 | `passed` | [平台说明](platform-notes.md)：客户端断开后守护进程继续运行，真实 Go↔C++ 管道，ARM64 与 x64e；B14：强制结束已安装的窗口，SyncD 继续运行，新窗口正确显示账号；ARM64，35.0.2 | x64e 下的强制结束 |
| R02 | `partial` | [平台说明](platform-notes.md)：在上传和下载未完成时结束进程；[平台说明](platform-notes.md#数据安全与恢复用例)：`discovery-kill`，在周期处于远程发现时结束进程，重启后在同一 journal 上把两个等待的变化各完成一次；ARM64 | 结束阶段；x64e |
| R03 | `passed` | [平台说明](platform-notes.md)：`switch-kill` 在每个持久切换阶段（`prepared`、`quiescing`、`verifying`、`committed`、`reconciling`）结束进程，并在同一 profile 上重启：前三个回到原绑定，后两个回到已提交的绑定，新 epoch，凭据引用、目录行和 journal 不变，已协调的内容不再发送；ARM64 与 x64e，隔离服务器与共享服务器；此后每次服务器层均通过 | 无 |
| R04 | `passed` | [平台说明](platform-notes.md)：真实守护进程运行中暂停在切换和恢复后仍有效；ARM64 与 x64e，35.0.2 | 无 |
| R05 | `partial` | [平台说明](platform-notes.md)：`multi-account`，一个守护进程上两个账号各两个目录，四个目录双向传输内容，一个账号切换时另一个继续同步且未被暂停；服务器层 ARM64；串行调度契约（[引擎](engine.md)） | 在持续变化下断言同一时间只有一个引擎周期的真实运行 |
| R06 | `limitation` | [平台说明](platform-notes.md)：跨会话检查尚未运行 | 第二个 Windows 登录会话 |
| R07 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`revocation`：在服务器上撤销的应用密码（`DELETE ocs/v2.php/core/apppassword`）使账号进入 `revoked`、其文件夹为 `AUTH_REQUIRED`，之后没有发送任何内容，任何配置目录文件中都没有秘密；[账号](accounts.md)：重新认证；ARM64，35.0.2 | 密钥库锁定是一项[限制](#010-已知限制) |
| R08 | `passed` | [平台说明](platform-notes.md#010-验收审计)：真实管道上的 `TestRealPipeFramingR08R12`：在前缀和正文内部拆开的帧、一次写入中的两帧，以及六种畸形帧（零长度、超过 1 MiB、非法 UTF-8、重复键、截断的 JSON、超过 64 层的嵌套）各自只结束该连接，下一个客户端照常得到服务且状态保持，永远不完整的半帧在 15.0 秒后被断开；ARM64 16.2 秒，x64e 17.1 秒 | 无 |
| R09 | `partial` | [平台说明](platform-notes.md#010-验收审计)：`TestRealPipeImpostorR09`：先占用管道名的进程被客户端拒绝（可执行文件不符），并使守护进程无法以该名称启动；经由 `\\127.0.0.1\pipe\` 的客户端作为远程客户端被拒绝（拒绝访问）；意外的服务端路径被拒绝（`TestRealPipeINV12INV13`）；ARM64 与 x64e | 跨用户和跨会话：一项[限制](#010-已知限制) |
| R10 | `passed` | [平台说明](platform-notes.md#010-验收审计)：`TestRealPipeReplayR10R11R14`：响应从未被读取的暂停只提交了一次；同一个键在重连之后、之后的恢复之后以及守护进程重启之后再次发送，都返回同一个回执，且从未再次暂停；ARM64 1.2 秒，x64e 2.3 秒 | 无 |
| R11 | `passed` | [平台说明](platform-notes.md#010-验收审计)：同一个键配不同输入返回 `OPERATION_KEY_CONFLICT`，状态和修订号不变，真实管道；ARM64 与 x64e | 无 |
| R12 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`TestRealPipeConcurrentSourceR12`：在守护进程自身的崩溃退避激活期间 bootstrap，激活作为 `seq` 更晚的事件到达，seq 从不后退，同一批两次 bootstrap 之间的暂停出现在事件中；ARM64 2.5 秒；`TestRealPipeFramingR08R12` | 无 |
| R13 | `passed` | [平台说明](platform-notes.md#010-验收审计)：`TestRealPipeSlowClientR13`：从不读取的客户端因有界输出队列被断开（ARM64 在 2.3 秒后，x64e 在 8.6 秒后），下一个客户端在 5 秒内得到服务，`events.resume` 返回 `RESYNC_REQUIRED`；重启后重新 bootstrap（`TestRealPipeINV12INV13`） | 无 |
| R14 | `passed` | [平台说明](platform-notes.md#010-验收审计)：`TestRealPipeReplayR10R11R14`：主版本为 2 的 `session.hello` 返回 `PROTOCOL_MISMATCH`，协商之前的修改命令和 bootstrap 同样被拒绝且不执行任何操作；ARM64 与 x64e | 无 |
| R15 | `passed (simulated)` | [平台说明](platform-notes.md)：`sleep-resume`（[开发](development.md)），在一次被扣住的上传期间把整个 SyncD 进程挂起 35 秒，唤醒后只有备用地址可达：失效连接被判为失败，没有任何内容被当成已删除，在目录空闲时以原因 `unreachable` 切换，上传只完成一次，journal 与目录身份不变；ARM64，35.0.2。[引擎](engine.md)：唤醒通知使用可注入时钟测试；UTC 回拨不缩短限流截止时间（`syncd_constraint_tests`）。之后在空闲机器上的一次运行在“切换没有等到目录空闲”处失败，正在另一个会话中调查 | 真实休眠：电源状态切换及其通知到达运行中的服务进程、单调时钟是否计入休眠时间、网卡与地址变化、未收到重置就失效的连接、数小时的休眠、休眠期间卷的变化、现代待机（Modern Standby）；x64e |
| R16 | `partial` | [打包](packaging.md#签名更新)：被拒绝的下载（错误的密钥、改过的归档或清单、其他架构、版本不更新）以及在归档传输到一半时断开的连接（`TestUpdateInterruptedDownload`）都不留暂存内容，也不启动安装程序（单元测试）；应用内路径经 setup 与 `daemon.prepareUpdate` 在运行中的 0.1.0 上安装了签名的 0.1.1，配置状态保留，ARM64 | 真实安装中应用内路径的 setup 失败；由已安装的界面发起 |
| R17 | `partial` | [平台说明](platform-notes.md#010-验收审计)：`TestRealNewerControlSchemaR17`：给已编译的守护进程一个新一个 schema 的 Control 数据库，它只提供 `databaseUnavailable`，拒绝修改，并让文件逐字节保持不变（ARM64 2.3 秒，x64e 2.6 秒）；迁移备份拒绝更新的 schema（`SyncDStartupContracts`）；安装程序拒绝降级（单元测试） | 由更新的 Desktop 版本升级过的同步 journal 被旧版本打开 |
| R18 | `limitation` | [平台说明](platform-notes.md#数据安全与恢复用例)：SyncD 不备份 journal，也不提供恢复；迁移备份只含 Control 数据库且拒绝更新的架构；安装程序拒绝降级 | 手动恢复的 journal 不会被发现：一项[限制](#010-已知限制) |
| R19 | `partial` | [打包](packaging.md)：ARM64 当前用户安装、重装与卸载；开机启动项、快捷方式、应用列表条目与安装目录均被删除；官方客户端注册未变；B14：移除账号并卸载后同步文件保留，ARM64 | 账号仍登录时卸载；x64 包是一项[限制](#010-已知限制) |
| R20 | `passed` | [平台说明](platform-notes.md)：ARM64 真实运行与 B14：已安装的 ARM64 包（30 个 PE 文件均为原生 ARM64）通过公共受信 TLS 完成登录、同步、切换、重装和卸载，35.0.2；[打包](packaging.md)：依赖闭包；休眠与唤醒（§15.2 第 7 项）仅以模拟通过（R15） | 真实的休眠与唤醒（§15.2 第 7 项） |
| R21 | `partial` | [平台说明](platform-notes.md#加速浸泡运行r21)：加速 1 小时浸泡运行，不是挂钟时间 24 小时（用户的决定），ARM64，35.0.2：89 个周期，约 3,600 次文件变更，12 次分块上传，54 次地址故障和 16 轮抖动，69 次切换均记录原因且没有一次发生在运行中的周期内，两次续期、一次重启、界面管道连接；斜率在界限内（私有字节 +4.8 MB/h，句柄 −17/h，线程 −1/h，Control 数据库 +40 KB/h）；每个周期哈希一致，没有重复上传或意外删除。已修复：无界 outbox。失败：三次本地重命名在 MOVE 失败后被撤销（上游）。使用补丁 0019 重新运行两次：两次都没有重命名被撤销（332 次和 352 次重命名）；第一次仅 TCP 波动带未通过（65，界限 60），第二次所有检查、斜率和波动带均通过 | 午夜翻转、真实令牌过期、真实睡眠、低于测量界限的泄漏、x64 |
| R22 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：`revocation`：任何配置目录文件中都没有密码、应用密码或夹具密码；支持包和守护进程日志中没有用户、服务器、本地路径、文件名或内容；B14 支持包检查；ARM64，35.0.2 | 无 |

## 性质（§20.5）

| 用例 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| P01 | `passed` | `SyncDSwitchingContracts`，500 个固定 seed、每个 300 个随机准入事件的序列：从不出现第二个引擎周期，因此从不出现第二个 journal 写入者；S19 准入竞争；ARM64 9.1 秒，x64e 10.9 秒（整个测试） | none |
| P02 | `passed` | `SyncDSwitchingContracts`，由恶意部件组成的 20,000 个 URL：请求闸门放行的每一个都是已认可主机和端口上安装路径内的 HTTPS；真实运行中从未向未认可的来源发送凭据（N03、N05）；ARM64 与 x64e | none |
| P03 | `passed` | `SyncDSwitchingContracts`，500 个固定 seed 的切换序列：周期在其开始时的 epoch 上结束，epoch 只在每个冻结目录都证明已静止后加一；ARM64 与 x64e | none |
| P04 | `passed` | [平台说明](platform-notes.md#010-验收审计)：`SyncDPropertyContracts`，500 个固定 seed、每个 400 个随机策略事件：来自旧网络 generation、旧配置、硬失败或复核之前的票据，以及已使用的票据，都被拒绝且不改变任何健康字段；ARM64 0.18 秒，x64e 0.40 秒 | 无 |
| P05 | `passed` | `SyncDSwitchingContracts`，同一准入运行：无论到达何种恢复、重试、限流或解冻事件，用户暂停的目录都不会被准入；ARM64 与 x64e | none |
| P06 | `passed` | `SyncDSwitchingContracts`，同一切换运行：指向其他 epoch 的请求或证明、不可信目标以及较旧 epoch 的协调都被拒绝；ARM64 与 x64e | none |
| P07 | `not run` | 没有运行记录 | 全部 |
| P08 | `passed` | [平台说明](platform-notes.md#010-验收审计)：同一次运行：任何自动或固定提议都从未指向被禁用、未认可、未核验、TLS 无效、在当前网络不允许、被隔离或因认证被阻止的端点，固定目标也从不回退到另一个端点；故意写错的模型在 seed 1 失败并缩减为两步 | 无 |
| P09 | `passed` | `SyncDSwitchingContracts`，在真实 Control 数据库上由修改、重放、冲突、过期修订号和重启组成的 20 个固定 seed 序列，每个 60 步（2026-11-13 之前为 40 个；CI x64 运行器超出其 60 秒测试上限）：每个已接受的回执在重启后原样返回；ARM64 与 x64e | none |

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
| 23.1-9 冲突、删除保护、根身份 | `partial` | [平台说明](platform-notes.md)：冲突、比例阈值下的删除审核、后代和过期批准均已真实运行（S03、S12、S20） | 100 个对象的 S11、S13、S15、S18、S21 |
| 23.1-10 错误分类、有界重试 | `partial` | [引擎](engine.md)：分类与退避；真实的 404 与配额已满 | 真实 429、维护页 |
| 23.2-1 schema、方法、事件、错误、版本 | `partial` | [IPC](ipc.md)：协议 1.10，跨主版本的版本协商在真实管道上被拒绝（R14） | 冻结协议 1.10 的发布 |
| 23.2-2 真实 Go/C++ Named Pipe | `passed` | [平台说明](platform-notes.md)：真实管道测试；ARM64 与 x64e | 无 |
| 23.2-3 重连后幂等命令 | `passed` | [平台说明](platform-notes.md#010-验收审计)：真实管道上的 R10、R11，回执在重连和重启后仍然有效 | 无 |
| 23.2-4 快照与事件竞态、背压 | `passed` | [平台说明](platform-notes.md#数据安全与恢复用例)：R12 与 R13 经过真实管道 | 无 |
| 23.2-5 前端不持有秘密或数据库 | `partial` | [界面](gui.md)：界面只经 IPC | 对已连接界面的扫描 |
| 23.2-6 认可、删除确认、重新认证可完成 | `partial` | [IPC](ipc.md)：后端真实运行；B14（[平台说明](platform-notes.md)）：登录、地址认可、删除审核和冲突在已安装的界面中完成 | 通过界面重新认证 |
| 23.3-1 名称和图标 | `partial` | [打包](packaging.md#与官方客户端共存)：显示名称为 **Nextcloud MyGo**（开始菜单快捷方式、应用列表条目、窗口标题、界面文字），标识保持为 `Nextcloud-MyGo`（目录、注册表键、可执行文件、管道名）；可执行文件图标取自上游图标；B14：显示了托盘图标 | 商标核对 |
| 23.3-2 包架构和依赖 | `partial` | [打包](packaging.md)：两个包均已检查（ARM64 28 个、x64 29 个 PE 文件，均为原生）；B14：ARM64 包经检查并运行 | 安装 x64 包：一项[限制](#010-已知限制) |
| 23.3-3 安装、升级、降级、卸载、共存 | `partial` | [打包](packaging.md)：ARM64 安装、同版本升级、卸载与共存；降级拒绝由单元测试覆盖；B14：在运行中且已登录的副本上同版本重装，账号和目录保留；安装包冒烟测试：在运行中的 SyncD 上经 `daemon.prepareUpdate` 从 0.1.0 升级到测试版本 0.1.1，核对了配置状态与启动报告（上一版本、schema、自检通过），ARM64 | 带已登录账号的版本间升级；x64 是一项[限制](#010-已知限制) |
| 23.3-4 矩阵、内容核验、24 小时报告 | `partial` | 本文档；24 小时报告是加速 1 小时浸泡运行的报告（`build/soak/summary.txt`，R21），不是挂钟时间 24 小时 | 加速一小时无法显示的内容（R21） |
| 23.3-5 仅在可信链就绪后自动更新 | `partial` | [打包](packaging.md#签名更新)：只有用保存在 GitHub secret 中的 `mygo keygen` 密钥构建的包才有更新；应用只安装用 `mygo.json` 中公钥对应密钥签名的归档，用户开启之前保持关闭；安装包冒烟测试：用一次性密钥，从环回 HTTP 获取签名的 0.1.1 测试更新并安装到 0.1.0 之上，ARM64 | 用项目密钥签名并从 GitHub 读取的 release；Authenticode 是一项[限制](#010-已知限制) |
| 23.3-6 §17.1 桌面功能 | `partial` | [平台说明](platform-notes.md)：B14：托盘图标与弹出面板、关闭到托盘、从开始菜单重新打开、“退出并停止同步”；安装程序在停止 SyncD 前调用 `daemon.prepareUpdate`（安装包冒烟测试） | 真实删除等待时 `daemon.prepareUpdate` 的拒绝；通知；登录时启动；真实休眠是一项[限制](#010-已知限制) |
| 23.3-7 许可证、源码、限制、诊断说明 | `partial` | [平台说明](platform-notes.md)：已知限制；本页的限制列表 | 用户指南；发布源码材料 |
| 23.3-8 未验证能力不列为已支持 | `passed` | [能力证据](#能力证据)：`virtualFiles` 保持 `false`；`endpointAutoSwitch` 和 `fullSync` 为 `true` 并附证据；[版本历史](version-history.md)中的 0.1.0 摘要不提供按需文件 | 无 |

## 里程碑退出条件（§18）

| 里程碑 | 状态 | 证据 | 缺失 |
|---|---|---|---|
| M0 可行性与审计 | `partial` | [平台说明](platform-notes.md)：原生 ARM64 真实基本同步，35.0.2；[后端进度](backend-progress.md)称 M0 未完成 | 其余审计；macOS File Provider 审计不在 0.1.0 范围内；原生 x64 是一项[限制](#010-已知限制) |
| M1 契约、账号、单端点 | `partial` | [IPC](ipc.md)、[账号](accounts.md)：两目录同步、登录、IPC 无秘密；真实管道上的 R08、R10、R11、R13、R14 | 整体退出条件尚未确认 |
| M2 可靠性和文件安全 | `partial` | [引擎](engine.md)：调度、根租约、删除否决；S04–S10、S12–S17、S20 已通过 | S11、S18、S21 |
| M3 端点登记与只读策略 | `partial` | [IPC](ipc.md)：认可、命名空间核验、策略；N01、N02 已通过 | N04、N06；N07 是一项[限制](#010-已知限制) |
| M4 安全自动切换 | `partial` | [平台说明](platform-notes.md)：手动切换和故障转移已真实运行，不确定的写入得到恢复（S07、S08），每个切换阶段被终止后都能恢复（R03）；自动切换能力为 `false` | 自动切换的证据（[能力证据](#能力证据)）；VPN 是一项[限制](#010-已知限制) |
| M5 前端接入与首版发布 | `partial` | [平台说明](platform-notes.md)：ARM64 上的 B14 联合验收，35.0.2，九步中六步通过；B14 发现的问题已在 SyncD 和安装程序中修复 | 通过界面重新运行第 6、8 步；带账号的版本间升级 |
| M6 现代网盘框架 | `partial` | [按需文件](on-demand.md)、[平台说明](platform-notes.md)：Windows 按需注册、按需下载、释放空间、始终保留、切换边界、崩溃恢复和注销，在原生 ARM64 与 x64e 上通过引擎运行时针对服务器 35.0.2 运行；Cloud Files 无法应答的主机跳过而不是通过 | 配对守护进程与界面、另一个 origin、占位文件下的删除保护与离线、卸载时注销；`virtualFiles` 为 false；macOS 部分按用户 2026-11-11 的决定不在 0.1.0 范围内 |

## 联合发布验收（B14）

原生 Windows 11 ARM64、已安装的包、由 UI Automation 驱动的真实窗口、Nextcloud 35.0.2，
2026-11-12；细节、发现的问题和能力证据见[平台说明](platform-notes.md)。

| 步骤 | 状态 | 核对方式 |
|---|---|---|
| 1 安装、设置、登录、目录、首次同步 | `passed` | 界面；设置的“开始”一步未经过 |
| 2 本地和远端改动、“活动” | `passed` | 界面、WebDAV 和本地文件，2026-11-13 通过已安装的安装包再次运行（[平台说明](platform-notes.md#经已安装界面的活动)）：本地改名和删除、服务器端改名和删除各自以其方向和本地时间列在“活动”中 |
| 3 批量删除审核 | `passed` | 界面和服务器 |
| 4 冲突 | `passed` | 界面和文件 |
| 5 目录暂停经过全部暂停和全部恢复 | `passed` | 界面和服务器 |
| 6 地址与切换 | `partial` | 界面；不提供固定/自动；超过排空窗口的切换现在以 `switchFailed` 结束（`switch-quota`，ARM64，35.0.2），未在界面中重新运行 |
| 7 关闭、重新打开、从托盘退出 | `passed` | 界面和进程 |
| 8 在运行中的副本上重装 | `partial` | 界面和进程；此后安装程序调用 `daemon.prepareUpdate`，自检不再因 `CANCELLED` 失败（`SyncDFolderErrorContracts`），安装包冒烟测试中 0.1.0 → 0.1.1 测试升级通过；未在界面中重新运行 |
| 9 移除账号、卸载 | `passed` | 界面、注册表、服务器令牌列表 |
| 休眠与唤醒 | `passed (simulated)` | 针对后端运行 `sleep-resume`，未经过界面或已安装的包（R15）；用户已决定不让这台机器真实休眠 |

## 能力证据

只有用户能决定开启某项能力。引擎构建的 `endpointAutoSwitch` 自 2026-11-12 起、`fullSync` 自 2026-11-13 起
为 `true`，各自在下列用例通过真实运行后按用户决定开启；`virtualFiles` 保持 `false`。每一项已有的证据和开启前缺少的内容：

| 能力 | 证据 | 开启前缺少 |
|---|---|---|
| `fullSync`（`true`） | S01–S08、S11、S12、S20 在真实服务器上通过（多数为 ARM64 与 x64e）；冲突、删除审核、暂停和恢复经过已安装的界面（B14）；真实周期中本地和服务器端的改名与删除出现在“活动”中（B14 第 2 步，2026-11-13）；两个账号各两个目录（R05 为 partial）；守护进程在界面退出后继续运行（R01、23.1-3）；加速浸泡运行中没有改名被撤销（R21） | E2EE 目录（S18、S21），一项[限制](#010-已知限制) |
| `endpointAutoSwitch`（`true`） | 手动和固定切换已真实运行，延后到安全点（N11、B14 第 6 步）；带排空和新 epoch 的故障转移（N12）；每个切换阶段被终止后都能恢复（R03）；不确定的写入得到恢复（S07、S08）；自动策略的真实运行：N03–N06、N08–N11、N13–N20 和 N22（[平台说明](platform-notes.md#自动切换验收)）；P01–P06、P08 与 P09；网络页面恰在能力为 true 时提供“自动”和“固定”（`TestAutomaticChoiceFollowsCapability`，针对 Mock 的原生截图） | 严格 VPN 出口（N07、N23，不宣称）；真实休眠（R15，仅模拟）；经已安装界面针对真实服务器选择自动模式 |
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
| 24 小时运行（R21、23.3-4） | 由加速 1 小时浸泡运行代替（[平台说明](platform-notes.md#加速浸泡运行r21)）：它无法显示午夜翻转、真实令牌过期或低于其测量界限的泄漏 |
| 端到端加密文件夹和其他提供方的 Cloud Files 占位文件（S18、S21） | 一次性服务器没有 `end_to_end_encryption` 应用，也没有从应用商店安装它；其他提供方的真正占位文件需要该提供方的同步根。联接点和脱机文件走同一条拒绝路径，并被真实拒绝 |
| 另一个卷复用驱动器号（S15） | 需要本机上的第二个固定 NTFS 卷；被替换的文件夹和驱动器别名被真实拒绝 |
| 密钥库锁定（R07） | 本机交互会话中的 Windows 凭据管理器不会被锁定；“未找到”以外的读取失败映射为 `CREDENTIALS_LOCKED` |
| 手动恢复的 journal（R18） | SyncD 不备份 journal，也没有恢复功能；要发现被复制覆盖当前文件的旧 journal，需要在 Control 中保存持久的 journal 标记，这是留给用户的设计决定（`PLAN.md` §16.3） |
