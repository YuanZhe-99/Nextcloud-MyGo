# 打包与安装

Nextcloud-MyGo 每个架构提供一个当前用户安装包。每个安装包包含界面、与之一起验证的 SyncD，以及它们
实际加载的 DLL，仅此而已。每个包都是没有 Authenticode 签名的**测试构建**：没有 Windows 代码签名证书，
因此首次安装时 SmartScreen 会警告；安装器、应用列表条目和快捷方式都会注明。用更新签名密钥构建的包还带有
[签名更新](#签名更新)，已安装的副本在应用内取用，用户开启之前保持关闭。

## 工具

安装器是一个小型 Go 程序 `packaging/windows/setup`，按安装包的架构交叉编译。没有采用 MyGo 自带的 NSIS
安装器：NSIS 会把 x86 的 `Uninstall.exe` 写入安装目录，而 ARM64 包不得包含它。用 WiX 构建 MSI 需要
.NET SDK 并接受 WiX 的许可条款，其自定义操作仍需原生代码来停止 SyncD。Go 程序只需要 Go 工具链，在
ARM64 与 x64 上原生运行，通过 `internal/syncd` 与 SyncD 通信，并与包内其他文件接受同一个架构检查器检查。

| 组成 | 路径 | 作用 |
|---|---|---|
| 打包脚本 | `tools/package.ps1` | 构建界面，暂存并检查安装包，构建安装器 |
| 检查与暂存工具 | `tools/pkg` | DLL 闭包、清单、PE 架构检查、运行中 SyncD 的报告 |
| 安装/卸载程序 | `packaging/windows/setup` | 当前用户安装、升级与卸载 |
| 清单 | `internal/release` | 包内文件及其两半所用的契约 |
| 安装测试 | `tools/package-smoke.ps1` | 在本机真实安装、重装、升级和卸载 |

## 构建安装包

```powershell
./tools/bootstrap-deps.ps1 -Architecture ARM64   # prints .dev/deps/env-arm64.json
./tools/build-syncd.ps1 -Architecture ARM64 -QtPrefix <target Qt> -QtHostPrefix <host Qt>
./tools/package.ps1 -Arch ARM64        # or x64; SyncD from build/syncd-<Arch>
go run ./tools/pkg check -arch ARM64 build/package/ARM64/Nextcloud-MyGo-0.1.0-ARM64
./tools/package-smoke.ps1 -Setup build/package/Nextcloud-MyGo-0.1.0-arm64-setup.exe
```

测试两个版本之间的升级：

```powershell
./tools/package.ps1 -Arch ARM64 -TestVersion 0.1.1
./tools/package-smoke.ps1 -Setup build/package/Nextcloud-MyGo-0.1.0-arm64-setup.exe `
    -UpgradeTo build/package/Nextcloud-MyGo-0.1.1-testversion-arm64-setup.exe
```

`tools/package.ps1` 从 SyncD 构建的 `CMakeCache.txt` 读取其上游构建，再读取该构建的前缀（Qt、原生
依赖、vcpkg），因此 DLL 只来自 SyncD 实际链接的构建树，外加已安装 Visual Studio 中最新的 Visual C++
运行库。每一步都有时限。它写出 `build/package/<Arch>/Nextcloud-MyGo-<version>-<Arch>/` 与
`build/package/Nextcloud-MyGo-<version>-<arch>-setup.exe`（0.1.3 之前为 `-test-setup.exe`），不安装任何东西。暂存之后，它运行已暂存
SyncD 的 `--version`，其版本、Control schema 或协议与清单不一致时即失败，因此安装包绝不会把界面与由其他
源码构建的 SyncD 配在一起。

### 测试版本

`-TestVersion X.Y.Z` 为升级测试构建另一个版本的安装包，不修改 `mygo.json` 或 `internal/version`。它配置
一个独立的 SyncD 构建树 `build/syncd-<Arch>-testversion`，使用 `-SyncDBuild` 的 CMake 缓存中的 Qt 前缀、
宿主 Qt 与上游构建，并设置 `BUILD_TESTING=OFF` 与 `SYNCD_TEST_VERSION=X.Y.Z`（以它代替 `PROJECT_VERSION`
作为 `SYNCD_VERSION`），在其中只构建 SyncD 与 CFAPI 插件；从不写入 `-SyncDBuild`。界面以
`MYGO_LDFLAGS=-X .../internal/version.Version=X.Y.Z` 链接（`Version` 为此是变量），暂存工具收到
`-test-version`，据此设置清单的版本与 `testVersion: true`。输出为 `build/package/<Arch>-testversion/` 与
`Nextcloud-MyGo-X.Y.Z-testversion-<arch>-setup.exe`；其安装器会说明版本是为测试而标记的。界面可执行
文件的文件属性仍为 `mygo.json` 的版本。这样的安装包绝不是发布版本。

### 许可证文本

没有其所含内容的许可证文本，就不生成安装包（GPL 义务，PLAN.md §16.5）。`tools/package.ps1` 为该 SyncD 构建运行
`tools/licenses.ps1`（[CI](ci.md)），并把结果作为 `-notices` 传给暂存工具。`-notices` 缺失，或缺少
`LICENSE.txt`、`THIRD_PARTY_NOTICES.md`、`SOURCE.txt`、Nextcloud Desktop 的 `COPYING`、Qt 模块 qtbase、qt5compat、
qtsvg 与 qtwebsockets 的文本、OpenSSL、SQLite、zlib、QtKeychain、KArchive 与 libp11 的文本以及 Go 许可证中的任何一项时，
`pkg stage` 在写入任何东西之前就拒绝。原生文本只存在于由 `tools/bootstrap-deps.ps1` 构建的前缀中，因此安装包从这些
前缀构建；手工组装的 `.dev` 树不能作为安装包的来源，`tools/licenses.ps1` 会提示用引导脚本重新构建。

安装后的副本把许可证包保存在其版本目录中：`LICENSE.txt`（GPL-3.0）、`THIRD_PARTY_NOTICES.md`、`SOURCE.txt`
（对应源代码的位置）以及 `licenses\<component>\`。这些文件与其他文件一样在 `manifest.json` 中列出并记录哈希，
因此安装器会校验它们，升级或卸载时随目录一起替换或删除。发布中的 `Nextcloud-MyGo-<version>-licenses.zip`
为每个架构包含同样的许可证包。

暂存工具从两个可执行文件、Qt 插件 `tls/qopensslbackend.dll` 与 `sqldrivers/qsqlite.dll` 以及
`uninstall.exe` 出发，跟随所有普通导入与延迟加载导入，复制在搜索目录中找到的每个 DLL；遇到既不在搜索
目录中、也不属于 Windows 的导入即停止。`libssl-3-<arch>.dll` 与 `libcrypto-3-<arch>.dll` 按名称加入
（`-runtime`）：Qt 的 OpenSSL 后端在运行时打开它们，而不是导入。Visual C++ 运行库总是随包提供，从不
依赖已安装的可再发行组件。SyncD 导入上游的 CFAPI 插件（`nextcloudsync_vfs_cfapi.dll`），因此闭包会把它放在 SyncD
旁边，上游按名称从那里加载它；其 Shell 扩展不构建，不随包提供任何由资源管理器加载的 DLL。它写入 `qt.conf`，使 Qt 只从版本目录的 `plugins` 加载插件。

## 安装布局

```text
%LOCALAPPDATA%\Programs\Nextcloud-MyGo\
  install.json                 {"current": "versions/0.1.0-1a2b3c4d"}
  uninstall.exe                copy of the current version's uninstaller
  versions\0.1.0-1a2b3c4d\
    Nextcloud-MyGo.exe         UI
    nextcloud-mygo-syncd.exe   SyncD
    Qt6*.dll, nextcloudsync.dll, qt6keychain.dll, libssl-3-*.dll, sqlite3.dll, MSVCP140*.dll, ...
    plugins\tls\qopensslbackend.dll
    plugins\sqldrivers\qsqlite.dll
    qt.conf, sync-exclude.lst, manifest.json, LICENSE.txt, THIRD_PARTY_NOTICES.md, SOURCE.txt, licenses\
```

用户数据留在各进程自己的位置，不在安装目录内：SyncD 的 profile 位于
`%LOCALAPPDATA%\Nextcloud-MyGo\syncd\<profile hash>`，界面设置位于 `%APPDATA%\Nextcloud-MyGo`。

## 清单

`manifest.json` 记录产品、版本与架构；`testBuild: true`、`signed: false` 与 `autoUpdate: false`；
仅 `-TestVersion` 安装包带有 `testVersion: true`；
每一半的可执行文件与协议版本（取自 `protocol/contract.json`）；SyncD 的 Control schema（读取
`backend/control.cpp` 中唯一一处 `PRAGMA user_version=` 赋值，不是恰好一处则暂存失败）；以及每个文件的
大小和 SHA-256。安装器先按清单校验解出的文件，新版本才可能成为当前版本。

## 架构检查

`go run ./tools/pkg check -arch ARM64|x64 <dir or file>...` 不需要服务器，也不调用 Windows API。它读取
每个以 `MZ` 开头的文件（无论文件名），以下情况失败：

| 发现 | 规则 |
|---|---|
| ARM64 包中的 x64 或 x86 映像，x64 包中的 ARM64 或 x86 映像 | 失败 |
| ARM64EC 映像（带 CHPE 元数据的 AMD64 machine） | 失败 |
| ARM64X 映像（带 CHPE 元数据的 ARM64 machine） | 失败，Visual C++ 运行库除外 |
| 包内与 Windows 都不提供的导入 | 失败 |
| 只能由已安装可再发行组件满足的 Visual C++ 运行库导入 | 失败 |
| 没有图标资源（`RT_GROUP_ICON`）的 `nextcloud-mygo-syncd.exe`，资源管理器用它显示其同步根 | 失败 |
| 没有图标资源的 `uninstall.exe` 或 `Nextcloud-MyGo-*-setup.exe`（从 0.1.3 起） | 失败 |

Microsoft 只以 ARM64X 形式提供 ARM64 Visual C++ 运行库。ARM64 进程映射其 ARM64 代码，从不映射其 ARM64EC
代码，因此它原生运行；检查器把每个此类文件作为提示列出。其他所有混合映像都失败。用户已于 2026-11-12 接受此例外。检查器的测试构造合成
PE 文件，因此 CI 可在任何平台运行。

## 安装与升级

从 0.1.3 起，setup 和 `uninstall.exe` 是窗口程序（Windows GUI 子系统，`ui_windows.go`）：Nextcloud 图标、“Nextcloud MyGo”和版本、
一条消息、显示当前步骤的进度条，以及“安装”“移除”“完成”或“关闭”和“取消”。安装界面说明它是全新安装、更新已安装的版本（账号、设置和同步的
文件都保留）还是重新安装，并用一行低调的文字说明此副本未经发布者签名（`-TestVersion` 安装包还会说明其版本是为测试而标记的）；控制台中的
“TEST BUILD”一行已去掉，而清单、应用列表条目和快捷方式仍注明测试构建。点“安装”后显示每一步，然后是“打开 Nextcloud MyGo”（默认勾选）和
“完成”；被拒绝时显示“Setup did not finish.”及下文相同的消息，并提供“关闭”。工作进行中无法关闭窗口。卸载界面询问“Remove Nextcloud MyGo?”，
说明同步的文件会保留，并提供“同时移除我的账号和设置”（`-remove-user-data` 会预先勾选）。两个可执行文件都带有图标、版本信息（FileDescription
为“Nextcloud MyGo Setup”或“Nextcloud MyGo Uninstall”、ProductName、FileVersion 与 ProductVersion、LegalCopyright）和清单（Common Controls 6、
按显示器的 DPI 感知、`asInvoker`，因此 Windows 从不提升其权限）。`pkg resources` 把它们写成 COFF 对象（`rsrc_windows_<goarch>.syso`，只用
标准库），`tools/package.ps1` 在每次构建前放入、构建后删除；没有它时 `go build` 仍可工作，只是没有图标和清单。

| 参数 | 行为 |
|---|---|
| 无 | 显示窗口，等待点“安装” |
| `-uninstall` [`-remove-user-data`] | 卸载窗口 |
| `-update` | 立即显示进度窗口，无需点击；成功时显示“已更新”，2 秒后关闭，并启动新版本；在已关闭界面之后失败时，再次启动已安装的界面 |
| `-quiet` | 不显示窗口；与早期版本相同的控制台输出（`tools/package-smoke.ps1` 使用它） |
| `-no-launch` | 之后不启动应用 |

各步骤的文字也总会写到标准输出，因此应用内更新会把它们保留在 `update\setup.log` 中。`-update` 或 `-no-launch` 与 `-uninstall` 同用、
不带 `-uninstall` 的 `-remove-user-data` 以及未知参数都会被拒绝，退出码为 2（“取消”也是）；失败时退出码为 1。

以下情况下，安装器在写入任何内容前拒绝：

- Windows 运行的架构与安装包不同（x64 包在 ARM64 上会以仿真运行）；
- 已安装版本更新，或其 Control schema 更新（PLAN.md §16.3）；
- 已安装的是另一架构的包：先卸载；文件与设置保留；
- setup 自身以 UAC 管理员的完整令牌运行（“以管理员身份运行”，或 Windows 的安装程序检测提升了没有清单的 setup，0.1.2 的就是如此）：
  它会以提升权限启动应用，而应用绝不以提升权限运行。没有 UAC 拆分令牌的账号所有程序都在同一级别运行，可以安装。

否则按以下顺序安装、重装或升级：

1. 解压到新的 `versions\<version>-<random>.partial`，按清单校验后再重命名。失败只删除这个 partial 目录。
2. 关闭界面（无状态客户端），使其无法再次启动 SyncD；SyncD 的管道一次只服务一个客户端，因此这也为安装器
   腾出管道。
3. 若已安装版本的 SyncD 正为此 profile 运行，则连接它（`syncd.ConnectRemote`，从不启动 SyncD），请求更新
   安全点 `daemon.prepareUpdate(<要安装的版本>)`。有删除等待复核（`DECISION_PENDING`）时立即停止安装：
   安装从不替用户作出该决定。切换、续期、地址检查或登录仍在进行、运行中的周期尚未结束
   （`QUIESCE_STALLED`）或尚无应答时，每秒重试，最长 2 分钟；之后与其他任何拒绝一样停止安装。期间退出的
   SyncD 无需准备。停止时删除新目录，重新启动已关闭的界面并让 SyncD 继续运行；安装器会说明原因（见下文）。
4. 向 SyncD 发送 `daemon.shutdown` 并最多等待 30 秒；未停止的 SyncD 被终止，它会像处理任何崩溃一样恢复。
   若仍有进程存活，删除新目录，已安装版本保持为当前版本。
5. 把 `uninstall.exe` 复制到根目录，让 `install.json`、开始菜单快捷方式与应用列表条目指向新目录，并把
   启动此安装的开机启动项改为启动新界面，保留其参数以及用户在任务管理器中的启用/禁用选择。
6. 删除其他版本目录，然后启动新界面，由它启动自己的 SyncD。

安装器在第 3 步停止时输出以下信息，其后均接 "the installed version was kept"：

| 情形 | 信息 |
|---|---|
| 应用以管理员身份运行（在关闭任何内容之前检查，卸载也检查） | Nextcloud MyGo is running as administrator, so setup cannot close it. Quit Nextcloud MyGo from its tray icon (Quit and stop syncing), then run setup again |
| 删除等待复核 | Nextcloud MyGo is waiting for you to review a deletion. Open Nextcloud MyGo, review it, then run setup again |
| 2 分钟后仍忙 | Nextcloud MyGo is still busy (signing in, checking or switching a server address, or finishing a sync) and did not get ready within 2 minutes. Wait a few minutes, then run setup again |
| 2 分钟后仍无应答 | Nextcloud MyGo's background sync is running but did not answer within 2 minutes. Quit Nextcloud MyGo from its tray icon, then run setup again |
| 其他任何拒绝 | Nextcloud MyGo could not get ready for the update. Quit Nextcloud MyGo from its tray icon, then run setup again |

只有从已安装版本目录运行的本 profile 的 SyncD 会被准备；安装根目录下的其他 SyncD 进程（其他 profile 的）
只按第 4 步停止。卸载按第 2 步与第 4 步停止 SyncD，不经过安全点。

新的 SyncD 从不与其他版本的 Qt DLL 共用目录，旧版本只在新版本完整且已成为当前版本后才删除。同样的规则
使同版本重装也完整切换到一个全新目录。

## 签名更新

一次更新就是新版本的当前用户安装器，通过 MyGo 的更新格式和密钥交付，因此界面和 SyncD 仍按上面的步骤一起
更换（PLAN.md §16.2）。不使用 MyGo 自带的 `Update.Install`：它替换运行中应用所在目录的文件，在这里会让新
界面与旧 SyncD 及其 Qt DLL 并存。

| 组成 | 位置 | 内容 |
|---|---|---|
| 密钥对 | `mygo keygen`；公钥在 `mygo.json` 的 `updates.publicKey`，私钥只在 GitHub secret `MYGO_UPDATER_PRIVATE_KEY` 中 | Ed25519，base64，与 MyGo 的存储方式相同 |
| 归档 | `Nextcloud-MyGo-<version>-<arch>-update.zip` | `update.json`（`product`、`version`、`arch`）与 `setup.exe`，不压缩存储 |
| 清单 | `update-windows-<goarch>.json`（`arm64`、`amd64`） | MyGo 的清单：`version`、`notes`、`date`、归档的 `url`（相对于清单）、`size`，以及 `signature`，即对归档 SHA-256 的 Ed25519 签名 |
| 更新源 | `https://github.com/<updates.github>/releases/latest/download/` | 最新的 release |

设置了 `MYGO_UPDATER_PRIVATE_KEY` 时由 `tools/package.ps1` 生成：它把 `release.UpdateFeed` 和
`release.UpdateKey`（mygo.json 中的公钥）链接进界面，并在安装器检查通过后运行 `pkg sign-update`；该命令
拒绝不属于这个公钥的私钥，并把两个文件写入 `build/package/update-<Arch>/`。`mygo build` 从不获得密钥（它
自己的归档只含界面；它会警告没有签名更新）。没有密钥时（所有本地构建都是如此），界面没有更新源，不提供
更新并会说明，也不写任何更新文件。`-UpdatePublicKey` 只能与 `-TestVersion` 一起使用，信任另一个公钥，用于
以一次性 `mygo keygen` 密钥对进行的测试。

已安装的界面（`internal/gui`，[更新](gui.md#更新)）读取清单，只有版本更新时才把归档下载到安装文件夹中的
`update\`（卸载会删除它；下次启动时清空），按内置公钥检查其大小和签名，并且除非签名的 `update.json` 写明
Nextcloud-MyGo、清单中的版本、本机架构以及比运行版本更新的版本，否则拒绝：清单本身未签名，因此改过的清单
既不能提供更旧的已签名包，也不能提供另一架构的包。随后它请 SyncD 到达安全点，在那里停止它
（`daemon.shutdown`，最多等待 30 秒直到连接结束），再启动 `setup.exe -update`（`release.StartSetup`，输出在
`update\setup.log`）；setup 关闭界面并执行第 1 到第 6 步，此时没有需要准备的 SyncD，因此上面的每种拒绝和失败都保留
已安装版本。如果 setup 在界面仍运行时就返回（它提前拒绝了），界面会再次启动 SyncD。0.1.2 及之前，界面让 SyncD 停在
安全点，setup 再次请求；SyncD 在已有请求保持期间会拒绝第二次请求，因此 setup 等满 2 分钟后停止，应用内更新从未成功安装。只获取 HTTPS，测试时也可以是到环回地址的 HTTP；重定向不得离开 HTTPS。

## 卸载

`uninstall.exe -uninstall`（应用列表条目的命令）按下面的顺序执行，以保证只要仍可能有按需目录处于注册状态，
就不会删除任何程序文件（否则它们的仅联机文件将无法再打开）：

1. 关闭界面（无状态客户端），同上。
2. 启动该 profile 已安装的 SyncD，对每个按需目录调用 `folders.remove`。SyncD 结束该目录的 Windows 同步根注册：
   已下载的文件变为普通文件，仅联机占位文件在本地被删除（服务器仍保存它们），journal、账号和设置保留。卸载程序先等待
   仍待处理的自动转换最多 20 秒使其落定（[按需文件](on-demand.md#完整文件目录的自动转换)）；遇到忙碌答复（正在运行的周期、切换、
   登录续期）时每秒重试一次，最多 5 分钟。完整文件目录不受影响。使用 `-remove-user-data` 时，SyncD 随后还通过
   `accounts.remove` 删除每个账号：删除其保存的凭据并在服务器上吊销其应用密码。未确认的吊销会被报告，但不阻止卸载。
3. 关闭 SyncD，并检查 Windows 中不再有该 profile 账号的 `Nextcloud-MyGo!<SID>!` 同步根。其他 profile 的同步根保留。
   使用 `-remove-user-data` 时，随后删除该 profile 的 SyncD 数据与界面设置。
4. 删除遗留的、标记恰好为 `ApplicationName=Nextcloud-MyGo` 的 HKCU 导航窗格条目（绝不删除官方客户端的 `Nextcloud`）；
   另一个 profile 的现存同步根以 `NamespaceCLSID` 指向的条目（0.1.2 起的共用条目），或目标就是这种根的条目（0.1.1），会保留。
5. 删除此安装的开机启动项、快捷方式、应用列表条目与安装目录。账号、设置与同步文件另行保留。正在运行的
   `uninstall.exe` 由一个辅助进程删除，它每秒重试一次、最多 10 分钟，因为窗口会停留在结果界面直到被关闭（`removeLater`）。

若第 2 步或第 3 步失败，则不删除任何内容，界面会重新启动以便在其中移除该目录，setup 以退出码 1 结束并指出该目录的本地路径。
没有已安装的 SyncD，或 SyncD 无法启动时，只有当 Windows 中没有此用户的 Nextcloud-MyGo 同步根时卸载才会继续；否则它会停止并列出这些同步根。
SyncD 无法删除的账号会使用户数据保持原样，并在程序删除之后被报告。卸载除按上述方式释放按需目录之外，不触碰同步文件夹或服务器内容。
目录按路径自下而上删除，不使用 Go 的 `os.RemoveAll`（[上游](upstream.md#go)）。`tools/package-smoke.ps1` 断言卸载后没有新的同步根或导航窗格条目。

已知限制：

- 只处理正在运行的（默认）profile；其他 profile 的注册保留。
- 没有任何目录支撑的注册（例如控制数据库丢失之后）会阻止卸载，目前界面中没有办法移除它。
- 降级：0.1.0 拒绝 Control schema 12 的数据库，安装器和 SyncD 都是如此。受支持的回退方式是先在 0.1.1 中移除这些目录，再重新安装 0.1.0。

安装器与卸载程序作用于默认 profile；`NEXTCLOUD_MYGO_PROFILE` 选择开发 profile，与界面相同。

## 与官方客户端共存

安装目录、应用列表条目（键 `Nextcloud-MyGo`）、快捷方式、配置目录与管道名称都属于 Nextcloud-MyGo 自身。
开始菜单快捷方式与应用列表条目显示名称 **Nextcloud MyGo**；目录、注册表键与可执行文件保留 `Nextcloud-MyGo`。
安装器不注册 URL 协议（`nc://`）或文件关联，不读取也不修改官方客户端的任何内容。开机启动项按其启动的
可执行文件匹配，其他应用的启动项保持不变。

## 已验证

ARM64（ARM64 硬件上的 Windows 11），2026-11-12：

- 两个安装包均已构建（`tools/package.ps1`，15 秒与 16 秒）并检查：ARM64，28 个 PE 文件，原生 ARM64，
  Visual C++ 运行库为 ARM64X；x64，29 个 PE 文件，原生 x64；用另一架构检查任一包都会失败；
- 对 ARM64 安装器运行 `tools/package-smoke.ps1`（25 秒）：安装并启动；界面及其启动的 SyncD 从版本目录
  运行，machine 为 `0xAA64`（`GetProcessInformation`），SyncD 加载的每个模块都来自版本目录或 Windows，
  没有任何随包 DLL 取自已安装副本；在运行中的版本上重装切换到全新目录，通过 `daemon.shutdown` 停止
  SyncD 而无需终止，删除旧目录并迁移开机启动项；卸载后不留进程、开机启动项、快捷方式、应用列表条目或
  程序文件，并保留 profile 数据；再次安装后 `-remove-user-data` 删除了 profile 数据与设置；官方客户端的
  `nc` 注册、配置目录与应用列表条目均未改变；
- x64 安装器拒绝在 ARM64 上运行；
- 更新安全点与两个版本之间的升级（`tools/package-smoke.ps1 -UpgradeTo`，36 秒）：0.1.0 安装包（其 SyncD
  构建于本次修改之前）与以已提交源码加本次修改构建的 `-TestVersion 0.1.1` 安装包（SyncD 构建树已构建时
  16 秒；该构建树首次构建用时 56 秒）。同版本重装（3.7 秒）与在运行中副本上的升级（3.8 秒）都在
  `daemon.shutdown` 之前输出 `SyncD is ready`；升级写入了新的 `control.pre-update.*.sqlite`；新 SyncD
  报告版本 0.1.1，`startupReport` 的 `previousVersion` 为 0.1.0，schema 从 11 到 11，自检 `passed`；升级前
  设置的暂停所有目录得以保留；随后卸载不留任何内容。在改为先关闭界面之前，一次重装在 2 分钟后以无应答
  信息停止，删除了其新目录并保留已安装版本：SyncD 的管道一次只服务一个客户端。
- 签名更新路径（`tools/package-smoke.ps1 -UpdateDir`，共 41 s）：一次性 `mygo keygen` 密钥对，不带密钥构建的
  0.1.0 包（没有更新通道），以及带签名更新的 `-TestVersion 0.1.1` 包（21 s）。`TestInstalledUpdate` 通过环回
  HTTP 提供更新目录，读取清单，下载 37 MB 的归档，校验其签名与 `update.json`，并用 `release.StartSetup` 启动
  其安装器（5.0 s）：setup 关闭了运行中的 0.1.0 界面，输出 `SyncD is ready`，切换到 0.1.1 并删除 0.1.0；
  SyncD 0.1.1 报告上一版本 0.1.0，schema 从 11 到 11，自检 `passed`，所有目录仍处于暂停；两次卸载都不留任何
  内容，一次性密钥已删除。单元测试（`internal/release`）拒绝错误的密钥、改过的归档、改过签名、版本或大小的
  清单、作为 ARM64 提供的 x64 更新，以及不更新的版本。

未验证：安装 x64 包（需要 x64 硬件）；SyncD 首次网络请求时加载哪个 TLS 后端（本次没有服务器）；对已登录
账号执行 `-remove-user-data`；真实硬件上的安装失败（仅单元测试覆盖）；真实安装中安全点的删除等待与仍忙
拒绝（仅单元测试 `TestPrepare`）；带已登录账号或 Control schema 变更的升级；由已安装的界面本身发起更新（测试
代替了它的角色，因此界面的 `PrepareUpdate` 与它自己的交接只有单元级覆盖）；用项目密钥签名并从 GitHub 读取的
release；真实安装中应用内路径的下载中断或 setup 失败。对于 0.1.1，释放真实按需目录的卸载、从已安装的 0.1.0 升级，以及资源管理器中的包图标同样未经验证（见[平台说明](platform-notes.md)）。

## 需要 SyncD 提供的内容

SyncD 现已提供全部四项。安装器使用 `daemon.prepareUpdate` 与 `ConnectRemote`（第 3 步），`tools/package.ps1`
按清单检查 `--version`：

- `daemon.prepareUpdate(targetVersion)`（[IPC](ipc.md)）：PLAN.md §16.2 的安全点（不再开始新周期、运行中的
  周期已结束、一致的 Control 副本 `control.pre-update.<id>.sqlite`），在第 3 步中先于 `daemon.shutdown` 发送；
  新 SyncD 启动后，`snapshot.daemon.startupReport` 给出之前的版本与 schema、新 schema 以及自检结果和原因代码
  （`go run ./tools/pkg report` 输出它）。
- `internal/syncd.ConnectRemote(ctx, profile, executable)`：从不启动 SyncD 的客户端。
- `nextcloud-mygo-syncd.exe --version` 不使用 profile，输出一行 JSON：`{"version", "protocol": {"major",
  "minor"}, "controlSchema", "engine"}`。清单的 schema 仍从源码读取；两者不一致时打包失败。
- 可执行文件旁有 `qt.conf`（安装布局）时，SyncD 在加载任何东西之前调用
  `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)` 与
  `SetDllDirectory("")`，并移除 `QT_PLUGIN_PATH`，使 DLL 与 Qt 插件只来自版本目录和 Windows；开发构建没有
  `qt.conf`，保留其 PATH。尚未在已安装的包中验证。
