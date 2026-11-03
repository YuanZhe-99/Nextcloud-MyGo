# 平台说明

| 平台 | 状态 |
|---|---|
| Windows 11 ARM64 | 仅脚手架：Go 检查通过（见下）。尚无界面和 SyncD。 |
| Windows 11 x64 | 仅脚手架；尚未在 x64 上构建或运行任何东西。 |
| Linux、macOS | 不支持。 |

## Windows

在 Windows 11 ARM64 上验证（2026-11-03；Go 1.27.3）：

| 检查 | 结果 |
|---|---|
| Go 模块 | `go vet ./...` 与 `go test ./...` 通过 |
| 文档镜像 | `go run ./tools/checkdocs` 通过 |

## 已知限制

- 在 M0 于 ARM64 真机上跑通完整引擎路径之前，不能声明原生 ARM64 支持。
