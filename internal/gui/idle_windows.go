package gui

import (
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows/registry"
)

var (
	user32           = syscall.NewLazyDLL("user32.dll")
	getLastInputInfo = user32.NewProc("GetLastInputInfo")
	kernel32         = syscall.NewLazyDLL("kernel32.dll")
	getTickCount     = kernel32.NewProc("GetTickCount")
)

// idleTime is how long the keyboard and mouse have been unused.
func idleTime() time.Duration {
	var info struct{ size, time uint32 }
	info.size = uint32(unsafe.Sizeof(info))
	if r, _, _ := getLastInputInfo.Call(uintptr(unsafe.Pointer(&info))); r == 0 {
		return 0
	}
	now, _, _ := getTickCount.Call()
	return time.Duration(uint32(now)-info.time) * time.Millisecond
}

// lightTaskbar reports whether Windows draws the taskbar light
// (Personalize's SystemUsesLightTheme), where a white tray glyph vanishes.
func lightTaskbar() bool {
	k, err := registry.OpenKey(registry.CURRENT_USER, `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`, registry.QUERY_VALUE)
	if err != nil {
		return false
	}
	defer k.Close()
	v, _, err := k.GetIntegerValue("SystemUsesLightTheme")
	return err == nil && v == 1
}
