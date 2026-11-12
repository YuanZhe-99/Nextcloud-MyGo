# 打包与安装

Nextcloud-MyGo 每个架构提供一个当前用户安装包。每个安装包包含界面、与之一起验证的 SyncD，以及它们
实际加载的 DLL，仅此而已。在可信签名链建立之前，每个包都是未签名的**测试构建**，自动更新关闭；安装器、
应用列表条目和快捷方式都会注明。

## 工具

安装器是一个小型 Go 程序 `packaging/windows/setup`，按安装包的架构交叉编译。没有采用 MyGo 自带的 NSIS
安装器：NSIS 会把 x86 的 `Uninstall.exe` 写入安装目录，而 ARM64 包不得包含它。用 WiX 构建 MSI 需要
.NET SDK 并接受 WiX 的许可条款，其自定义操作仍需原生代码来停止 SyncD。Go 程序只需要 Go 工具链，在
ARM64 与 x64 上原生运行，通过 `internal/syncd` 与 SyncD 通信，并与包内其他文件接受同一个架构检查器检查。

| 组成 | 路径 | 作用 |
|---|---|---|
| 打包脚本 | `tools/package.ps1` | 构建界面，暂存并检查安装包，构建安装器 |
| 检查与暂存工具 | `tools/pkg` | DLL 闭包、清单、PE 架构检查 |
| 安装/卸载程序 | `packaging/windows/setup` | 当前用户安装、升级与卸载 |
| 清单 | `internal/release` | 包内文件及其两半所用的契约 |
| 安装测试 | `tools/package-smoke.ps1` | 在本机真实安装、重装和卸载 |

## 构建安装包

```powershell
./tools/build-syncd.ps1 -Architecture ARM64 -QtPrefix <target Qt> -QtHostPrefix <host Qt>
./tools/package.ps1 -Arch ARM64        # or x64; SyncD from build/syncd-<Arch>
go run ./tools/pkg check -arch ARM64 build/package/ARM64/Nextcloud-MyGo-0.1.0-ARM64
./tools/package-smoke.ps1 -Setup build/package/Nextcloud-MyGo-0.1.0-arm64-test-setup.exe
```

`tools/package.ps1` 从 SyncD 构建的 `CMakeCache.txt` 读取其上游构建，再读取该构建的前缀（Qt、原生
依赖、vcpkg），因此 DLL 只来自 SyncD 实际链接的构建树，外加已安装 Visual Studio 中最新的 Visual C++
运行库。每一步都有时限。它写出 `build/package/<Arch>/Nextcloud-MyGo-<version>-<Arch>/` 与
`build/package/Nextcloud-MyGo-<version>-<arch>-test-setup.exe`，不安装任何东西。

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
    qt.conf, sync-exclude.lst, manifest.json, LICENSE.txt, licenses\
```

用户数据留在各进程自己的位置，不在安装目录内：SyncD 的 profile 位于
`%LOCALAPPDATA%\Nextcloud-MyGo\syncd\<profile hash>`，界面设置位于 `%APPDATA%\Nextcloud-MyGo`。

## 清单

`manifest.json` 记录产品、版本与架构；`testBuild: true`、`signed: false` 与 `autoUpdate: false`；
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

Microsoft 只以 ARM64X 形式提供 ARM64 Visual C++ 运行库。ARM64 进程映射其 ARM64 代码，从不映射其 ARM64EC
代码，因此它原生运行；检查器把每个此类文件作为提示列出。其他所有混合映像都失败。用户已于 2026-11-12 接受此例外。检查器的测试构造合成
PE 文件，因此 CI 可在任何平台运行。

## 安装与升级

以下情况下，安装器在写入任何内容前拒绝：

- Windows 运行的架构与安装包不同（x64 包在 ARM64 上会以仿真运行）；
- 已安装版本更新，或其 Control schema 更新（PLAN.md §16.3）；
- 已安装的是另一架构的包：先卸载；文件与设置保留。

否则按以下顺序安装、重装或升级：

1. 解压到新的 `versions\<version>-<random>.partial`，按清单校验后再重命名。失败只删除这个 partial 目录。
2. 关闭界面（无状态客户端），使其无法再次启动 SyncD；再向 SyncD 发送 `daemon.shutdown` 并最多等待 30
   秒；未停止的 SyncD 被终止，它会像处理任何崩溃一样恢复。若仍有进程存活，删除新目录，已安装版本保持
   为当前版本。
3. 把 `uninstall.exe` 复制到根目录，让 `install.json`、开始菜单快捷方式与应用列表条目指向新目录，并把
   启动此安装的开机启动项改为启动新界面，保留其参数以及用户在任务管理器中的启用/禁用选择。
4. 删除其他版本目录，然后启动新界面，由它启动自己的 SyncD。

新的 SyncD 从不与其他版本的 Qt DLL 共用目录，旧版本只在新版本完整且已成为当前版本后才删除。同样的规则
使同版本重装也完整切换到一个全新目录。

## 卸载

`uninstall.exe -uninstall`（应用列表条目的命令）关闭界面，按上文停止 SyncD，删除此安装的开机启动项、
快捷方式、应用列表条目与安装目录，保留账号、设置与同步文件。`-remove-user-data` 另外启动 SyncD，通过
`accounts.remove` 删除每个账号：删除其保存的凭据并在服务器上吊销其应用密码；然后删除该 profile 的 SyncD
数据与界面设置。未确认的吊销会被报告，但不阻止卸载；SyncD 无法删除的账号会使用户数据保持原样并被报告。
两种方式都不触碰同步文件夹或服务器内容。
按需目录的 Cloud Files 注册在卸载后保留，重新安装后会再次为其提供服务。`-remove-user-data` 无法删除仍有目录的账号，因此会保留其数据，
绝不会留下无主的注册；卸载时先移除这些目录是一个未完成项（[按需文件](on-demand.md)）。

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
- x64 安装器拒绝在 ARM64 上运行。

未验证：安装 x64 包（需要 x64 硬件）；SyncD 首次网络请求时加载哪个 TLS 后端（本次没有服务器）；对已登录
账号执行 `-remove-user-data`；真实硬件上的安装失败（仅单元测试覆盖）；两个不同版本之间的升级。

## 需要 SyncD 提供的内容

SyncD 现已提供前四项；安装器与暂存工具尚未采用，许可证包仍未完成：

- `daemon.prepareUpdate(targetVersion)`（[IPC](ipc.md)）：PLAN.md §16.2 的安全点（不再开始新周期、运行中的
  周期已结束、一致的 Control 副本 `control.pre-update.<id>.sqlite`），用于替换第 2 步中的 `daemon.shutdown`，
  之后再发送 `daemon.shutdown`；新 SyncD 启动后，`snapshot.daemon.startupReport` 给出之前的版本与 schema、
  新 schema 以及自检结果和原因代码。
- `internal/syncd.ConnectRemote(ctx, profile, executable)`：从不启动 SyncD 的客户端。
- `nextcloud-mygo-syncd.exe --version` 不使用 profile，输出一行 JSON：`{"version", "protocol": {"major",
  "minor"}, "controlSchema", "engine"}`，暂存因此可以不再从源码读取 schema。启动时据此校验清单属于暂存工具。
- 可执行文件旁有 `qt.conf`（安装布局）时，SyncD 在加载任何东西之前调用
  `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)` 与
  `SetDllDirectory("")`，并移除 `QT_PLUGIN_PATH`，使 DLL 与 Qt 插件只来自版本目录和 Windows；开发构建没有
  `qt.conf`，保留其 PATH。尚未在已安装的包中验证。
- 许可证包：Qt、QtKeychain、OpenSSL、SQLite、zlib、KArchive 与 libp11 的许可证文本尚未与
  `LICENSE.txt` 及 Nextcloud Desktop 的 `COPYING` 一起提供。
