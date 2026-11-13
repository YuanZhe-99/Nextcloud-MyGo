# 持续集成与发布

公共仓库中的 GitHub Actions 对每次推送和拉取请求在 Windows x64 与 ARM64 上原生构建并测试
Nextcloud-MyGo，并把 `v*` 标签连同两个安装包发布为 release。除 `GITHUB_TOKEN` 外，只有发布使用一个 secret，
即为应用内更新签名的 `MYGO_UPDATER_PRIVATE_KEY`；没有任何内容带 Authenticode 签名。

| 工作流 | 触发 | 内容 |
|---|---|---|
| `.github/workflows/ci.yml` | 每次推送到分支、每个拉取请求、手动触发 | 调用 `build.yml` |
| `.github/workflows/build.yml` | 由另外两个工作流调用 | 在 `windows-latest`（x64）和 `windows-11-arm`（ARM64）上运行 Go 检查、依赖引导、上游引擎与 SyncD 构建以及快速层；带 `package` 时还生成安装包和许可证包 |
| `.github/workflows/release.yml` | 推送 `v*` 标签 | 检查标签，带 `package` 调用 `build.yml`，发布 GitHub release |

## 运行器

`windows-latest`（Windows Server 2025，x64）和 `windows-11-arm`（Windows 11，ARM64）对公共仓库免费，两者都带有
含 C++ CMake 工具的 Visual Studio 2026，这是构建脚本所需的（`Visual Studio 18 2026` 生成器）；镜像中没有它时
第一步会明确失败，并打印找到的 Visual Studio 与 MSVC 版本。每个架构都在自己的硬件上构建和运行：没有作业交叉编译
或仿真另一架构的产品，因此 CI 也是原生 x64 的证据，而这台 ARM64 开发机无法提供（[平台说明](platform-notes.md)）。
在 ARM64 上，x64 宿主 Qt 的工具（moc、rcc、lrelease）在运行器的 x64 仿真下运行，与本地构建完全相同；它们的产物不会进入安装包。
“Cloud Files diagnostics”步骤打印镜像名称和 Cloud Files 微筛选器的状态（`sc query/qc cldflt`、`fltmc`）。Cloud Files
无法作答时，`SyncDOnDemandContracts` 把其注册用例报告为已跳过（[按需文件](on-demand.md)）。

## `build.yml` 的步骤

1. Go：`gofmt`、`go vet`、分别以默认 cgo 设置和 `CGO_ENABLED=0` 运行 `go test ./...`、
   `go tool mygo vet .`、`go tool mygo i18n check`、`go run ./tools/checkdocs`（AGENTS.md）。
2. `tools/bootstrap-deps.ps1`（见下文），输出到 `.dev/deps`。
3. `tools/build-upstream.ps1 -Mode engine`：由子模块加上记录的补丁构建 SyncD 所链接的 `libsync`、`csync` 和 CFAPI 插件。
   上游自身的测试套件（`-Mode tests`，长时间构建后约八分钟的测试，其 GUI 资源需要 Inkscape）仍是本地层。
4. `tools/build-syncd.ps1`，它也运行 SyncD 的 CTest 目标。
5. `tools/test-all.ps1 -Tier fast`（ARM64 上的 gofmt 与 vet 步骤、真实 Go 到 C++ 管道测试、
   全部 CTest 目标），预算两分钟。
6. 带 `package` 时：`tools/package.ps1`、对暂存目录运行 `go run ./tools/pkg check`、
   并上传安装包以及 `tools/package.ps1` 暂存进其中的许可证包。

**服务器层不在 CI 中运行**：它需要 `tools/test-server.ps1` 的一次性 Linux Nextcloud 容器，而 GitHub 的 Windows
运行器不能运行 Linux 容器。它仍是本地层（[开发](development.md)），在发布前于两个架构上运行并记录在
[验收](acceptance.md)中；上游测试、安装冒烟测试（`tools/package-smoke.ps1` 会安装到用户的 profile 中）和共享服务器夹具同样如此。

## 依赖引导

`tools/bootstrap-deps.ps1 -Architecture ARM64|x64 [-Prefix <dir>]` 按 `build-lock.json` 在 `preparedDependencies`
下的记录精确组装所有原生依赖，放到 `<Prefix>`（默认 `.dev/deps`），并写出 `<Prefix>/env-<arch>.json`，其中包含
`tools/build-upstream.ps1` 与 `tools/build-syncd.ps1` 的参数（`QtPrefix`、`QtHostPrefix`、`DependencyPrefix`）以及
`PkgConfig`，即上游 CMake 用来查找 libp11 的 pkg-config（设为 `PKG_CONFIG`）。它不在 `<Prefix>` 之外安装任何东西。

| 组件 | 来源 | 方式 |
|---|---|---|
| Qt 6.12.0 | `download.qt.io` 在线仓库，`msvc2022_64` 与 `msvc2022_arm64_cross_compiled` | 基础包以及 Qt 5 Compat、WebSockets 和 Shader Tools，不含文档和翻译；校验 SHA-1；不加修改地解压到 `Qt/6.12.0/msvc2022_64`（同时作为宿主 Qt）和 `msvc2022_arm64` |
| OpenSSL、SQLite（`json1`）、zlib | 记录的提交与工具版本的 vcpkg，经典模式，triplet `<arch>-windows` | 安装的版本与端口版本必须等于记录 |
| pkg-config | vcpkg 记录的提交为宿主（`clangarm64` 或 `mingw64`）固定的 msys2 pkgconf 包 | 从 vcpkg 自己的脚本读取 URL 和 SHA-512，下载、校验并解包到 `tools/pkgconf-<host>`（vcpkg 只在构建端口时自行获取它，由二进制缓存提供端口时不会） |
| ECM、QtKeychain、KDSingleApplication、KArchive | 各项目的发布标签，浅克隆 | 标签必须解析到记录的提交；使用原始构建的 CMake 选项，Release，安装到 `native/<arch>` |
| libp11 | 发布标签 `libp11-0.4.21`，同样检查提交 | 用其 nmake makefile 只构建 `libp11.dll`，链接 vcpkg 的 OpenSSL；头文件和 `libp11.pc` 复制到 `native/<arch>` |
| 许可证文本 | 每个随包发布的 Qt 模块在标签 `v6.12.0` 下的 `LICENSES/`、vcpkg 的 `copyright` 文件、各源码的许可证文件 | 复制到 `native/<arch>/share/licenses`，供 `tools/licenses.ps1` 使用 |

不一致即失败，绝不退回到其他版本。已安装前缀中的每个 DLL 和 EXE 都必须是目标架构的 PE machine。只要标记文件
（`native/<arch>/bootstrap.json`：记录和脚本的哈希）仍然一致，已完成的架构就会被跳过；只有归档列表变化时才重新解压 Qt。
一个前缀容纳两个架构；请依次运行，因为它们共用 vcpkg 检出。

### 与手工组装的 `.dev` 的差异

- QtKeychain、KDSingleApplication 和 KArchive 过去用同一个源码检出为两个架构构建；引导脚本为每个架构各克隆一份。
  提交和选项相同。
- ECM 过去只安装一次并复制到 x64 前缀；引导脚本为每个架构分别安装。
- Qt 下载与原来的 `get-qt.ps1` 步骤相同（aqtinstall 3.3.0 无法读取新的仓库布局）。
- pkg-config 过去是 vcpkg 解包在其 `downloads/tools/msys2` 下的副本；引导脚本自行解包同一个固定的包（pkgconf 3.0.7）。
- `native/<arch>/share/licenses` 和 `bootstrap.json` 是新增的。

## 缓存

每个架构三个缓存，各自在其步骤成功后立即保存，因此后续失败不会丢弃已完成的构建：

| 缓存 | 内容 | 键 |
|---|---|---|
| `qt-…` | Qt 归档（`.dev/deps/downloads`），x64 约 0.25 GB，ARM64 约 0.5 GB（目标与宿主） | Qt 版本、引导脚本 |
| `deps-…` | `native/<arch>`、vcpkg 的安装树和 `tools/`（pkgconf） | 引导脚本、MSVC 版本、`preparedDependencies` |
| `upstream-…` | 打过补丁的源码 `build/src-*` 和引擎构建 `build/?e-*`，不含目标文件和 PDB | `deps` 键、子模块提交、记录的上游提交、补丁哈希、`tools/build-upstream.ps1` |

因此修改补丁、子模块、`build-lock.json` 中的依赖、任一脚本或运行器的 MSVC 版本，只会重建依赖于它的部分。
Qt 每次运行都从缓存的归档解压（解压后的树各 2.5 GB）。标签运行可以使用默认分支的缓存。

## 耗时

| 运行 | x64（`windows-latest`） | ARM64（`windows-11-arm`） |
|---|---|---|
| 冷启动（无缓存） | 预计约 50 分钟 | 预计约 50 分钟 |
| 热启动（缓存全部命中，SyncD 有改动） | 预计约 12 分钟 | 预计约 12 分钟 |

这些是预期值而非测量值：由下方本地时间按运行器的四核换算。首次 GitHub 运行后以实测时间替换。冷启动的大部分时间是
OpenSSL 的 vcpkg 构建和上游引擎；热启动则是 Go 检查、解压 Qt、SyncD 构建和快速层。

2026-11-12 在 ARM64 开发机（8 核，同时有其他构建在运行）上测得，依赖放在全新前缀 `.dev/ci-verify`，构建根目录为
`build/cv`，SyncD 和测试来自已提交源码的 `git archive` 加上这些脚本：

| 步骤 | ARM64（原生） | x64（在 ARM64 上构建，测试经仿真运行） |
|---|---|---|
| Qt 下载与解压（ARM64 需要两个 SDK） | 91 s | 57 s（仅宿主 SDK） |
| vcpkg 端口 | 15 s（由 vcpkg 二进制缓存提供，ABI 输入相同） | 875 s（冷构建，`VCPKG_BINARY_SOURCES=clear`） |
| ECM、QtKeychain、KDSingleApplication、KArchive、libp11、pkgconf、许可证文本 | 约 160 s | 约 220 s |
| 记录未变时再次运行引导 | 1.0 s（Qt 已解压；CI 先从缓存的归档解压） | 1.0 s |
| 上游引擎（`-Mode engine`） | 904 s | 843 s |
| SyncD 构建及其 CTest（22 个全部通过） | 155 s | 156 s |
| 快速层 | 42.1 s，预算 120 s（最终引导后再次运行为 36.8 s） | 38.8 s，预算 120 s（36.7 s） |
| `tools/package.ps1` 与 `tools/pkg check` | 11 s，29 个 PE 文件均为原生 ARM64（Visual C++ 运行时为 ARM64X） | 13 s，30 个 PE 文件均为原生 x64 |

与手工组装的 `.dev` 对比：16 个 Qt 归档逐字节相同；每个架构的原生前缀都有相同的 247 个文件，其中 241 个逐字节相同，
四个 DLL 只在 4 到 14 个字节上不同（PE 时间戳和调试标识），`KF6Archive.pc` 和 `ECMConfig.cmake` 只在前缀路径上不同；
vcpkg 全部六次端口构建的 ABI 输入相同，ARM64 端口 DLL 逐字节相同。当时工作树中未提交的后端改动不属于这次运行。
本地没有运行的是工作流本身在 GitHub 运行器上的执行。

## 发布

推送标签 `vX.Y.Z`（在提交之后，AGENTS.md）会运行 `release.yml`：

1. 标签必须等于 `internal/version/version.go` 中的 `Version` 和 `mygo.json` 中的 `version`。
2. 在两个架构上带 `package` 运行 `build.yml`：上述全部步骤，然后是安装包、暂存安装包中每个 PE 文件的架构检查
   （ARM64 Visual C++ 运行时是已接受的 ARM64X 例外，[打包](packaging.md)），以及许可证包。
   有 secret `MYGO_UPDATER_PRIVATE_KEY`（只传给 Package 步骤）时，每个包还会得到其签名更新
   `Nextcloud-MyGo-X.Y.Z-<arch>-update.zip` 与 `update-windows-<goarch>.json`（[打包](packaging.md#签名更新)），
   一并上传。
3. 一个 GitHub release，附带 `Nextcloud-MyGo-X.Y.Z-arm64-test-setup.exe`、
   `Nextcloud-MyGo-X.Y.Z-x64-test-setup.exe`、`Nextcloud-MyGo-X.Y.Z-licenses.zip`、存在时两个架构的更新文件
   （只有一个架构则作业失败），以及覆盖所有这些文件的 `SHA256SUMS`。有更新时它标记为**最新**（已安装的副本从这个
   release 读取清单），标题注明安装包未做代码签名的测试构建；没有更新时它是**预发布**，标题注明未签名测试构建，
   已安装的副本无法更新到它。说明中写明安装包没有 Authenticode 签名，因此首次安装时 SmartScreen 会警告，写明是否
   提供应用内更新，以及服务器层在 CI 之外运行。

安装包从不带 Authenticode 签名：没有 Windows 代码签名证书。更新密钥只保存在 GitHub secret 和维护者的离线副本中；
`tools/pkg sign-update` 从环境变量读取它且从不输出，`mygo build` 从不获得它。

### 许可证包

`tools/licenses.ps1 -SyncDBuild <tree> -Arch <arch> -Out <dir>` 根据该构建实际链接的内容写出：

- `LICENSE.txt`（GPL-3.0，本产品）和 `THIRD_PARTY_NOTICES.md`；
- `SOURCE.txt`：仓库 URL、标签与提交，子模块的 Nextcloud Desktop 标签与提交，`patches/nextcloud/` 中的补丁按
  `build-lock.json` 的顺序应用，以及每个原生依赖的版本与提交；
- `licenses/Nextcloud-Desktop/`：`COPYING` 和快照中按文件划分的 `LICENSES/`；
- `licenses/Qt/<module>/`、`OpenSSL`、`SQLite`、`zlib`、`QtKeychain`、`KArchive`、`libp11`：来自依赖前缀
  （前缀不是由引导脚本生成时失败）；
- `licenses/Go/`：Go 工具链的许可证，以及链接进界面和安装器的每个模块的许可证文件（模块没有许可证文件时失败）。

`tools/package.ps1` 把这个许可证包暂存进每个安装包，`pkg stage` 拒绝不完整的许可证包（[打包](packaging.md)）。
发布的 zip 为每个架构各包含一个这样的目录，与安装包中的文件相同。
