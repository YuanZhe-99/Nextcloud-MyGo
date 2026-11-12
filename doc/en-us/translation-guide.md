# Translation guide

## 1. Scope

`doc/en-us/` is authoritative. `doc/zh-cn/` mirrors it; interface strings live in the ARB
catalogs. `PLAN.md` is written in Chinese and is not mirrored.

## 2. Structural parity

Both trees contain the same files with the same number of headings, table rows and code blocks;
code blocks are byte-identical. `go run ./tools/checkdocs` enforces this.

## 3. Never translated

Code, commands, paths, config keys, identifiers, error codes, invariant IDs (`INV-01`, …), version
numbers, product and company names (Nextcloud, Nextcloud-MyGo, MyGo, Qt, Microsoft) and protocol
and API names (WebDAV, JSON-RPC, Login Flow v2, CFAPI).

## 4. Style

Plain, short sentences. Chinese text uses full-width punctuation, with a space between Chinese
and Latin words only where it helps readability.

## 5. Glossary

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
| test build | 测试构建 |
| version directory | 版本目录 |
| Apps entry | 应用列表条目 |
| on-demand files | 按需文件 |
| full files (folder mode) | 完整文件 |
| server storage full (quota) | 服务器存储空间已满 |
| start report | 启动报告 |
| placeholder | 占位文件 |
| hydration | 按需下载（hydration） |
| free up space (dehydration) | 释放空间（dehydration） |
| always keep on this device | 始终保留在此设备上 |
| directory population | 目录填充（population） |
| Cloud Files provider | 提供程序（provider） |

## 6. Review checklist

- Same files, headings, tables and code blocks in both trees.
- Glossary terms used consistently.
- Links point to the same documents in each tree.
