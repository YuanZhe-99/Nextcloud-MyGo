//go:build !payload

package main

// payload is nil in the uninstaller build, which can only uninstall.
var payload []byte
