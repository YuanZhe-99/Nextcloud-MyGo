# Development

## Requirements

- The latest Go release, named in `go.mod` (an older Go downloads it automatically). MyGo is a Go
  tool dependency (`go tool mygo …`) at its latest tag.
- Windows: Visual Studio 2026 (or its Build Tools) with the C++ tools for ARM64 and x64 and the
  Windows SDK; CMake and Ninja. M0 chooses their versions, together with Qt and the other native
  dependencies, and `build-lock.json` records what each build used.

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

## Running the interface

```bash
go run ./cmd/nextcloud-mygo                                  # sample data
NEXTCLOUD_MYGO_FRESH=1 go run ./cmd/nextcloud-mygo           # first run: setup
NEXTCLOUD_MYGO_PROFILE=dev go run ./cmd/nextcloud-mygo       # a separate instance
SCREENSHOT_DIR=/tmp/shots go test ./internal/gui             # every page as PNG
go tool mygo generate                                        # after editing locales/*.json
```

Until SyncD exists the interface runs against `syncd.Mock`: nothing touches files, the network
or the browser, and every page says it shows sample data.

## Testing principles

Tests protect product contracts and error-prone logic, preferably table-driven; every fixed bug
gets a regression test, and each test names the invariant or acceptance case it covers.
Scheduling, scoring, backoff and switching run on a controllable clock. Tests that need a real
Nextcloud server run only when their environment variables are set. An HTTP mock never replaces a
real server for DAV, locking or chunked uploads in acceptance.
