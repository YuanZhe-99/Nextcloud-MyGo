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
| endpoint | 端点 |
| origin | origin |
| canonical URL | 规范地址 |
| endpoint epoch | 端点 epoch |
| switch | 切换 |
| quiescent boundary | 静止边界 |
| namespace verification | 命名空间核验 |
| acknowledge (an origin) | 认可（origin） |
| credential reference | 凭据引用 |
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

## 6. Review checklist

- Same files, headings, tables and code blocks in both trees.
- Glossary terms used consistently.
- Links point to the same documents in each tree.
