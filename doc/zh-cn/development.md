# 开发

## 环境要求

- `go.mod` 中写明的最新 Go 发行版（较旧的 Go 会自动下载它）。MyGo 是 Go 工具依赖（`go tool mygo …`），
  使用其最新标签。
- Windows：Visual Studio 2026（或其 Build Tools），含 ARM64 与 x64 的 C++ 工具及 Windows SDK；CMake 与
  Ninja。M0 选定它们以及 Qt 和其他原生依赖的版本，`build-lock.json` 记录每次构建实际使用的版本。

## 命令

```bash
gofmt -l cmd internal tools
go vet ./...
go test ./...
CGO_ENABLED=0 go test ./...
go tool mygo vet .
go tool mygo i18n check
go run ./tools/checkdocs           # doc/en-us and doc/zh-cn mirror each other
```

x64 与 ARM64 的 SyncD 构建和测试命令随第一批后端代码加入此处。

## 测试原则

测试保护产品契约和容易出错的逻辑，优先表驱动；每个修复的缺陷都有回归测试，且每个测试注明它覆盖的不变量或
验收用例。调度、评分、退避与切换使用可控时钟。需要真实 Nextcloud 服务器的测试只在设置了相应环境变量时运行。
验收中，HTTP mock 从不替代真实服务器的 DAV、文件锁与分块上传行为。
