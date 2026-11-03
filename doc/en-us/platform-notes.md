# Platform notes

| Platform | Status |
|---|---|
| Windows 11 ARM64 | Scaffold only: the Go gate passes (below). No UI, no SyncD yet. |
| Windows 11 x64 | Scaffold only; nothing built or run on x64 yet. |
| Linux, macOS | Not supported. |

## Windows

Verified on Windows 11 ARM64 (2026-11-03; Go 1.27.3):

| Check | Result |
|---|---|
| Go module | `go vet ./...` and `go test ./...` pass |
| Documentation mirror | `go run ./tools/checkdocs` passes |

## Known limitations

- No native ARM64 claim can be made before M0 runs the full engine path on ARM64 hardware.
