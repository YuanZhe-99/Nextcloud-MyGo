//go:build !windows

package release

import (
	"errors"
	"os/exec"
)

// StartSetup: the installer is a Windows program; nothing else has one.
func StartSetup(setup, log string) (*exec.Cmd, error) {
	return nil, errors.New("the installer runs on Windows only")
}
