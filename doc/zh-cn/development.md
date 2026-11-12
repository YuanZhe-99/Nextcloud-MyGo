# 开发

## 环境要求

- `go.mod` 中写明的最新 Go 发行版（较旧的 Go 会自动下载它）。MyGo 是 Go 工具依赖（`go tool mygo …`），
  使用其最新标签。
- Windows：Visual Studio 2026（或其 Build Tools），含 ARM64 与 x64 的 C++ 工具及 Windows SDK；CMake 与
  Ninja。M0 选定它们以及 Qt 和其他原生依赖的版本，`build-lock.json` 记录每次构建实际使用的版本。

## 命令

使用 `git submodule update --init third_party/nextcloud` 初始化所记录的上游快照。
保持检出源码不变；构建树与依赖下载放在被忽略的 `.dev/` 或 `build/` 中。
当前 M0 状态见 [backend-progress.md](backend-progress.md)。

`tools/bootstrap-deps.ps1 -Architecture ARM64`（再运行 `x64`）下载 Qt，并按 `build-lock.json` 的记录精确构建
其他原生依赖到 `.dev/deps`，最后输出 `.dev/deps/env-<arch>.json` 的路径：其中的 `QtPrefix`、`QtHostPrefix` 和
`DependencyPrefix` 就是下文的参数，`PkgConfig` 则作为 `PKG_CONFIG` 供 `tools/build-upstream.ps1` 使用。CI 运行同一个
脚本（[CI](ci.md)）。`tools/build-upstream.ps1 -BuildRoot`、`tools/build-syncd.ps1 -Build` 和
`tools/test-all.ps1 -Build` 可把构建树放到别处，例如在常用构建树之外针对第二个依赖前缀构建。

```bash
gofmt -l cmd internal tools
go vet ./...
go test ./...
CGO_ENABLED=0 go test ./...
go tool mygo vet .
go tool mygo i18n check
go run ./tools/checkdocs           # doc/en-us and doc/zh-cn mirror each other
```

使用 `tools/build-syncd.ps1 -Architecture ARM64 -QtPrefix <target Qt>
-QtHostPrefix <host Qt>` 构建连通守护进程；以 `-Architecture x64` 重复执行。
它构建可执行文件，并运行共享分帧、持久性及切换守卫 CTest 目标。
将目标 Qt 的 `bin` 加入 PATH，`QT_PLUGIN_PATH` 设为其 `plugins`，
将 `SYNCD_TEST_EXE` 设为构建出的守护进程绝对路径，再运行
`go test ./internal/syncd -count=1 -v` 验证真实 Go 到 C++ 管道契约。
测试使用唯一 profile 并在结束后删除；这些命令不安装或注册应用。

用 `tools/package.ps1 -Arch ARM64`（或 `x64`）打包已构建的 SyncD；`go run ./tools/pkg check -arch
ARM64 <package dir>` 重新运行架构检查。`tools/package-smoke.ps1 -Setup <installer>` 以一次性 profile
为当前用户真实安装、重装和卸载该包，并再次删除所有内容；已有安装时拒绝运行。要测试两个版本之间的升级，
用 `tools/package.ps1 -Arch ARM64 -TestVersion 0.1.1` 构建第二个安装包（使用自己的 SyncD 构建树；已提交的
版本文件保持不变），并将其作为 `-UpgradeTo` 传入。见[打包](packaging.md)。

M0 的仅上游构建入口为 `tools/build-upstream.ps1`。在 PowerShell 中传入
`-Architecture ARM64`（或 `x64`）、`-Mode engine`（或 `tests`）、`-QtPrefix`、`-QtHostPrefix`
及 `-DependencyPrefix`。前缀参数为 SDK 或依赖的绝对目录；DependencyPrefix 可包含多个目录。
脚本发现 Visual Studio 的 CMake，核对记录中的干净上游提交，并构建到
`build/<a|x><e|t>-<source fingerprint>`（架构和模式缩写使生成的 MSBuild 路径不超过 Windows
限制）。记录的补丁经哈希检查后仅应用于
生成的源码克隆。引擎模式构建无 GUI 和 Shell 集成的 `nextcloudcmd`；
测试模式启用上游 GUI 测试依赖，并以 `--no-tests=error` 运行 CTest。
它从不安装或启动官方应用。完整依赖组合仍在组装；这不代表构建已成功。

真实服务器 M0 测试入口为 `tools/test-upstream.ps1 -Executable <absolute CLI path>`。
可在 PowerShell 中用 `. ./.dev/nextcloud-test.local.ps1` 加载已批准的本地测试凭据。
此文件仅保存在本地，由 `.gitignore` 的 `/.dev/` 规则排除；不得强制暂存。
在自己的终端设置 `NEXTCLOUD_TEST_URL`、`NEXTCLOUD_TEST_USER` 和 `NEXTCLOUD_TEST_PASSWORD`，
并将匹配的运行时 DLL 目录加入 PATH。它创建唯一远程集合与临时本地配置，检查上传、下载、
修改、冲突内容及 journal 位置稳定性，然后清理自身资源。凭据通过子进程环境传入，不进入参数或
文件；原始客户端输出仅驻留内存，从不打印。测试观察上游请求创建记录，要求首次周期出现
上传和下载请求作为阳性对照，并要求无变化周期包含发现请求，但没有 DAV 内容 GET 或写请求。
此检查已在原生 ARM64 上通过；它不测量传输层字节数。在 Windows 上，
`QT_FORCE_STDERR_LOGGING=1` 将选定 Qt 类别发送到捕获管道而不是调试器。
远程清理失败会明确报告。
不会调用安装器或注册云同步根、Shell 入口。

使用 `tools/test-login.ps1 -Executable <absolute-daemon-path> -SyncFolders -Reauthenticate`
运行真实配对守护进程账号/切换/重新认证夹具。它签发原始及替换应用密码，在内存中自动执行
两次明确授权，检查取消、映射身份、独立暂停、旧密码撤销、恢复上传及重启，然后移除自有
远程集合、凭据及配置。重新认证的一次性浏览器交接通过子进程 stdout 的内存管道消费，
不写文件。这不构成交互浏览器、SSO/MFA 或错误账号验收。
带 `-SyncFolders` 时它并行运行四条通道，每条都有自己的登录（授权、跨重启保持、带确认撤销的删除）、
配置目录和两个自有文件夹：`switching`（带正常重启和终止进程的切换、备用地址上传、已发布的活动）、
`crash`（在真实未完成传输期间终止进程）、`cross`（被取消的传输在另一来源上恢复）和 `renewal`
（递归监视、独立暂停、删除审核、固定策略协调，以及带 `-Reauthenticate` 时的重新认证）。每条通道输出
自己的 `PASS` 行、`STEP <ms> <name>` 时间线和墙钟时间。配置目录名带随机部分：各通道在同一个 Windows
时钟节拍内启动，两条通道共用一个配置目录时会共用一个守护进程并删除对方的账户。共享服务器对每个
重新认证的请求都要花两秒以上，夹具和守护进程都一样。
DAV 阳性回读仅对 GET 在有界预算内重试 HTTP 423；夹具绝不重放修改请求来掩盖写入结果。

`tools/test-reauth-lifetime.ps1 -Executable <absolute-syncd_reauth_tests-path>` 在真实嵌套
DAV 验证期间执行取消/新会话替换。它仅将批准的凭据作为借用的只读测试凭据，不授权或撤销
应用密码，并等待确切子进程。父进程在唯一且被忽略的配置目录下预留并校验原生凭据键清理
清单；子进程崩溃后也执行清理。夹具在本地登录会话租约修复前复现访问冲突 `0xC0000005`。
这是生命周期回归，不是活动传输崩溃验收。
同一父进程包装器接受 `-Scenario network`，运行隔离的真实网络验收模式。子进程仅以只读
方式借用凭据，只认可提供的两个 HTTPS origin，并以进程局部 Qt 代理故障拒绝选定连接。
不更改系统代理、DNS、路由、防火墙、注册表或 Explorer 设置。它通过实际 HTTPS 检查验证
自动故障选择、全坏等待及固定目标拒绝；不使用 HTTP 模拟代替 DAV 验收。
配对守护进程夹具还在批准的配额内创建 4 MiB 随机自有上传/下载对象。仅在诊断观察到
未结束的内容回复及正内容字节后，才杀死它自己启动的子进程；重启后检查恢复内容哈希及
原生 journal 文件身份。未观察到活动传输的运行会失败，不凭界面状态宣称崩溃验收通过。
全部对象位于夹具预留的集合下，由既有清理流程删除。
扩展还观察正字节、未结束的 4 MiB 传输，显式暂停/取消它，将整个暂停账号切换到另一
认可 origin，恢复后重新核对。双向检查恢复哈希和原生 journal 身份。这是取消加手动跨
origin 恢复，不证明映射目录活动传输期间的自动故障选择。
独立的 `-Scenario network-active` 验收模式使用自有本地 CONNECT 中继在某个字节偏移处保持
真实传输未完成，然后将其打断。TLS 仍在认可的真实服务器终止，不在中继终止。普通守护进程探测和
策略必须完成取消、排空、故障切换，并使用原 journal 核对映射根。仅创建一个唯一预留的
远程集合；即使子进程崩溃，父进程清理也验证其清单并删除它。不更改系统网络或重放结果
不明的用户写入。此模式处于实现/验收中。
其中的确认丢失场景仅扣留大上传隧道的加密回复，通过另一认可 origin 证明 PUT 已可见，
再于守护进程收到成功前断开旧连接。恢复必须保留结果不明状态并重新发现；夹具不会告诉
守护进程隐藏的 PUT 已成功。这与早期打断上传是独立场景。
`-Scenario wrong-instance` 是独立只读 N02 夹具，要求两个真正隔离的 Nextcloud 实例，使用
相同的新建测试 UID/凭据但不同的 DAV 命名空间身份，并已显式认可两个精确 HTTPS origin。
它必须在命名空间验证而非仅认证阶段失败，持久化隔离、拒绝激活并保留规范绑定。本地
证书信任要求用户明确许可。错误实例测试程序仅接受已认可的公有根指纹，只加入自身 Qt
进程配置，不加入 Windows 信任库；仍启用正常证书链及主机名检查，绝不关闭 TLS 验证。隔离实例
凭据及清理清单仅留在忽略的本机配置中，不进入提交的夹具。

`-Scenario network-chunked` 在第二个分块内切断一个分块会话，并要求它在另一个 origin 上续传
且不重发第一个分块。上游仅对超过 100 MiB 的文件分块，因此它上传 112 MiB，需要没有小配额
的隔离服务器，并使用同样的夹具本地根证书（`SYNCD_TEST_CA_CERT`、`SYNCD_TEST_CA_SHA1`）。
Qt 对 `127.0.0.1` 或 `localhost` 的请求从不经过代理，这会绕过故障中继：请通过两个保留的
`.test` 名称访问回环服务器，这两个名称须出现在它的证书和 `trusted_domains` 中，并把
`NEXTCLOUD_TEST_CLEANUP_URL` 设为直接的回环 origin，供不经过中继的清理步骤使用。失败时
脚本输出已到达的 `PASS` 行和一条脱敏原因。
隔离夹具服务器由 `NEXTCLOUD_TEST_CLEANUP_URL`（中继连接到该主机）和 `SYNCD_TEST_CA_CERT`
（已认可的根证书）识别；两者都设置时，下面的每个传输场景和 `network-active` 都可以对它运行。

`-Scenario network-uncertain` 丢弃真实服务器已提交的写入的回复，需要同样的隔离服务器。夹具
把它指名路径的那一个请求送过一个只转发请求、绝不转发回复的中继，等到另一个 origin 已显示
写入效果而守护进程仍在等待时，再切断主地址。它覆盖对既有条目的 MOVE 和 DELETE（恢复前由
另一个客户端在原路径重新创建对象），以及 112 MiB 分块会话的最终 MOVE。恢复必须让重新创建的
对象和已提交的目标保持不变（服务器文件 ID 和 ETag 相同），且不重发任何分块；夹具绝不告诉
守护进程服务器做了什么。

`-Scenario switch-kill` 在真实切换的某个持久阶段结束它自己的子进程，再在同一配置目录上重启：
`prepared`（意图已保存）、`quiescing`（旧地址上仍有上传在进行）、`verifying`（即将发送对
目标的带凭据检查）、`committed`（绑定已发布，新 epoch 尚未发出任何请求）和 `reconciling`
（文件的一部分已发送到新地址）。`-Stage` 选择阶段；每个阶段使用全新的配置目录。重启后的
子进程除自身存储外得不到任何提示：前三个阶段必须停在原绑定上，后两个阶段必须停在已提交的
绑定上，并使用新的 epoch、相同的凭据引用、目录绑定和原生 journal 文件，哈希一致，且已核对
的文件不再次传输。它的内容为 8 MiB，因此也可以对共享测试服务器运行。

`-Scenario multi-account` 针对一次性服务器，在一个守护进程上运行两个账号、每个账号两个目录（`PLAN.md` §2.1、
R05）。它再次登录夹具用户并要求得到 `ACCOUNT_EXISTS`，用随机密码创建第二个服务器用户（先记录在 profile
的 `owned-user.json` 中，由本次运行和父进程的 `--cleanup` 删除），通过生产登录路径添加两个账号，为每个
账号映射两个目录（其中一个本地目录尚不存在），在全部四个目录中双向传输内容，在第二个账号的目录持续上传
时把第一个账号切换到另一个地址（这些目录都不得出现 `switchInProgress` 阻断或暂停，其绑定与 epoch 不得
改变），给第二个用户设置 1 KB 配额使其上传被拒绝（目录显示 `REMOTE_QUOTA_EXCEEDED`），在配额已满时为该账号提交
固定策略，要求第一个账号继续同步、守护进程继续服务，并在第一个账号继续同步时移除第二个账号。它需要允许夹具用户
创建用户的服务器，因此只在服务器层运行。

`-Scenario switch-quota` 在一次性服务器上重现 B14 第 6 步中消失的切换。它创建一个自有服务器用户（与
`multi-account` 一样记录在 `owned-user.json`），登录该用户，批准备用地址并映射一个目录，用
`NetworkTunnel::setStallAfter` 让主地址上的 64 MiB 上传保持未完成，再像“使用此地址”那样请求备用地址
（固定策略的 `endpoints.setPolicy`）。上传被挂起期间，网络必须显示待处理的请求（`switching`、
`requestedEndpointId`、`postponed`）；30 秒排空窗口过后，请求必须以带 `QUIESCE_STALLED`、两个地址及名称
的 `switchFailed` 活动结束，绑定与策略不变，回执失败。随后放行上传，通过 provisioning API 给该用户 1 MB
配额，使下一次上传为 `REMOTE_QUOTA_EXCEEDED`，再次请求并要求切换发生且被记录，最后移除账号和用户。
修复之前，请求消失且没有任何记录，运行停在其结果一步。

`-Scenario slow-server` 让中继对客户端的每一轮发送都晚 1.5 秒应答（`NetworkTunnel::setReplyDelay`；新连接上的
一次健康探测因此约需 4.5 秒），并要求两个地址都保持健康：可达但应答缓慢的服务器不是 `NETWORK_UNAVAILABLE`。
引入 `metadataDeadline` 之前，它在 15 秒内失败。

`-Scenario file-safety` 在真实服务器上运行无故障用例：双向重命名和目录移动且保留服务器文件
ID、对含十二个后代的单个目录的删除确认（方向和数量）、后续变化之后的过期批准、被租约持有
的本地根，以及返回 404 的远程根。`-Scenario network-large` 在超过 4 GiB 的文件越过 4 GiB
边界之后切断其分块会话，并要求它在另一个 origin 上续传；内容以流的方式写入、发送和校验，
需要有约 9 GiB 可用空间的隔离服务器。
`-Scenario held-revocation` 同样需要隔离服务器：它用夹具自己的密码签发一个应用密码，把它
记录为被中断登录的搁置撤销，重启 Control，并要求在 60 秒下限之后只发送一次 DELETE、得到
服务器确认，且该密码此后被拒绝。夹具自己的密码绝不会被撤销。

`tools/build-syncd.ps1` 运行 CTest 时只把 Qt 的 `bin` 加入 PATH。集成测试还需要相同架构的
对应上游构建 `bin/Release` 和原生依赖 `bin` 目录；请在调用前加入，直接运行已构建的可执行
文件时还要设置 `QT_PLUGIN_PATH`。绝不混用来自其他上游构建的头文件或 DLL。

M0 发现生命周期回归测试使用 `./tools/test-discovery-lifetime.ps1 -Executable <build>/bin/Release/RemoteDiscoveryTest.exe`，并将目标 Qt 和依赖 DLL 目录加入 PATH。
它在 20 个有时间上限的子进程中执行上游超时及下一夹具组合，遇到非零退出或超时即失败。
诊断输出保留在忽略的 `build/` 下；这是模拟服务器崩溃回归，不是真实服务器验收。
它已复现补丁 0005 前的崩溃；补丁后在原生 ARM64 上运行 20/20 次通过。

M0 CTest 运行排除 `UtilityTest`，因为其中的启动测试会实际创建开机启动注册项。
必须明确报告此排除项；此配置不验证启动集成。
它们还排除 `SyncCfApiTest`，该测试会注册临时同步根，且有一个用例会打开资源管理器窗口。请用
`tools/test-cfapi-upstream.ps1 -Build <上游测试构建树>` 运行它（PATH 中需有目标 Qt 和依赖的 `bin` 目录）：
它跳过该用例，若遗留 `OC-TEST` 注册项或上游的 Shell 扩展类注册项则失败。

`tools/test-ondemand.ps1 -Architecture ARM64|x64 [-Build <SyncD 构建树>]` 是真实的按需运行
（[按需文件](on-demand.md)）。它用 `tools/test-server.ps1` 启动一次性服务器，在 150 秒内运行
`syncd_ondemand_acceptance`（其中每一步都有各自的期限），移除服务器，并在运行期间注册的
`Nextcloud-MyGo!` 同步根仍存在时失败。`-State` 复用已在运行的服务器。CTest 目标 `SyncDOnDemandContracts`
不需要服务器。

复用的构建源码通过临时 Git 索引检查：按相反顺序撤销记录补丁，再与记录的上游提交比较。
这样可处理重叠补丁，且不修改源码工作目录及其真实索引。

## 测试分层

`tools/test-all.ps1 -Tier fast|server -Architecture ARM64|x64` 是唯一的入口。它运行
`build/syncd-<arch>` 中构建的 SyncD，并使用其 CMake 缓存记录的 Qt 和上游 DLL；每个场景输出一行
`PASS`/`FAIL` 及其耗时，最后输出一行 `TOTAL`；遇到第一个失败即停止，总时长超出该层预算时判为失败。
失败的场景不会重跑，无论是在同一架构还是另一架构上。

| 层 | 预算 | ARM64（原生） | x64（在 ARM64 主机上模拟） |
|---|---|---|---|
| `fast` | 2 分钟，无服务器 | `gofmt`、`go vet`、包含真实 Go↔C++ 管道测试的 `go test ./...`、全部 SyncD CTest 目标 | 为 amd64 构建的 `go test ./...`、全部 SyncD CTest 目标 |
| `server` | 10 分钟，含容器 | 下文所有真实服务器场景，分六个并行通道 | 一个冒烟场景（在 `committed` 阶段的 `switch-kill`） |

服务器层用 `tools/test-server.ps1 -Start` 启动自己的一次性 Nextcloud（默认 `wslc`，在 Docker 能运行 Linux
容器的地方用 `-Runtime docker`；GitHub 的 Windows 运行器不能，因此 [CI](ci.md) 只运行快速层），结束后用 `-Stop` 删除：一个带标签、使用匿名卷、只监听回环端口的容器；仅当本次运行拉取了镜像时才删除镜像；
夹具本地根证书和管理员密码在此之前保存在被忽略的 `build/test-server-<id>/` 中。该根证书只在各验收进程内部受信任
（`SYNCD_TEST_CA_*`）；不修改任何信任存储、hosts 文件或代理设置，TLS 检查始终开启。客户端通过夹具中继以
`primary.syncd.test` 和 `alternate.syncd.test` 访问服务器，清理步骤和 `lifetime` 用例使用直接回环源。
该层不需要私有服务器。`-ServerVersion` 选择服务器版本（默认 `35.0.2`，即最新版）；也要用上一个受支持主版本的
最新发行版运行（2026-11-12 使用 `-ServerVersion 34.0.5`：ARM64 通过，用时 431.4 秒）。

每个用例都在独立的进程和配置目录中运行，因此每个用例都从健康的端点开始，而不必等待前一个用例留下的熔断历史：
`network-active` 将 `upload`、`download` 和 `unacknowledged-upload` 作为独立用例运行，`network-uncertain`
运行 `move`、`delete` 和 `final-move`（在 `tools/test-reauth-lifetime.ps1` 中用 `-Case` 选择），
`switch-kill` 逐个运行 `-Stage`。相互独立的用例共享容器并在并行通道中运行。中继不再限速：
传输在某个字节偏移处被保持为未完成（`NetworkTunnel::setStallAfter`，仅作用于主地址或备用地址的中继，
小于 1 MiB 的请求不受影响），再由用例指定的故障打断。用例剩余的大部分时间是守护进程自身的故障切换、
熔断和协调时序，测试不缩短这些时序。

每一步都有自己的截止时间，失败时给出步骤名称：C++ 等待默认 60 秒，并输出一行 `STEP <ms> <name>`，
子进程超出 150 秒预算时由包装脚本报告该行（`network` 为 240 秒，因为它要等待两个熔断打开窗口）；
`SYNCD_TEST_STEPS=1` 输出所有步骤时间。Go 探针为每次等待留 90 秒（登录 150 秒）、每次命令重试留 30 秒、每条通道的阶段留 5 分钟，
失败时给出步骤名称（命令以其源代码位置命名）和守护进程脱敏后的状态；即使某一步忽略了自己的 context，看门狗也会在阶段预算之后不久输出 goroutine 位置并退出。
没有就绪连接时发出的调用以“未连接”失败，因此已不在的守护进程绝不会被误认为忙碌。
所有包装脚本都会在一个拥有关闭即终止 Job Object 的子 `pwsh` 中重新启动自身（`tools/test-job.ps1`），
探针也把自己放入一个这样的 Job Object，因此失败、被终止或被中断的运行不会留下守护进程、探针或测试可执行文件。
清理是直接的：探针的 `abort` 先通过守护进程删除自有账户（最多等待其 60 秒崩溃退避），再删除自有集合和根目录；
它从不重放流程。其客户端在整个 `abort` 期间存在；它曾只存在于 75 秒的连接等待内，在较慢的账户删除中途关闭了自己的管道。守护进程拒绝删除账户时，`abort` 保留配置目录、根目录和状态文件并说明原因，
因为只有根目录存在时，被中断的切换才能完成。

预算之外、需显式开关的项目：`-Large` 加入 `network-large`（一个超过 4 GiB 的分块会话在 4 GiB 边界之后被切断，
约 30 分钟，服务器需约 9 GiB 可用空间）；`-SharedServer` 加入针对 `NEXTCLOUD_TEST_*` 中已批准共享服务器的配对守护进程夹具
`tools/test-login.ps1`：配对守护进程只信任公共受信任的证书链，因此不能使用一次性服务器。`wrong-instance`
需要第二个实例，单独运行。
## 运行界面

```bash
go build -o build/app/nextcloud-mygo.exe ./cmd/nextcloud-mygo # put paired SyncD beside it
NEXTCLOUD_MYGO_MOCK=1 go run ./cmd/nextcloud-mygo            # sample data
NEXTCLOUD_MYGO_MOCK=1 NEXTCLOUD_MYGO_FRESH=1 go run ./cmd/nextcloud-mygo # demo setup
NEXTCLOUD_MYGO_PROFILE=dev go run ./cmd/nextcloud-mygo       # a separate instance
SCREENSHOT_DIR=/tmp/shots go test ./internal/gui             # every page as PNG
go tool mygo generate                                        # after editing locales/*.json
```

正常启动连接同目录的 `nextcloud-mygo-syncd.exe`。在守护进程声明已实现登录能力之前，
登录保持禁用。显式 Mock 模式不修改服务器文件，并注明其示例数据。

## 测试原则

测试保护产品契约和容易出错的逻辑，优先表驱动；每个修复的缺陷都有回归测试，且每个测试注明它覆盖的不变量或
验收用例。调度、评分、退避与切换使用可控时钟。需要真实 Nextcloud 服务器的测试只在设置了相应环境变量时运行。
验收中，HTTP mock 从不替代真实服务器的 DAV、文件锁与分块上传行为。
