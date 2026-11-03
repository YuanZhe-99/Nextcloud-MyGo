# Platform notes

| Platform | Status |
|---|---|
| Windows 11 ARM64 | The interface runs natively against sample data (below). No SyncD yet. |
| Windows 11 x64 | Nothing built or run on x64 yet. |
| Linux, macOS | Not supported. |

## Windows

Verified on Windows 11 ARM64 (2026-11-03; Go 1.27.3, MyGo v0.7.4):

| Check | Result |
|---|---|
| Go module | `go vet ./...`, `go test ./...`, `go tool mygo vet .` and `go tool mygo i18n check` pass |
| Documentation mirror | `go run ./tools/checkdocs` passes |
| Interface | `go build` makes a native ARM64 executable that starts with its window and tray against the Mock and runs without errors; every page renders in light and dark, English and Simplified Chinese (view tests and screenshots) |

## Known limitations

- No native ARM64 claim can be made before M0 runs the full engine path on ARM64 hardware.
- The interface talks to `syncd.Mock`, not SyncD: no file syncs, no login reaches a server.
- The tray glyph and the mark are placeholders until the Nextcloud icon comes from the upstream
  release.
- The tray flyout, notifications, open at sign-in and the update check were not exercised by
  hand; app updates are off in unsigned builds.
