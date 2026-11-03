# Development

## Requirements

- Go 1.27.3 (`go.mod`; an older Go downloads it automatically). MyGo is a Go tool dependency
  (`go tool mygo …`).
- Windows: Visual Studio 2026 (or its Build Tools) with the C++ tools for ARM64 and x64 and the
  Windows SDK; CMake and Ninja. M0 pins the exact versions, together with Qt and the other native
  dependencies, in `build-lock.json`.

## Commands

```bash
gofmt -l cmd internal tools
go vet ./...
go test ./...
CGO_ENABLED=0 go test ./...
go tool mygo vet .
go tool mygo i18n check
go run ./tools/checkdocs           # doc/en-us and doc/zh-cn mirror each other
```

The SyncD build and test commands for x64 and ARM64 are added here with the first backend code.

## Testing principles

Tests protect product contracts and error-prone logic, preferably table-driven; every fixed bug
gets a regression test, and each test names the invariant or acceptance case it covers.
Scheduling, scoring, backoff and switching run on a controllable clock. Tests that need a real
Nextcloud server run only when their environment variables are set. An HTTP mock never replaces a
real server for DAV, locking or chunked uploads in acceptance.
