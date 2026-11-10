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
配对守护进程夹具还在批准的配额内创建 16 MiB 随机自有上传/下载对象。仅在诊断观察到
未结束的内容回复及正内容字节后，才杀死它自己启动的子进程；重启后检查恢复内容哈希及
原生 journal 文件身份。未观察到活动传输的运行会失败，不凭界面状态宣称崩溃验收通过。
全部对象位于夹具预留的集合下，由既有清理流程删除。
扩展还观察正字节、未结束的 16 MiB 传输，显式暂停/取消它，将整个暂停账号切换到另一
认可 origin，恢复后重新核对。双向检查恢复哈希和原生 journal 身份。这是取消加手动跨
origin 恢复，不证明映射目录活动传输期间的自动故障选择。
独立的 `-Scenario network-active` 验收模式使用自有本地 CONNECT 中继限制加密流吞吐量并
打断真实未结束传输。TLS 仍在认可的真实服务器终止，不在中继终止。普通守护进程探测和
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

`tools/build-syncd.ps1` 运行 CTest 时只把 Qt 的 `bin` 加入 PATH。集成测试还需要相同架构的
对应上游构建 `bin/Release` 和原生依赖 `bin` 目录；请在调用前加入，直接运行已构建的可执行
文件时还要设置 `QT_PLUGIN_PATH`。绝不混用来自其他上游构建的头文件或 DLL。

M0 发现生命周期回归测试使用 `./tools/test-discovery-lifetime.ps1 -Executable <build>/bin/Release/RemoteDiscoveryTest.exe`，并将目标 Qt 和依赖 DLL 目录加入 PATH。
它在 20 个有时间上限的子进程中执行上游超时及下一夹具组合，遇到非零退出或超时即失败。
诊断输出保留在忽略的 `build/` 下；这是模拟服务器崩溃回归，不是真实服务器验收。
它已复现补丁 0005 前的崩溃；补丁后在原生 ARM64 上运行 20/20 次通过。

M0 CTest 运行排除 `UtilityTest`，因为其中的启动测试会实际创建开机启动注册项。
必须明确报告此排除项；此配置不验证启动集成。

复用的构建源码通过临时 Git 索引检查：按相反顺序撤销记录补丁，再与记录的上游提交比较。
这样可处理重叠补丁，且不修改源码工作目录及其真实索引。

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
