//go:build !windows

package main

import (
	"fmt"
	"os"
)

// main reports that the installer exists for Windows only.
func main() {
	fmt.Fprintln(os.Stderr, "Nextcloud MyGo setup runs on Windows only")
	os.Exit(2)
}
