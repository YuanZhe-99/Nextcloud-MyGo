//go:build payload

package main

import _ "embed"

// payload is the package zip tools/package.ps1 stages next to this file
// before building the installer with -tags payload; it is never committed.
//
//go:embed payload.zip
var payload []byte
