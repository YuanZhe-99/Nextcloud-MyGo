//go:build windows

package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

// Registry locations of the user's Apps entry and startup entries.
const (
	uninstallKey = `Software\Microsoft\Windows\CurrentVersion\Uninstall\Nextcloud-MyGo`
	runKey       = `Software\Microsoft\Windows\CurrentVersion\Run`
	approvedKey  = `Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run`
)

// nativeMachine returns the machine Windows itself runs (IMAGE_FILE_MACHINE_*),
// which differs from this process's when it runs under emulation.
func nativeMachine() (uint16, error) {
	var process, native uint16
	err := windows.IsWow64Process2(windows.CurrentProcess(), &process, &native)
	return native, err
}

// process is a running process started from an executable under the install root.
type process struct {
	pid  uint32
	path string
}

// processesUnder lists this user's processes whose executable lies under root.
func processesUnder(root string) ([]process, error) {
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return nil, err
	}
	defer windows.CloseHandle(snap)
	var out []process
	e := windows.ProcessEntry32{Size: uint32(unsafe.Sizeof(windows.ProcessEntry32{}))}
	for err = windows.Process32First(snap, &e); err == nil; err = windows.Process32Next(snap, &e) {
		h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, e.ProcessID)
		if err != nil {
			continue // another user's or a protected process
		}
		buf := make([]uint16, windows.MAX_LONG_PATH)
		n := uint32(len(buf))
		if windows.QueryFullProcessImageName(h, 0, &buf[0], &n) == nil {
			if p := windows.UTF16ToString(buf[:n]); within(p, root) {
				out = append(out, process{e.ProcessID, p})
			}
		}
		windows.CloseHandle(h)
	}
	return out, nil
}

// waitExit waits until every process has exited or the timeout passes.
// Returns: the processes still running.
func waitExit(ps []process, timeout time.Duration) []process {
	deadline := time.Now().Add(timeout)
	var left []process
	for _, p := range ps {
		h, err := windows.OpenProcess(windows.SYNCHRONIZE, false, p.pid)
		if err != nil {
			continue // already gone
		}
		ms := max(time.Until(deadline).Milliseconds(), 0)
		if ev, _ := windows.WaitForSingleObject(h, uint32(ms)); ev != windows.WAIT_OBJECT_0 {
			left = append(left, p)
		}
		windows.CloseHandle(h)
	}
	return left
}

// terminate ends processes at once and waits briefly for them to go.
// Returns: the processes still running.
func terminate(ps []process) []process {
	for _, p := range ps {
		if h, err := windows.OpenProcess(windows.PROCESS_TERMINATE, false, p.pid); err == nil {
			windows.TerminateProcess(h, 1)
			windows.CloseHandle(h)
		}
	}
	return waitExit(ps, 10*time.Second)
}

// launch starts the UI detached from this console.
func launch(exe string) error {
	c := exec.Command(exe)
	c.Dir = filepath.Dir(exe)
	c.SysProcAttr = &syscall.SysProcAttr{CreationFlags: windows.DETACHED_PROCESS | windows.CREATE_NEW_PROCESS_GROUP}
	if err := c.Start(); err != nil {
		return err
	}
	return c.Process.Release()
}

// programsDir returns the user's own program folder (%LOCALAPPDATA%\Programs).
func programsDir() (string, error) {
	return windows.KnownFolderPath(windows.FOLDERID_UserProgramFiles, windows.KF_FLAG_CREATE)
}

// localAppData returns the folder SyncD keeps its profiles under; Qt's
// GenericDataLocation resolves to it on Windows.
func localAppData() (string, error) {
	return windows.KnownFolderPath(windows.FOLDERID_LocalAppData, 0)
}

// shortcutPath returns the Start menu shortcut's path.
func shortcutPath() (string, error) {
	dir, err := windows.KnownFolderPath(windows.FOLDERID_Programs, windows.KF_FLAG_CREATE)
	return filepath.Join(dir, "Nextcloud-MyGo.lnk"), err
}

// COM identities of the shell link object and the interfaces used on it.
var (
	clsidShellLink = windows.GUID{Data1: 0x00021401, Data4: [8]byte{0xc0, 0, 0, 0, 0, 0, 0, 0x46}}
	iidShellLinkW  = windows.GUID{Data1: 0x000214f9, Data4: [8]byte{0xc0, 0, 0, 0, 0, 0, 0, 0x46}}
	iidPersistFile = windows.GUID{Data1: 0x0000010b, Data4: [8]byte{0xc0, 0, 0, 0, 0, 0, 0, 0x46}}

	ole32            = windows.NewLazySystemDLL("ole32.dll")
	coCreateInstance = ole32.NewProc("CoCreateInstance")
)

// vcall calls method index of a COM object's vtable.
func vcall(obj unsafe.Pointer, index int, args ...uintptr) error {
	vtbl := *(*unsafe.Pointer)(obj)
	fn := *(*uintptr)(unsafe.Add(vtbl, index*int(unsafe.Sizeof(uintptr(0)))))
	hr, _, _ := syscall.SyscallN(fn, append([]uintptr{uintptr(obj)}, args...)...)
	if int32(hr) < 0 {
		return fmt.Errorf("COM method %d: %w", index, syscall.Errno(hr))
	}
	return nil
}

// writeShortcut creates or replaces the Start menu shortcut to exe, with the
// executable's own (Nextcloud) icon.
// Side effects: writes the .lnk under the user's Start menu Programs folder.
func writeShortcut(exe string) error {
	link, err := shortcutPath()
	if err != nil {
		return err
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	// S_FALSE (1): this thread already had COM; the call still pairs with CoUninitialize.
	if err := windows.CoInitializeEx(0, windows.COINIT_APARTMENTTHREADED); err != nil && !errors.Is(err, syscall.Errno(1)) {
		return err
	}
	defer windows.CoUninitialize()
	var sl, pf unsafe.Pointer
	if hr, _, _ := coCreateInstance.Call(uintptr(unsafe.Pointer(&clsidShellLink)), 0, windows.CLSCTX_INPROC_SERVER,
		uintptr(unsafe.Pointer(&iidShellLinkW)), uintptr(unsafe.Pointer(&sl))); int32(hr) < 0 {
		return fmt.Errorf("CoCreateInstance: %w", syscall.Errno(hr))
	}
	defer vcall(sl, 2) // Release
	str := func(s string) uintptr { p, _ := windows.UTF16PtrFromString(s); return uintptr(unsafe.Pointer(p)) }
	// IShellLinkW: SetDescription 7, SetWorkingDirectory 9, SetIconLocation 17, SetPath 20.
	for _, c := range []struct {
		index int
		args  []uintptr
	}{
		{20, []uintptr{str(exe)}},
		{9, []uintptr{str(filepath.Dir(exe))}},
		{7, []uintptr{str("Nextcloud-MyGo (test build)")}},
		{17, []uintptr{str(exe), 0}},
	} {
		if err := vcall(sl, c.index, c.args...); err != nil {
			return err
		}
	}
	if err := vcall(sl, 0, uintptr(unsafe.Pointer(&iidPersistFile)), uintptr(unsafe.Pointer(&pf))); err != nil {
		return err
	}
	defer vcall(pf, 2)
	return vcall(pf, 6, str(link), 1) // IPersistFile::Save(path, remember)
}

// writeAppsEntry registers the per-user entry in Settings > Apps.
func writeAppsEntry(root, exe, ver, arch string, sizeKB uint32) error {
	k, _, err := registry.CreateKey(registry.CURRENT_USER, uninstallKey, registry.SET_VALUE)
	if err != nil {
		return err
	}
	defer k.Close()
	uninstall := `"` + filepath.Join(root, "uninstall.exe") + `" -uninstall`
	for name, v := range map[string]string{
		"DisplayName":          "Nextcloud-MyGo (test build)",
		"DisplayVersion":       ver,
		"DisplayIcon":          exe + ",0",
		"Publisher":            "Nextcloud-MyGo",
		"Comments":             "Unsigned " + arch + " test build; automatic updates are off.",
		"InstallLocation":      root,
		"InstallDate":          time.Now().Format("20060102"),
		"UninstallString":      uninstall,
		"QuietUninstallString": uninstall,
	} {
		if err := k.SetStringValue(name, v); err != nil {
			return err
		}
	}
	for name, v := range map[string]uint32{"NoModify": 1, "NoRepair": 1, "EstimatedSize": sizeKB} {
		if err := k.SetDWordValue(name, v); err != nil {
			return err
		}
	}
	return nil
}

// deleteAppsEntry removes the entry in Settings > Apps.
func deleteAppsEntry() error {
	if err := registry.DeleteKey(registry.CURRENT_USER, uninstallKey); err != nil && !errors.Is(err, registry.ErrNotExist) {
		return err
	}
	return nil
}

// startupEntries applies fn to each of the user's Run entries that starts an
// executable under root; fn returns the new command, or "" to remove the entry.
// Side effects: removing an entry also removes its Task Manager on/off state;
// rewriting one keeps it, so a startup the user turned off stays off.
func startupEntries(root string, fn func(command string) string) error {
	k, err := registry.OpenKey(registry.CURRENT_USER, runKey, registry.QUERY_VALUE|registry.SET_VALUE)
	if errors.Is(err, registry.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	defer k.Close()
	names, err := k.ReadValueNames(0)
	if err != nil {
		return err
	}
	for _, name := range names {
		command, _, err := k.GetStringValue(name)
		if err != nil {
			continue
		}
		if exe, _ := startupTarget(command); !within(exe, root) {
			continue
		}
		next := fn(command)
		if next != "" {
			err = k.SetStringValue(name, next)
		} else if err = k.DeleteValue(name); err == nil {
			if a, e := registry.OpenKey(registry.CURRENT_USER, approvedKey, registry.SET_VALUE); e == nil {
				a.DeleteValue(name)
				a.Close()
			}
		}
		if err != nil {
			return err
		}
	}
	return nil
}

// removeLater deletes this uninstaller and the then empty install root once
// this process has exited, through a hidden cmd.exe; a running executable
// cannot delete itself.
func removeLater(exe, root string) error {
	script := fmt.Sprintf(`ping -n 3 127.0.0.1 >nul & del /f /q "%s" & rmdir /s /q "%s"`, exe, root)
	c := exec.Command(filepath.Join(os.Getenv("SystemRoot"), "System32", "cmd.exe"))
	c.SysProcAttr = &syscall.SysProcAttr{CmdLine: `cmd.exe /d /c "` + script + `"`, HideWindow: true,
		CreationFlags: windows.CREATE_NO_WINDOW}
	c.Dir = os.TempDir()
	if err := c.Start(); err != nil {
		return err
	}
	return c.Process.Release()
}
