package release

import (
	"os"
	"os/exec"
	"syscall"
)

// StartSetup hands a verified update over to its installer: setup runs
// without a console window, its output going to the file log, and outlives
// the interface, which it closes before it asks SyncD for the update safe
// point (doc/en-us/packaging.md, Install and upgrade).
// Returns: the running installer; Wait reports its exit while the caller is
// still alive (a refusal before setup closed the interface).
// Side effects: starts a process in the caller's environment, so the same
// NEXTCLOUD_MYGO_PROFILE.
func StartSetup(setup, log string) (*exec.Cmd, error) {
	f, err := os.Create(log)
	if err != nil {
		return nil, err
	}
	defer f.Close() // the child keeps its own handle
	c := exec.Command(setup)
	c.Stdout, c.Stderr = f, f
	// CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
	c.SysProcAttr = &syscall.SysProcAttr{CreationFlags: 0x08000000 | 0x00000200}
	if err := c.Start(); err != nil {
		return nil, err
	}
	return c, nil
}
