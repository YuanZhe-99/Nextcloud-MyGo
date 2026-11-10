# URL 使用面审计

## 范围

M0/B01 对 Desktop v35.0.2、提交
`07761410a90a2f27007932fcb61afc23ddda5998` 的源码审计。
源码位置相对于 `third_party/nextcloud`。下面的原始清单是历史记录；当前有界运行时证据
见[平台说明](platform-notes.md)，剩余切换阻塞项见下文。

## 请求使用面

| 使用面 | 源码观察 | 启用前必须提供的证明 |
|---|---|---|
| 凭据注入 | `src/libsync/creds/httpcredentials.cpp:51–74` 在 QNAM 请求重载中注入凭据 | 注入前检查获认可的精确 HTTPS origin 与安装路径；不得继承 userinfo 凭据（INV-03） |
| 重定向 | `src/libsync/abstractnetworkjob.cpp:240–304` 解析相对地址、拒绝降级，在主机或端口变化时移除 Authorization 并设置 DontAddCredentials | 最多三跳、规范端口比较、不转发未获认可的请求体或 token、不跨 origin 重放 DAV 写入（N05） |
| 请求策略与日志 | `src/libsync/accessmanager.cpp:103–128` 记录 URL 和响应头，仅在未指定重定向策略时默认手动 | 服务强制策略、脱敏及日志和 IPC 零秘密扫描（R22） |
| MOVE | `src/libsync/propagateremotemove.cpp` 根据运行时 DAV 路径及重命名目标构造 Destination | 子路径和 Unicode 编码、源和目标属于同一运行时、禁止越出前缀（N06） |
| 分块上传 | `src/libsync/propagateuploadng.cpp:257–280` 保存传输元数据，再发送带 Destination 的 MKCOL | 相同命名空间、上传集合与最终 MOVE 经取消和重新发现后可恢复且不盲目重放（S06–S08） |
| 下载直链 | `src/libsync/propagatedownload.cpp:738–754` 分支使用服务器提供的 URL 和可选直链 Cookie | 首版禁用跨 origin 优化并验证无凭据泄漏（INV-03） |
| 恢复 | `src/common/syncjournaldb.h:104–134` 提供 DownloadInfo、UploadInfo 和 PollInfo；PollInfo 包含 URL | 审计所有恢复消费者、拒绝不支持的端点绑定恢复、从不改写 journal URL（INV-08、INV-09） |
| DAV multistatus | `src/libsync/networkjobs.cpp` 解析包含下载直链信息的服务器属性 | 在真实服务器验证相对或绝对 href、逐属性失败与允许前缀（S14、N06） |
| 登录 / OCS / capabilities | 产品 Login Flow 和运行时映射尚未实现 | 规范地址与运行时职责、获授权的轮询 origin 及稳定 DAV UID（B04/B09） |
| Push / WebSocket | 产品运行时生命周期尚未实现 | 在证明请求所有权和 epoch 隔离前保持未激活（INV-06） |
| 用户链接 / 签名查询 | 产品浏览器与诊断路由尚未实现 | 不得从服务器返回的链接推导凭据授权；不改写或记录签名查询 |

## 接入阻塞项

- 上游凭据注入是接入点，不是产品的 origin 授权策略。
- 在重定向时移除 Authorization，不能证明转发请求体是安全的。
- UploadInfo 不含显式 URL，不能证明所有恢复状态都独立于端点：PollInfo 显式包含 URL，
  其消费者仍需检查。
- 完整 URL、凭据、静止及 journal 恢复验收仍未完成；有界真实登录、删除保护及固定 journal
  同步检查已记录在平台说明中。

## 当前切换阻塞项

2026-11-09 对 Desktop v35.0.3 的可移植恢复源码审计：`propagateuploadv1.cpp:51–68`
以零传输 ID/分块及内容校验和持久化单 PUT 结果不明状态；`discovery.cpp:1952–2003`
在远程发现后比较该校验和。`propagatedownload.cpp:662–742` 使用相对本地临时路径和
ETag；`GETFileJob` 在接受续传字节前检查回复 ETag、内容长度和范围。只读准入门槛现允许
这两类有界状态，并要求重新验证目标能力/根和强制上游重新发现。真实跨 origin 中断验收
待完成。`propagateuploadng.cpp:34–38` 从当前账号派生相对分块 URL，但这本身不足以证明
会话可移植：分块上传和全部待处理轮询行仍被阻塞。

同日后续：`propagateuploadng.cpp:90–123` 仅在大小和 mtime 仍匹配时复用已保存的会话，随后
列出会话集合（`141–193`）并从最后一个连续分块之后继续；集合不存在时开始新的上传
（`195–209`），发现阶段按校验和接纳已落盘的最终 MOVE。补丁 0016 证明了换地址后的这两种
情形，闸门现准入带校验和的分块 v2 行。轮询行和旧版 v1 分块索引仍被阻塞。

2026-11-07 对 Desktop v35.0.3 的源码复查；后续修正进行中：

- `src/common/syncjournaldb.cpp:277–288` 不返回提交结果；SQLite 提交失败仅记日志，
  并保留事务标记。706–716 行的 `close()` 调用该方法后关闭数据库，不报告提交是否成功。
  连接已关闭或引擎已完成都不足以证明 `SwitchCoordinator::Proof::journalFlushed`。
  补丁 0011 现已暴露粘滞提交失败，适配器在关闭后检查它。新版本上游验收在平台说明记录前
  仍待完成。
- `src/libsync/account.cpp:309` 通过以 `deleteLater` 销毁的共享指针拥有 QNAM。
  仅释放账号不能证明回复和旧网络管理器已销毁。适配器现已观察活动/保留回复、结果不明写入
  及管理器实际销毁。`ClientSideEncryption::_account` 还持有账号强反向引用；退役在引擎销毁后
  清除此未启用 E2EE 的引用。切换前必须通过真实生命周期验收。
- `src/libsync/propagateupload.cpp:651–668` 在启动 job 前将轮询 URL 保存到 `PollInfo`；
  `src/libsync/syncengine.cpp:493–505` 在正常发现前消费待处理轮询。因此不能假定后续发现
  会在首次使用前处理端点绑定轮询 URL。不支持的跨端点恢复必须阻止激活，不得改写或删除 journal。
- 独立切换守卫要求提交前恢复结果不明的写入，而 PLAN §9.3 S5 在发布新 epoch 且仍控制准入时
  执行重新发现。接通运行时必须区分资源已排空与核对已完成，不能在 `finished` 时断言两者均成立。
