# 翻译指南

## 1. 范围

`doc/en-us/` 为权威版本。`doc/zh-cn/` 镜像它；界面字符串位于 ARB 目录中。`PLAN.md` 以中文撰写，不做镜像。

## 2. 结构一致

两棵目录树包含相同的文件，标题、表格行和代码块数量相同；代码块逐字节一致。由
`go run ./tools/checkdocs` 强制检查。

## 3. 不翻译的内容

代码、命令、路径、配置键、标识符、错误码、不变量编号（`INV-01` 等）、版本号、产品与公司名称（Nextcloud、
Nextcloud-MyGo、MyGo、Qt、Microsoft），以及协议与 API 名称（WebDAV、JSON-RPC、Login Flow v2、CFAPI）。

## 4. 风格

句子平实简短。中文使用全角标点，中英文之间仅在有助阅读时加空格。

## 5. 术语表

| English | 简体中文 |
|---|---|
| sync | 同步 |
| sync root | 同步根 |
| folder (sync mapping) | 目录（同步映射） |
| journal | journal（同步数据库） |
| SyncD | SyncD（同步服务进程） |
| Control DB | 控制数据库（Control DB） |
| crash ledger | 崩溃记录 |
| state availability | 状态可用性 |
| ancestor-presence event | 祖先存在事件 |
| endpoint | 端点 |
| origin | origin |
| canonical URL | 规范地址 |
| endpoint epoch | 端点 epoch |
| switch | 切换 |
| quiescent boundary | 静止边界 |
| namespace verification | 命名空间核验 |
| acknowledge (an origin) | 认可（origin） |
| credential reference | 凭据引用 |
| reauthentication | 重新认证 |
| credential rotation | 凭据轮换 |
| rollback | 回滚 |
| reserved vault slot | 预留凭据库槽位 |
| app password | 应用密码 |
| conflict | 冲突 |
| conflict copy | 冲突副本 |
| deletion protection | 删除保护 |
| proposal | 提案 |
| dirty generation | dirty generation |
| circuit breaker | 断路器 |
| debounce | 防抖 |
| blocking reason | 阻塞原因 |
| operation ID | operationId |
| snapshot | 快照 |
| rediscovery | 重新发现 |

## 6. 审阅清单

- 两棵树的文件、标题、表格和代码块一致。
- 术语使用一致。
- 链接在各自目录树中指向相同的文档。
