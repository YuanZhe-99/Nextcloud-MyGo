package testprofile

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

// Remove deletes a test profile directory after its daemon was stopped, retrying for up to 30 s.
// Inputs: exe is the daemon executable the test ran.
// Side effects: removes dir; a file that stays locked fails the test only when a process running exe
// holds it (a daemon nobody stopped, for example one a client started on its own); any other holder,
// such as a scanner on a hosted runner (seen past 30 s), is logged and the profile is left behind.
func Remove(t testing.TB, dir, exe string) {
	t.Helper()
	var err error
	for deadline := time.Now().Add(30 * time.Second); ; time.Sleep(250 * time.Millisecond) {
		if err = os.RemoveAll(dir); err == nil {
			return
		}
		if time.Now().After(deadline) {
			break
		}
	}
	var pathErr *os.PathError
	if !errors.As(err, &pathErr) {
		t.Error(err)
		return
	}
	held, herr := holders(pathErr.Path)
	if herr != nil {
		t.Logf("profile left behind: %v (holders unknown: %v)", err, herr)
		return
	}
	for _, h := range held {
		if strings.EqualFold(filepath.Clean(h), filepath.Clean(exe)) {
			t.Errorf("%v: still held by a running daemon %s", err, h)
			return
		}
	}
	t.Logf("profile left behind: %v (held by %q)", err, held)
}

var (
	rstrtmgr            = windows.NewLazySystemDLL("rstrtmgr.dll")
	rmStartSession      = rstrtmgr.NewProc("RmStartSession")
	rmRegisterResources = rstrtmgr.NewProc("RmRegisterResources")
	rmGetList           = rstrtmgr.NewProc("RmGetList")
	rmEndSession        = rstrtmgr.NewProc("RmEndSession")
)

// rmProcessInfo is RM_PROCESS_INFO.
type rmProcessInfo struct {
	PID              uint32
	StartTime        windows.Filetime
	AppName          [256]uint16
	ServiceShortName [64]uint16
	AppType          uint32
	AppStatus        uint32
	TSSessionID      uint32
	Restartable      int32
}

// holders lists the processes that have file open, by image path where it can be read, through the
// Restart Manager.
func holders(file string) ([]string, error) {
	var session uint32
	key := make([]uint16, 33) // CCH_RM_SESSION_KEY + 1
	if r, _, _ := rmStartSession.Call(uintptr(unsafe.Pointer(&session)), 0, uintptr(unsafe.Pointer(&key[0]))); r != 0 {
		return nil, windows.Errno(r)
	}
	defer rmEndSession.Call(uintptr(session))
	name, err := windows.UTF16PtrFromString(file)
	if err != nil {
		return nil, err
	}
	if r, _, _ := rmRegisterResources.Call(uintptr(session), 1, uintptr(unsafe.Pointer(&name)), 0, 0, 0, 0); r != 0 {
		return nil, windows.Errno(r)
	}
	infos := make([]rmProcessInfo, 16)
	for {
		var needed, reasons uint32
		n := uint32(len(infos))
		r, _, _ := rmGetList.Call(uintptr(session), uintptr(unsafe.Pointer(&needed)), uintptr(unsafe.Pointer(&n)),
			uintptr(unsafe.Pointer(&infos[0])), uintptr(unsafe.Pointer(&reasons)))
		if windows.Errno(r) == windows.ERROR_MORE_DATA {
			infos = make([]rmProcessInfo, needed+4)
			continue
		}
		if r != 0 {
			return nil, windows.Errno(r)
		}
		var out []string
		for _, p := range infos[:n] {
			out = append(out, imagePath(p))
		}
		return out, nil
	}
}

// imagePath is the process's executable path, or the name the Restart Manager gave it.
func imagePath(p rmProcessInfo) string {
	if h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, p.PID); err == nil {
		defer windows.CloseHandle(h)
		buf := make([]uint16, windows.MAX_LONG_PATH)
		size := uint32(len(buf))
		if windows.QueryFullProcessImageName(h, 0, &buf[0], &size) == nil {
			return windows.UTF16ToString(buf[:size])
		}
	}
	return windows.UTF16ToString(p.AppName[:])
}
