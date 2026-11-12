# SyncD IPC

## 契约

协议 1.9 在本地 Named Pipe 上传输 JSON-RPC 2.0：四字节小端长度后跟 UTF-8 JSON，
最多 1 MiB、64 层嵌套容器。重复对象键无效。`protocol/contract.json` 记录方法及能力门槛。
`protocol/fixtures.json` 由 Go 和 C++ 契约测试共享。线上状态投影使用小驼峰字段名。
夹具的 `valid` 表示 JSON 边界接受与否；RPC 消息外形与方法校验单独测试。

## 会话与状态

客户端先协商 `session.hello`，再请求 `state.bootstrap`。Bootstrap 原子返回 `bootId`、
十进制 `seq`、十进制配置 `revision` 及 `snapshot`，同时订阅连接。持久状态变更发布携带新
投影的 `event` 通知。重连始终重新 bootstrap。界面断线后将目录标记为离线，不显示过期的
绿色状态。关闭客户端不会关闭 SyncD。

## 命令

修改命令携带 UUID `operationKey`；配置修改还携带 `expectedRevision`。守护进程在回复前，
于同一 SQLite 事务中保存请求摘要、结果及状态事件。同一键配不同输入返回冲突。回复丢失
意味着操作结果不确定；重连后使用原键重试。客户端用 `operations.get` 查询已接受操作。
不支持的能力返回 `UNSUPPORTED_CAPABILITY`，不模拟成功。

## 实现范围

协议 1.9 与 Control schema 11 增加界面和安装程序需要的内容（示例见 `protocol/state.examples.json`，
schema 见 `protocol/message.schema.json`）：

- 多个账号。可以在同一或另一台服务器上添加第二个账号；同一服务器上的同一用户（相同 UID 与 DAV
  根身份，无论使用哪个地址）在其登录证明身份后被拒绝，返回 `ACCOUNT_EXISTS`，该登录取得的密码被撤销。
  `multiAccount` 能力报告这一点（[账号](accounts.md)）。
- 账号移除不再因账号忙碌而被拒绝：`accounts.remove` 停止该账号的登录、引擎与检查，移除其映射
  （文件与日志数据库保留），然后照旧撤销密码。
- `account.auth` 为 `signedIn`、`needsSignIn`（凭据无法在本机使用，或续期正在等待浏览器）或 `revoked`
  （服务器拒绝了应用密码）；`signedIn` 保留，等于 `auth == signedIn`。
- 周期运行时 `folder.phase` 为 `checking`、`uploading` 或 `downloading`，否则为空；`folder.filesDone`
  统计本周期已完成的文件。运行中的周期在两者变化时最多每秒发布一次快照事件。
- `endpoint.latencyMs`（平滑后的探测往返时间，探测成功前为 0）、`endpoint.kind`（`internet`、`home`、
  `vpn`，在 `endpoints.stage` 用可选的 `kind` 选择，从不影响信任）以及 `endpoint.blockedReason`：
  `blocked` 为 true 的唯一原因，按强弱排序：安全隔离、`AUTH_REQUIRED`、`VERIFICATION_PENDING` 或
  `NETWORK_UNAVAILABLE`。
- `network.why`（选择当前地址的原因）、`network.switchReason` 与 `network.switched`（最近一次已提交的切换）。
- 批量删除携带 `groups`（最多八个顶层条目及其文件数，零散文件合为一组）。上游保留的冲突副本是 `conflict`
  类决定，带 `file` 与 `copy`；`resolved` 关闭一条，两个副本都保留。
- `daemon.prepareUpdate(targetVersion)` 已分派。删除等待用户时拒绝并返回 `DECISION_PENDING`，切换、
  续期、地址检查、登录或移除进行中时返回 `QUIESCE_STALLED`；否则不再开始新周期，运行中的周期自然结束，
  写出一致的 Control 副本 `control.pre-update.<id>.sqlite`（只保留最新一份），回执以 `{"ready": true}`
  成功。保持状态持续十分钟或直到关闭。之后 `daemon.startupReport` 说明一次启动从何迁移
  （`previousVersion`、`previousSchema`、`schema`）以及自检是否通过（`selfCheck`：pending、passed 或
  failed，并带 `reasonCodes`）。
- `engine` 能力区分链接上游引擎的构建与基础构建。
- `folders.inspect` 接受尚不存在、但父目录存在的本地目录（`localExists` 为 false）；随后 `folders.add`
  将其创建为空目录。

Go 客户端发送 `minor: 9`，把 `ACCOUNT_EXISTS` 映射为 `ErrAccountExists`、`DECISION_PENDING` 映射为
`ErrBusy`，并提供从不启动 SyncD 的 `ConnectRemote`，以及 `Remote.Disconnect`：供诊断使用的稳定短语，
说明上一次连接为何结束。`nextcloud-mygo-syncd.exe --version` 输出一行包含版本、协议与 Control schema
的 JSON，不启动任何东西（[打包](packaging.md)）。

协议 1.8 增加目录文件模式（[按需文件](on-demand.md)）。`folders.add` 接受可选的 `mode`：`full`（默认）或
`onDemand`；其他取值或非字符串为 `INVALID_PARAMS`，缺少 CFAPI 插件时 `onDemand` 为 `UNSUPPORTED_CAPABILITY`。
模式在目录的整个生命周期内固定，由 Control schema 10 存储（`folders.mode`，既有行为 `full`），每个目录投影都携带它。
移除按需目录时先注销其 Cloud Files 注册；其按需下载仍在运行时拒绝并返回 `QUIESCE_STALLED`。`virtualFiles` 能力仍为
false：按需路径已在原生 ARM64 上通过 SyncD 的引擎运行时针对真实服务器运行，但尚未经过配对守护进程与界面。参见
`protocol/folders.schema.json`（`addParams`）和 `protocol/folders.examples.json`（`addOnDemand`）。Go 客户端解码
`Folder.Mode`，`AddFolder` 接受可选的模式；schema 9 的映射迁移后为 `full`（`testFolderModeMigration`）。界面中选择模式
尚待加入。

协议 1.7 增加显式启动可用性及稳定目录 `reasonCode` 投影。Control schema 8 正在集成
持久账号级服务器限流（此前启动验收使用 schema 7）。Control schema 9 仅新增
`held_revocations` 表（[账号](accounts.md)）；协议 1.7 的方法、字段、事件和错误均未随之改变。
`snapshot.daemon.stateAvailable=false` 表示账号/文件状态未知：快照省略全部账号/目录/
端点/网络/切换/决策/活动数组，而非报告空的成功结果。`startupState` 为 `normal`、
`backoff`、`diagnostic`、`stopped` 或 `databaseUnavailable`。经过认证的诊断 bootstrap
及 `diagnostics.snapshot` 仍可读取；依赖状态的调用拒绝并报告 `STATE_UNAVAILABLE`。
普通传输就绪与应用状态健康分开。任何诊断或退避状态都不能产生绿色同步状态。
客户端也拒绝 `stateAvailable=true` 与任何非 `normal` 启动状态的矛盾组合。
连接启动契约及经过认证的管道状态转换已在两个架构通过；更广的恢复矩阵仍在进行，尚不构成支持
声明。见 `protocol/startup.examples.json` 及
投影 schema。
RPC 就绪不等于界面健康连接：不可用 bootstrap 保持诊断及显式关闭可调用，将界面标为
断开，并将缓存应用集合清为未知。只有明确协商 pre-1.7 协议时才推断旧版本可用性。
数据库不可用时，关闭操作在单个原子写入文件中最多保留 32 个持久回执，不淘汰历史。
容量满时新键以 `QUIESCE_STALLED` 拒绝；旧键重试保持真实，绝不停止另一次启动。
全部回执以事务导入恢复后的 Control 存储，然后才移除文件。不从这一有限诊断回执存储
推断任何账号或文件状态。

同一尚未发布的 1.7 契约将 `PERMISSION_DENIED`、`REMOTE_LOCKED` 及 `SERVER_THROTTLED`
与传输/认证失败区分。重试期限仅在内部保留，不新增 IPC 字段。这些约束不能触发端点
回退或隔离；服务器限流作用于整个账号，而非单个 origin。消费方实现和验收正在进行。
见 `protocol/constraints.examples.json`。

修改命令跨重连/bootstrap 重试时保留原操作键、参数及预期修订号。取消操作等待返回上下文
错误，不会取消守护进程已接受的工作；之后可使用同一操作 ID 继续查询。取消导致的传输
EOF 不能掩盖取消或暗示回滚（INV-12）。

`diagnostics.export` 创建用户请求的本地 JSON 支持包，仅包含脱敏运行时快照（版本、ID、
epoch、状态、阻塞和计数）及创建时间。不复制日志、journal、数据库、文件内容或凭据库。
仅接受空的 `options` 选项集。操作返回生成的本地 `path`，供诊断使用，不供主界面显示。
包以原子文件写入配置的 `diagnostics` 目录；仅保留最近五个自有包文件。清理要求匹配已
记录的导出操作；绝不将无关 UUID 文件名视为自有包。操作键重试返回
相同回执/路径。中断的导出在重启时失败，不静默重放。此最小包不宣称完整日志或遥测实现；
不自动上传任何内容。

有界活动历史存储上游已确认条目完成及已提交切换的汇总，不保存文件名、路径或请求正文。
`activity.list` 接受可选的十进制排他 `cursor`、`accountId`/`folderId` 过滤及 `limit`
（默认 100、最多 500），按最新优先返回。条目携带用于分页的十进制游标。最多保留 1,000
条记录。快照活动包含最近 100 条。部分周期计数仅描述上游实际报告成功的条目，不说明
中断请求的结果，也绝不授权传播。

协议 1.6 增加持久 `endpoints.setPolicy` 及独立的待处理 `requestedEndpointId` 网络字段。
固定配置仅在全部根的新 epoch 重新核对完成后提交；调用者停止等待后仍可查询已接受操作。
即使自动策略也拒绝非字符串 `fixedEndpointId`；null 不等于省略/空目标。
`Remote.RequestPolicy` 返回接受结果，独立于 `WaitOperation`。Schema/示例位于
`protocol/policy.*`。Control schema 7 持久化策略、待处理策略、安全隔离及冻结的切换证据
上下文。既有隐式固定配置不会被静默启用自动选择。此连接路径正接受真实故障验收；完整
约定范围验证前，自动切换能力仍为 false。

`diagnostics.snapshot` 仅返回脱敏 ID、不可变 epoch 及汇总的请求/排空计数。不返回请求
URL、标头、密码、路径或文件内容，也不替代用户触发的支持包导出。活动上传/下载计数表示
实际未结束的内容回复，而非界面状态持续时间；结果不明不证明远程写入失败。

Control schema 6 增加保留账号的重新认证预留及原生凭据库替换恢复。
`accounts.reauthenticate` 接受账号 ID、预期 revision 和操作键；通过
`accounts.login.status` 轮询浏览器会话。清理警告不掩盖已安装的替换：状态包含原账号 ID
及稳定的警告原因。阶段与警告语义见[账号](accounts.md)及
`protocol/reauthentication.schema.json`。

Control schema 5 增加持久端点绑定和切换意图。启动时在同一个 Control 事务中阻塞被中断
的意图并将回执设为失败，同时保留已提交 epoch。目录挂接通过当前 origin 许可解析已提交
端点，并恢复独立的切换门槛。引擎构建公开 `endpoints.requestSwitch`，接受 `accountId`、
`endpointId`、`expectedRevision` 和 `operationKey`（见 `protocol/switch.schema.json`）。
它返回持久运行中回执，不等待用户暂停的根。轮询 `operations.get`；成功要求全部冻结根在
新 epoch 完成重新核对。Go 的 `Remote.RequestSwitch`/`WaitOperation` 将接受与完成分开。
结束等待不会取消操作。完整同步及自动切换保持能力禁用，等待完整恢复验收。
中断切换在启动时阻塞后，先重新取得原根租约，再由内部替代操作以新 epoch 重新验证已提交
origin。旧回执保持失败；不重放记录的 HTTP 写入。此前硬验证失败不会自动重试。
端点移除还会拒绝已提交绑定或待切换目标；删除其 origin 许可不能绕过已有运行时/恢复门槛。

端点登记使用 Control schema 4 中的持久暂存记录。`endpoints.stage` 执行不带凭据的 HTTPS
状态探测；不可变暂存 ID 将展示的 URL 绑定十分钟。`endpoints.approveOrigin` 先持久记录
明确的凭据发送许可，再启动只读 UID/DAV 根验证；`endpoints.verify` 重试已获认可的端点。
所有映射根（包括暂停目录）都须匹配。登记不改变活动端点。重启使未完成操作及暂存/已验证
结果失效；已认可 origin 保持许可，但须重新验证。`endpoints.remove` 拒绝移除探测中的端点
及规范端点。这些操作不启用自动切换。
再次暂存时清除失败或过期的未认可暂存记录，绝不以此方式清除已认可 origin。
重用旧操作键仍返回原来的结果。

链接引擎的构建实现了 `folders.inspect`、`folders.add`、`folders.pause`、`folders.resume`、
`folders.syncNow` 和 `folders.remove`。检查接受已存在的本地目录，自 1.9 起也接受父目录存在、自身尚不存在的目录；返回的五分钟 token
绑定两侧根身份，添加时重新检查。移除映射保留文件及 journal；运行中的映射须先静止。
自 1.9 起移除账号时会自行移除其目录映射。仅基础层的构建拒绝这些引擎操作。

`decisions.list` 及快照公开带有 `localDeletes`、`remoteDeletes`、`digest` 和 `expires` 的
`bulkDeletions`（自 1.9 起还有 `groups`）。`decisions.resolve` 接受 `decisionId`、`proposalDigest` 及取值为
`approveDeletion` 或 `later` 的 `action`；批准消耗待处理决策，
并授权在提案创建后十分钟内进行一次摘要匹配的重新发现。重启使旧提案失效。批准不代表
传播成功：须观察目录状态及可能替换的决策。当前 UI 仍需增加此类决策的操作控件。

首个连通实现提供守护进程生命周期、持久全局暂停、快照订阅、操作去重及 Go 客户端适配。
端点切换协调器作为独立控制线程组件检查信任、身份、epoch 和静止条件；引擎适配器必须
提供实际静止证明后才可激活端点。登录现使用内存中的 Login Flow v2 会话和 Windows
凭据管理器，见[账号](accounts.md)。`accounts.login.status` 以独立于启动操作的会话 ID
查询状态。完整同步和自动切换在完整路径实现前保持能力禁用。Mock 保留为显式开发模式。

Windows 守护进程在当前用户的应用数据目录下拥有按 profile 哈希区分的 Control DB。
SQLite 连接留在 Qt 线程；设置、操作回执和 outbox 行以完全同步持久性一起提交。
终态回执目前无限期保留。`events.resume` 要求重新 bootstrap。新的守护进程启动后，
旧的关闭回执不会再次触发关闭。独立切换守卫尚未连接 libsync。

重叠 Win32 管道拒绝远程客户端，将访问限制到登录 SID，去除客户端创建实例权限，
并保留首实例。双方均检查用户和会话身份；Go 客户端另检查服务端可执行文件路径。
每次服务一个界面连接；唯一管道实例在重连期间保持保留，输出队列有界，停滞 I/O 有截止时间。
前端在守护进程不存在时尝试启动一次
同目录的 `nextcloud-mygo-syncd.exe`。`NEXTCLOUD_MYGO_MOCK=1` 显式选择演示模式。

## 验证

端点选择策略是使用调用者提供的单调时间的确定性控制线程组件。先过滤信任、网络条件和
新鲜度，再应用 PLAN §8 权重；保留独立隔离状态，跟踪绑定 generation/revision 的探测票据，
并限制半开探测。输出仅为提议，绝不发布活动端点或绕过引擎静止条件。启用自动切换前，
仍须接通运行时网络通知、探测调度和切换激活。

连通功能单元已在原生 ARM64 和仿真 x64 上通过共享分帧夹具、持久性/幂等及切换守卫测试。
两种架构的真实 Go 到 C++ 管道测试均通过，覆盖重启持久性、重连及拒绝非预期服务端
可执行文件路径。原生前端已打开配套守护进程；截图显示登录不可用提示及禁用的继续按钮。
Windows 跨用户/会话和冒充验收独立于上述检查，仍待完成。完整同步和引擎驱动切换未验证。
