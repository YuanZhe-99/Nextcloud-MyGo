# 按需文件（Windows Cloud Files）

按需目录把每个远程文件都保留为 Cloud Files 占位文件：列表中显示其名称、大小和日期，只有应用打开它时才下载内容。
这是 M6 的 Windows 部分（`PLAN.md` §2.3）。目录在添加时选择完整文件模式或按需模式；已有目录不会切换模式，
因为上游的模式切换会删除并重建 journal。

## 复用的上游代码

SyncD 加载上游的 CFAPI 插件（`src/libsync/vfs/cfapi`、`nextcloudsync_vfs_cfapi.dll`），作为该目录的 VFS
交给引擎。发现、占位文件创建、按需下载、释放空间、固定状态和目录填充均由上游完成。补丁 0017 让插件适用于
自行管理账号的服务（[上游](upstream.md)）：

- `BUILD_CFAPI_VFS` 在不构建资源管理器 Shell 扩展（它依赖 GUI）的情况下构建插件。
- `VfsSetupParams::syncRootAccountId` 代替同步根 ID 中账号的显示名（`user@host`），注册因此不依赖服务器地址。
- 同步根不指向任何未注册的缩略图、自定义状态处理类或导航窗格类，因此绝不会让资源管理器调用其他客户端的
  Shell 扩展。
- `Vfs::refuseNewRequests` 与 `Vfs::hasRequestsInFlight` 让所有者先关闭同步根，并在断开连接前确认没有正在
  运行的按需下载或目录填充回调。

Shell 扩展（缩略图、锁定与共享叠加图标）不构建。资源管理器自身的 Cloud Files 集成提供状态列与图标、传输进度，
以及“始终保留在此设备上”和“释放空间”命令。

## 注册

`OnDemandRoot`（`backend/platform/windows/`）为一个绑定 epoch 注册并连接一个目录的同步根。注册项为
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager` 下的
`Nextcloud-MyGo!<用户 SID>!<账号 ID>!<目录 ID>`，由当前用户在不提权的情况下写入，提供程序名为
`Nextcloud-MyGo`，版本为产品版本。官方客户端注册为 `Nextcloud!…`，两者从不共用同一注册项。

只有既不是同步根、也不位于任何同步根之内的目录才能成为按需同步根。Cloud Files 报告属于其他提供程序、其他目录
或位于某个同步根之内的路径一律为 `FOLDER_OWNED`。只有按需目录的根租约才接受根目录自身的云重解析标记和目录树中的
Cloud Files 占位文件；其他任何重解析点，以及完整文件目录中的任何占位文件，仍会阻止该目录。目录树检查从不列举尚未
填充的目录，否则会请求 SyncD 自己的提供程序去填充它。

## 请求与切换边界

按需下载是当前端点 epoch 上的请求：它使用该 epoch 的账号及其绑定 origin 的网络管理器，因此凭据仍然只发往已认可的
origin。账号为切换或重新认证进入静止时，该账号的所有按需同步根都会关闭：此后到达的请求立即失败，已在运行的请求
像传输一样继续完成。只有既没有同步周期、也没有按需请求在运行时，账号才处于静止；随后退役会断开并销毁同步根，
从而释放旧账号。重新绑定会在同一注册项下为新账号连接新的同步根。若静止结束时没有重新绑定，则在下一个同步周期之前
重新连接同步根。同步根断开期间，应用打开仅联机文件时，Windows 报告提供程序不可用；绝不会显示为空文件。

用户会话生命周期与 SyncD 一致：按用户运行的进程，从不提权，也不是服务。SyncD 未运行时，仅联机文件无法打开，
已下载的文件仍可读取。下次启动时重新连接注册项。

## 按需下载期间崩溃

提供程序进程结束时，Cloud Files 让打开请求失败。占位文件保留已经到达的内容，仍为仅联机状态；其 journal 记录仍为
虚拟记录，因为上游只在按需下载完成时才记录。重启后再次打开时从头重新下载该文件。

## 移除

移除按需目录时，在其运行时结束后注销注册项。内容已在磁盘上的文件恢复为普通文件，因此以后仍可把该目录映射为完整
文件模式；仅联机占位文件会被删除，因为没有提供程序就无法打开它们，而服务器仍保存其内容。其他本地内容和 journal
保留不变。SyncD 从不调用上游的 `unregisterFolder`，它会在其提供程序的同步根全部移除后删除 Shell 扩展类。

## 验证

`syncd_ondemand_tests`（CTest `SyncDOnDemandContracts`）在临时 NTFS 目录上针对真实的 Cloud Files API 运行，
不连接服务器：注册标识、拒绝外部和嵌套同步根、切换所等待的请求闸门，以及完整注销。`tools/test-ondemand.ps1`
针对一次性本地服务器运行 `syncd_ondemand_acceptance`：占位文件、打开时按需下载、目录填充、释放空间、始终保留在
此设备上、等待正在运行的按需下载的切换、关闭期间的拒绝与下一个 epoch 的服务、按需下载期间终止进程后重启，以及注销。
`tools/test-cfapi-upstream.ps1` 运行上游的 `SyncCfApiTest`。结果见[平台说明](platform-notes.md)。
