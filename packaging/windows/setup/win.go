//go:build windows

package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
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

// Registry locations of Cloud Files sync roots (HKLM) and of the Explorer
// navigation entries SyncD writes for on-demand folders (HKCU, on-demand.md).
const (
	syncRootsKey   = `SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager`
	classesKey     = `Software\Classes\CLSID`
	classesWow64   = `Software\Classes\Wow6432Node\CLSID`
	namespaceKey   = `Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace`
	hiddenIconsKey = `Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel`
)

var regDeleteTree = windows.NewLazySystemDLL("advapi32.dll").NewProc("RegDeleteTreeW")

// userSID returns the string SID of the user setup runs as, the second part
// of every sync root ID of that user.
func userSID() (string, error) {
	u, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return "", err
	}
	return u.User.Sid.String(), nil
}

// syncRoot is one Cloud Files registration of SyncD for this user.
type syncRoot struct {
	id, account, path string
	// clsid is the navigation entry the registration names (NamespaceCLSID).
	clsid string
}

// syncRoots lists the user's Nextcloud-MyGo sync root registrations, of
// every profile, with the local root Windows recorded for each.
// Notes: read-only; only Cloud Files (through SyncD) ever unregisters one.
func syncRoots(sid string) ([]syncRoot, error) {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, syncRootsKey, registry.ENUMERATE_SUB_KEYS)
	if errors.Is(err, registry.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	defer k.Close()
	names, err := k.ReadSubKeyNames(0)
	if err != nil {
		return nil, err
	}
	var out []syncRoot
	for _, id := range names {
		account, ok := rootAccount(id, sid)
		if !ok {
			continue
		}
		path, clsid := "(unknown folder)", ""
		if r, err := registry.OpenKey(k, id, registry.QUERY_VALUE); err == nil {
			clsid, _, _ = r.GetStringValue("NamespaceCLSID")
			r.Close()
		}
		if u, err := registry.OpenKey(k, id+`\UserSyncRoots`, registry.QUERY_VALUE); err == nil {
			if p, _, err := u.GetStringValue(sid); err == nil && p != "" {
				path = p
			}
			u.Close()
		}
		out = append(out, syncRoot{id, account, path, clsid})
	}
	return out, nil
}

// tagged reports whether the HKCU key carries ApplicationName=Nextcloud-MyGo.
func tagged(key string) bool {
	k, err := registry.OpenKey(registry.CURRENT_USER, key, registry.QUERY_VALUE)
	if err != nil {
		return false
	}
	defer k.Close()
	v, _, err := k.GetStringValue("ApplicationName")
	return err == nil && ourTag(v)
}

// removeNavigation deletes the user's leftover Explorer navigation entries of
// Nextcloud-MyGo: the CLSID keys (both views) and NameSpace keys tagged
// ApplicationName=Nextcloud-MyGo that no registered sync root names and
// whose target folder is no longer a registered sync root (0.1.1's
// per-folder entries), and their HideDesktopIcons values. It backs up
// SyncD's own removal, which ends each entry with the last folder naming it.
// Side effects: deletes HKCU keys and values; never an entry tagged
// otherwise (the official client's say "Nextcloud"), nor one still serving a
// root of another profile.
// Notes: a HideDesktopIcons value whose CLSID keys are already gone carries
// no tag, so it is left alone.
func removeNavigation() error {
	sid, err := userSID()
	if err != nil {
		return err
	}
	roots, err := syncRoots(sid)
	if err != nil {
		return err
	}
	live, named := map[string]bool{}, map[string]bool{}
	for _, r := range roots {
		live[rootPath(r.path)] = true
		named[strings.ToUpper(r.clsid)] = true
	}
	keep, removed := map[string]bool{}, map[string]bool{}
	var errs []error
	for _, tree := range []string{classesKey, classesWow64, namespaceKey} {
		for _, name := range subKeys(tree) {
			key := tree + `\` + name
			if name == "" || !tagged(key) || keep[name] {
				continue
			}
			if named[strings.ToUpper(name)] {
				keep[name] = true
				continue
			}
			if tree != namespaceKey {
				if target := stringValue(key+`\Instance\InitPropertyBag`, "TargetFolderPath"); target != "" && live[rootPath(target)] {
					keep[name] = true
					continue
				}
			}
			if r, _, _ := regDeleteTree.Call(uintptr(registry.CURRENT_USER), uintptr(unsafe.Pointer(windows.StringToUTF16Ptr(key)))); r != 0 && syscall.Errno(r) != windows.ERROR_FILE_NOT_FOUND {
				errs = append(errs, fmt.Errorf("removing %s: %w", key, syscall.Errno(r)))
				continue
			}
			removed[name] = true
		}
	}
	if len(removed) > 0 {
		if k, err := registry.OpenKey(registry.CURRENT_USER, hiddenIconsKey, registry.SET_VALUE); err == nil {
			for name := range removed {
				if !keep[name] {
					if err := k.DeleteValue(name); err != nil && !errors.Is(err, registry.ErrNotExist) {
						errs = append(errs, err)
					}
				}
			}
			k.Close()
		}
		step("removed leftover Explorer navigation entries of Nextcloud MyGo (%d)", len(removed))
	}
	if err := errors.Join(errs...); err != nil {
		return fmt.Errorf("some Explorer navigation entries of Nextcloud MyGo could not be removed: %w", err)
	}
	return nil
}

// subKeys lists the names of an HKCU key's subkeys, none when it is missing.
func subKeys(key string) []string {
	k, err := registry.OpenKey(registry.CURRENT_USER, key, registry.ENUMERATE_SUB_KEYS)
	if err != nil {
		return nil
	}
	defer k.Close()
	names, _ := k.ReadSubKeyNames(0)
	return names
}

// stringValue reads one HKCU string value, "" when it is missing.
func stringValue(key, name string) string {
	k, err := registry.OpenKey(registry.CURRENT_USER, key, registry.QUERY_VALUE)
	if err != nil {
		return ""
	}
	defer k.Close()
	v, _, _ := k.GetStringValue(name)
	return v
}

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

// elevatedByUAC reports whether this process runs with the full token of a
// UAC administrator: started with Run as administrator, or elevated by
// Windows' installer detection (the 0.1.2 setup had no manifest).
// Notes: an account without a split token (UAC off) runs everything at one
// level and is not elevated in this sense.
func elevatedByUAC() bool {
	var kind, n uint32
	err := windows.GetTokenInformation(windows.GetCurrentProcessToken(), windows.TokenElevationType, (*byte)(unsafe.Pointer(&kind)), 4, &n)
	return err == nil && kind == 2 // TokenElevationTypeFull
}

// elevatedApp reports whether any of ps runs elevated while setup does not.
func elevatedApp(ps []process) bool {
	return !windows.GetCurrentProcessToken().IsElevated() && slices.ContainsFunc(ps, runsElevated)
}

// runsElevated reports whether p runs with an elevated token; false when it
// is gone or its token cannot be read.
// Notes: a medium-integrity setup may still terminate such a process (the
// right counts as execute access), but SyncD's pipe and its safe point are
// out of reach, so setup refuses before closing anything (stopApp).
func runsElevated(p process) bool {
	h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, p.pid)
	if err != nil {
		return false
	}
	defer windows.CloseHandle(h)
	var token windows.Token
	if windows.OpenProcessToken(h, windows.TOKEN_QUERY, &token) != nil {
		return false
	}
	defer token.Close()
	return token.IsElevated()
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
	return filepath.Join(dir, displayName+".lnk"), err
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
// Notes: like LazyProc.Call, pointers converted to uintptr in the call
// expression stay alive and do not move until vcall returns; arguments built
// earlier must be kept alive by the caller.
//
//go:uintptrescapes
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
	// Heap cells, so COM's out-parameter writes never land on a moved stack.
	sl, pf := new(unsafe.Pointer), new(unsafe.Pointer)
	if hr, _, _ := coCreateInstance.Call(uintptr(unsafe.Pointer(&clsidShellLink)), 0, windows.CLSCTX_INPROC_SERVER,
		uintptr(unsafe.Pointer(&iidShellLinkW)), uintptr(unsafe.Pointer(sl))); int32(hr) < 0 || *sl == nil {
		return fmt.Errorf("CoCreateInstance: %w", syscall.Errno(hr))
	}
	defer vcall(*sl, 2) // Release
	// The UTF-16 strings are passed as uintptr, so keep them reachable until
	// every call that reads them has returned.
	var keep []*uint16
	defer func() { runtime.KeepAlive(keep) }()
	str := func(s string) uintptr {
		p, _ := windows.UTF16PtrFromString(s)
		keep = append(keep, p)
		return uintptr(unsafe.Pointer(p))
	}
	// IShellLinkW: SetDescription 7, SetWorkingDirectory 9, SetIconLocation 17, SetPath 20.
	for _, c := range []struct {
		index int
		args  []uintptr
	}{
		{20, []uintptr{str(exe)}},
		{9, []uintptr{str(filepath.Dir(exe))}},
		{7, []uintptr{str(displayName + " (test build)")}},
		{17, []uintptr{str(exe), 0}},
	} {
		if err := vcall(*sl, c.index, c.args...); err != nil {
			return err
		}
	}
	if err := vcall(*sl, 0, uintptr(unsafe.Pointer(&iidPersistFile)), uintptr(unsafe.Pointer(pf))); err != nil {
		return err
	}
	if *pf == nil {
		return errors.New("IShellLink did not return IPersistFile")
	}
	defer vcall(*pf, 2)
	return vcall(*pf, 6, str(link), 1) // IPersistFile::Save(path, remember)
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
		"DisplayName":          displayName + " (test build)",
		"DisplayVersion":       ver,
		"DisplayIcon":          exe + ",0",
		"Publisher":            displayName,
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
// Notes: the uninstaller's window stays open on its result until it is
// closed, so the script retries the delete once a second for up to ten
// minutes before removing the root.
func removeLater(exe, root string) error {
	script := fmt.Sprintf(`ping -n 3 127.0.0.1 >nul & (for /l %%i in (1,1,600) do @if exist "%[1]s" (del /f /q "%[1]s" 2>nul & if exist "%[1]s" ping -n 2 127.0.0.1 >nul)) & rmdir /s /q "%[2]s"`, exe, root)
	c := exec.Command(filepath.Join(os.Getenv("SystemRoot"), "System32", "cmd.exe"))
	c.SysProcAttr = &syscall.SysProcAttr{CmdLine: `cmd.exe /d /c "` + script + `"`, HideWindow: true,
		CreationFlags: windows.CREATE_NO_WINDOW}
	c.Dir = os.TempDir()
	if err := c.Start(); err != nil {
		return err
	}
	return c.Process.Release()
}
