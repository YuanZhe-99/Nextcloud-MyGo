# 第三方代码

Nextcloud-MyGo 以 GPL-3.0 授权。本清单列出它复用或内嵌的每个上游、用途及相应义务。许可声明本身见
`THIRD_PARTY_NOTICES.md`。

## 清单

| 组件 | 仓库与版本 | 用途 | 许可证 | 修改 | 义务 |
|---|---|---|---|---|---|
| MyGo | `egoist/mygo` v0.7.4 | 界面框架、打包 | MIT | 无 | 保留版权声明；其内嵌的 Kage 编译器与 MingCute 图标为 Apache-2.0 |
| Kyokai 与 Easytier-MyGo 的文件 | `YuanZhe-99/Kyokai`、`YuanZhe-99/Easytier-MyGo` | `tools/checkdocs`、`internal/version` 模式、MyGo 维护技能、工作规范 | AGPL-3.0 / GPL-3.0，由作者本人改以 GPL-3.0 授权 | 调整名称与注释 | 无，除 Nextcloud-MyGo 自身许可证外 |
| Nextcloud Desktop | `nextcloud/desktop`，在 M0 固定 | 链接进 SyncD 的同步引擎（`libsync`、`common`）；应用图标 | GPL-2.0-or-later 及其他文件级许可证 | 仅通过 `patches/nextcloud/` 中的补丁 | 保留文件头；随发布提供许可证文本、对应源代码与构建补丁 |

## 商标

Nextcloud 名称与图标是 Nextcloud GmbH 的商标。产品名称与图标复用遵循用户的决定；首次发布前核对商标条款。

## 计划中的组件

Qt、QtKeychain、TLS 后端与 SQLite 在 M0 于 `build-lock.json` 中固定后，连同确切版本与许可证加入本清单。
