//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"unicode"
	"unicode/utf8"
	"unsafe"

	"golang.org/x/sys/windows"
)

// Win32 entry points of the window; the .syso tools/pkg resources writes
// supplies its icon and the manifest that selects Common Controls v6 and
// per-monitor DPI awareness.
var (
	user32   = windows.NewLazySystemDLL("user32.dll")
	gdi32    = windows.NewLazySystemDLL("gdi32.dll")
	comctl32 = windows.NewLazySystemDLL("comctl32.dll")
	kernel32 = windows.NewLazySystemDLL("kernel32.dll")

	procRegisterClassEx      = user32.NewProc("RegisterClassExW")
	procCreateWindowEx       = user32.NewProc("CreateWindowExW")
	procDefWindowProc        = user32.NewProc("DefWindowProcW")
	procDestroyWindow        = user32.NewProc("DestroyWindow")
	procGetMessage           = user32.NewProc("GetMessageW")
	procIsDialogMessage      = user32.NewProc("IsDialogMessageW")
	procTranslateMessage     = user32.NewProc("TranslateMessage")
	procDispatchMessage      = user32.NewProc("DispatchMessageW")
	procPostMessage          = user32.NewProc("PostMessageW")
	procSendMessage          = user32.NewProc("SendMessageW")
	procPostQuitMessage      = user32.NewProc("PostQuitMessage")
	procShowWindow           = user32.NewProc("ShowWindow")
	procEnableWindow         = user32.NewProc("EnableWindow")
	procSetFocus             = user32.NewProc("SetFocus")
	procSetWindowText        = user32.NewProc("SetWindowTextW")
	procSetWindowPos         = user32.NewProc("SetWindowPos")
	procLoadImage            = user32.NewProc("LoadImageW")
	procLoadCursor           = user32.NewProc("LoadCursorW")
	procDestroyIcon          = user32.NewProc("DestroyIcon")
	procGetDpiForWindow      = user32.NewProc("GetDpiForWindow")
	procAdjustWindowRectEx   = user32.NewProc("AdjustWindowRectExForDpi")
	procMonitorFromWindow    = user32.NewProc("MonitorFromWindow")
	procGetMonitorInfo       = user32.NewProc("GetMonitorInfoW")
	procGetSysColor          = user32.NewProc("GetSysColor")
	procGetSysColorBrush     = user32.NewProc("GetSysColorBrush")
	procSetTimer             = user32.NewProc("SetTimer")
	procCreateFont           = gdi32.NewProc("CreateFontW")
	procDeleteObject         = gdi32.NewProc("DeleteObject")
	procSetBkMode            = gdi32.NewProc("SetBkMode")
	procSetTextColor         = gdi32.NewProc("SetTextColor")
	procInitCommonControlsEx = comctl32.NewProc("InitCommonControlsEx")
	procGetModuleHandle      = kernel32.NewProc("GetModuleHandleW")
	procAttachConsole        = kernel32.NewProc("AttachConsole")
)

// Window styles, messages and control values used here (winuser.h,
// commctrl.h).
const (
	wsCaption      = 0x00c00000
	wsSysMenu      = 0x00080000
	wsMinimizeBox  = 0x00020000
	wsClipChildren = 0x02000000
	wsChild        = 0x40000000
	wsVisible      = 0x10000000
	wsTabStop      = 0x00010000
	windowStyle    = wsCaption | wsSysMenu | wsMinimizeBox | wsClipChildren

	ssIcon        = 0x0003
	ssNoPrefix    = 0x0080
	ssEditControl = 0x2000
	bsDefPush     = 0x0001
	bsAutoCheck   = 0x0003
	pbsMarquee    = 0x0008

	wmDestroy     = 0x0002
	wmClose       = 0x0010
	wmSetFont     = 0x0030
	wmSetIcon     = 0x0080
	wmCommand     = 0x0111
	wmTimer       = 0x0113
	wmCtlColorStc = 0x0138
	wmDpiChanged  = 0x02e0
	wmRun         = 0x8000 // WM_APP: run the queued calls on the window thread
	bmGetCheck    = 0x00f0
	bmSetCheck    = 0x00f1
	stmSetImage   = 0x0172
	pbmSetMarquee = 0x040a

	idOK     = 1
	idCancel = 2
	idCheck  = 100

	imageIcon     = 1
	colorWindow   = 5
	colorGrayText = 17
	swHide        = 0
	swShow        = 5
	swpPlace      = 0x0004 | 0x0010 // SWP_NOZORDER | SWP_NOACTIVATE
	cwUseDefault  = 0x80000000
)

// caption is the title bar text of every setup window and dialog.
const caption = "Nextcloud MyGo Setup"

// unsignedNote is the one line about the missing publisher signature.
const unsignedNote = "This copy is not signed by a publisher, so Windows may warn before it runs."

// rect is a Win32 RECT.
type rect struct{ left, top, right, bottom int32 }

// msg is a Win32 MSG.
type msg struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	pt      struct{ x, y int32 }
	private uint32
}

// wndClassEx is a Win32 WNDCLASSEXW.
type wndClassEx struct {
	size       uint32
	style      uint32
	wndProc    uintptr
	clsExtra   int32
	wndExtra   int32
	instance   uintptr
	icon       uintptr
	cursor     uintptr
	background uintptr
	menuName   *uint16
	className  *uint16
	iconSm     uintptr
}

// page is what the window shows at one stage of the work.
type page struct {
	message string
	check   string // the checkbox's label; "" hides it
	checked bool
	ok      string // the default button's label; "" hides it
	cancel  bool   // shows Cancel
	busy    bool   // shows the progress bar; buttons and the close box do nothing
}

// window is setup's one window: icon, product name, version, a message, a
// progress bar, a checkbox, a note and two buttons.
// Notes: every field is used on the window thread only, except queue (mu).
type window struct {
	hwnd, instance                                                   uintptr
	icon, title, version, message, progress, check, note, ok, cancel uintptr
	font, titleFont, image                                           uintptr
	dpi                                                              int
	busy                                                             bool
	onOK, onClose, onTimer                                           func()
	exit                                                             int

	mu    sync.Mutex
	queue []func()
}

// ui is the window wndProc serves.
var ui *window

// runWindow shows setup's window for t and runs the work from it: a
// confirmation first, then progress, then the result; with -update it starts
// at once and closes itself after a success.
// Returns: the exit code (0 done, 1 failed, 2 cancelled before anything
// changed).
// Side effects: everything install or uninstallApp does; may start the
// installed interface.
// Notes: the window lives on this OS thread; the work runs on a goroutine
// and reaches the window through window.do.
func runWindow(o options, t task, root, profile string) int {
	runtime.LockOSThread()
	_, installed, isInstalled, _ := readState(root)
	w := &window{}
	ui = w
	if err := w.create(); err != nil {
		errorBox("Nextcloud MyGo setup could not open its window: " + err.Error())
		return 1
	}
	removeData := false
	begin := func(first string) {
		removeData = t == taskUninstall && w.checked()
		w.show(page{message: first, busy: true})
		onStep = func(s string) { w.do(func() { w.setText(w.message, sentence(s)) }) }
		go func() {
			err := run(o, t, root, profile, removeData, o.update && !o.noLaunch)
			w.do(func() { w.finish(o, t, root, removeData, err) })
		}()
	}
	w.onClose = func() { w.quit(2) }
	switch t {
	case taskUninstall:
		w.setText(w.title, "Uninstall Nextcloud MyGo")
		if isInstalled {
			w.setText(w.version, "Version "+installed.Version)
		}
		w.onOK = func() { begin("Removing Nextcloud MyGo…") }
		w.show(page{message: "Remove Nextcloud MyGo? Your synced files stay on this PC and on the server.",
			check: "Also remove my accounts and settings from this PC", checked: o.removeData, ok: "Remove", cancel: true})
	default:
		_, m, err := payloadManifest()
		if err != nil {
			w.finish(o, t, root, false, err)
			break
		}
		w.setText(w.version, fmt.Sprintf("Version %s for %s", m.Version, m.Arch))
		note := unsignedNote
		if m.TestVersion {
			note += " Its version was stamped for an upgrade test; it is not a release."
		}
		w.setText(w.note, note)
		if o.update {
			begin(fmt.Sprintf("Updating Nextcloud MyGo to %s…", m.Version))
			break
		}
		text := "Setup installs Nextcloud MyGo for your Windows account. It needs no administrator rights."
		switch {
		case isInstalled && installed.Version == m.Version:
			text = fmt.Sprintf("Nextcloud MyGo %s is already installed. Setup installs it again; your accounts, settings and synced files stay.", m.Version)
		case isInstalled:
			text = fmt.Sprintf("Nextcloud MyGo %s is installed. Setup updates it to %s; your accounts, settings and synced files stay.", installed.Version, m.Version)
		}
		w.onOK = func() { begin("Installing Nextcloud MyGo…") }
		w.show(page{message: text, ok: "Install", cancel: true})
	}
	procShowWindow.Call(w.hwnd, swShow)
	var m msg
	for {
		if r, _, _ := procGetMessage.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0); int32(r) <= 0 {
			break
		}
		if r, _, _ := procIsDialogMessage.Call(w.hwnd, uintptr(unsafe.Pointer(&m))); r == 0 {
			procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
			procDispatchMessage.Call(uintptr(unsafe.Pointer(&m)))
		}
	}
	return w.exit
}

// finish shows the result of the work (or of reading the payload) and what
// the buttons do next.
// Notes: after an install the checkbox opens the installed version on
// Finish; after an -update the new version was already started and the
// window closes itself after two seconds.
func (w *window) finish(o options, t task, root string, removeData bool, err error) {
	onStep = nil
	w.onClose = func() { w.quit(0) }
	if err != nil {
		w.onClose = func() { w.quit(1) }
		w.onOK = w.onClose
		text := "Setup did not finish. " + sentence(err.Error())
		if _, e := os.Stat(filepath.Join(root, "install.json")); t == taskUninstall && os.IsNotExist(e) {
			text = "Nextcloud MyGo was removed, but " + strings.TrimSuffix(err.Error(), ".") + "."
		}
		w.show(page{message: text, ok: "Close"})
		return
	}
	switch {
	case t == taskUninstall:
		text := "Nextcloud MyGo was removed. Your synced files stay on this PC and on the server."
		if removeData {
			text += " Your accounts and settings were removed too."
		}
		w.onOK = w.onClose
		w.show(page{message: text, ok: "Close"})
	case o.update:
		w.onOK = w.onClose
		w.onTimer = w.onClose
		w.show(page{message: "Nextcloud MyGo is updated.", ok: "Close"})
		procSetTimer.Call(w.hwnd, 1, 2000, 0)
	default:
		w.onOK = func() {
			if w.checked() {
				if dir, _, ok, _ := readState(root); ok {
					if err := launch(filepath.Join(dir, uiExe)); err != nil {
						errorBox("Nextcloud MyGo could not be started: " + err.Error())
					}
				}
			}
			w.quit(0)
		}
		w.show(page{message: "Nextcloud MyGo is installed.", check: "Open Nextcloud MyGo", checked: !o.noLaunch, ok: "Finish"})
	}
}

// create registers the window class and creates the hidden window and its
// controls, laid out for the monitor's DPI and centered on it.
func (w *window) create() error {
	icc := [2]uint32{8, 0x20} // INITCOMMONCONTROLSEX: ICC_PROGRESS_CLASS
	procInitCommonControlsEx.Call(uintptr(unsafe.Pointer(&icc)))
	w.instance, _, _ = procGetModuleHandle.Call(0)
	class := windows.StringToUTF16Ptr("NextcloudMyGoSetup")
	cursor, _, _ := procLoadCursor.Call(0, 32512) // IDC_ARROW
	wc := wndClassEx{wndProc: windows.NewCallback(wndProc), instance: w.instance, cursor: cursor,
		background: colorWindow + 1, className: class}
	wc.size = uint32(unsafe.Sizeof(wc))
	if r, _, err := procRegisterClassEx.Call(uintptr(unsafe.Pointer(&wc))); r == 0 {
		return err
	}
	h, _, err := procCreateWindowEx.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(windows.StringToUTF16Ptr(caption))),
		windowStyle, cwUseDefault, cwUseDefault, 100, 100, 0, 0, w.instance, 0)
	if h == 0 {
		return err
	}
	w.hwnd = h
	w.icon = w.child("STATIC", "", ssIcon, 0)
	w.title = w.child("STATIC", "Nextcloud MyGo", ssNoPrefix, 0)
	w.version = w.child("STATIC", "", ssNoPrefix, 0)
	w.message = w.child("STATIC", "", ssNoPrefix|ssEditControl, 0)
	w.progress = w.child("msctls_progress32", "", pbsMarquee, 0)
	w.check = w.child("BUTTON", "", bsAutoCheck|wsTabStop, idCheck)
	w.note = w.child("STATIC", "", ssNoPrefix|ssEditControl, 0)
	w.ok = w.child("BUTTON", "", bsDefPush|wsTabStop, idOK)
	w.cancel = w.child("BUTTON", "Cancel", wsTabStop, idCancel)
	dpi, _, _ := procGetDpiForWindow.Call(h)
	w.layout(int(dpi))
	w.place(nil)
	return nil
}

// child creates one visible control of the window.
func (w *window) child(class, text string, style, id uintptr) uintptr {
	h, _, _ := procCreateWindowEx.Call(0, uintptr(unsafe.Pointer(windows.StringToUTF16Ptr(class))),
		uintptr(unsafe.Pointer(windows.StringToUTF16Ptr(text))), wsChild|wsVisible|style, 0, 0, 0, 0, w.hwnd, id, w.instance, 0)
	return h
}

// layout sizes the fonts, icons and controls for dpi (96 is 100%).
func (w *window) layout(dpi int) {
	w.dpi = dpi
	s := func(v int) uintptr { return uintptr(v * dpi / 96) }
	font, titleFont := w.font, w.titleFont
	w.font, w.titleFont = createFont(9, 400, dpi), createFont(13, 600, dpi)
	for _, c := range []uintptr{w.version, w.message, w.check, w.note, w.ok, w.cancel} {
		procSendMessage.Call(c, wmSetFont, w.font, 1)
	}
	procSendMessage.Call(w.title, wmSetFont, w.titleFont, 1)
	for _, f := range []uintptr{font, titleFont} {
		if f != 0 {
			procDeleteObject.Call(f)
		}
	}
	for _, c := range []struct {
		h           uintptr
		x, y, w, h2 int
	}{
		{w.icon, 24, 24, 48, 48},
		{w.title, 88, 24, 368, 30},
		{w.version, 88, 56, 368, 20},
		{w.message, 24, 92, 432, 78},
		{w.progress, 24, 180, 432, 14},
		{w.check, 24, 176, 432, 22},
		{w.note, 24, 210, 432, 36},
	} {
		procSetWindowPos.Call(c.h, 0, s(c.x), s(c.y), s(c.w), s(c.h2), swpPlace)
	}
	w.placeButtons()
	// The icon resource (ID 1) is in the .syso; a build without it has none.
	if img := w.loadIcon(48 * dpi / 96); img != 0 {
		if old, _, _ := procSendMessage.Call(w.icon, stmSetImage, imageIcon, img); old != 0 {
			procDestroyIcon.Call(old)
		}
	}
	procSendMessage.Call(w.hwnd, wmSetIcon, 1, w.loadIcon(32*dpi/96)) // ICON_BIG
	procSendMessage.Call(w.hwnd, wmSetIcon, 0, w.loadIcon(16*dpi/96)) // ICON_SMALL
}

// placeButtons puts the default button left of Cancel, or in its place when
// Cancel is hidden.
func (w *window) placeButtons() {
	s := func(v int) uintptr { return uintptr(v * w.dpi / 96) }
	x := 368
	if visible(w.cancel) {
		x = 272
	}
	procSetWindowPos.Call(w.ok, 0, s(x), s(252), s(88), s(28), swpPlace)
	procSetWindowPos.Call(w.cancel, 0, s(368), s(252), s(88), s(28), swpPlace)
}

// place sizes the window for its client area at the current DPI, at the
// top-left corner at (when Windows suggests one on a DPI change) or centered
// on its monitor's work area.
func (w *window) place(at *rect) {
	r := rect{0, 0, int32(480 * w.dpi / 96), int32(300 * w.dpi / 96)}
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&r)), windowStyle, 0, 0, uintptr(w.dpi))
	width, height := r.right-r.left, r.bottom-r.top
	var x, y int32
	if at != nil {
		x, y = at.left, at.top
	} else {
		mon, _, _ := procMonitorFromWindow.Call(w.hwnd, 2) // MONITOR_DEFAULTTONEAREST
		var mi struct {
			size          uint32
			monitor, work rect
			flags         uint32
		}
		mi.size = uint32(unsafe.Sizeof(mi))
		procGetMonitorInfo.Call(mon, uintptr(unsafe.Pointer(&mi)))
		x = mi.work.left + (mi.work.right-mi.work.left-width)/2
		y = mi.work.top + (mi.work.bottom-mi.work.top-height)/2
	}
	procSetWindowPos.Call(w.hwnd, 0, uintptr(x), uintptr(y), uintptr(width), uintptr(height), swpPlace)
}

// loadIcon loads the executable's icon (resource 1) at size pixels, 0 when
// the build carries none.
func (w *window) loadIcon(size int) uintptr {
	h, _, _ := procLoadImage.Call(w.instance, 1, imageIcon, uintptr(size), uintptr(size), 0)
	return h
}

// show switches the window to p.
func (w *window) show(p page) {
	w.setText(w.message, p.message)
	w.setText(w.check, p.check)
	w.setText(w.ok, p.ok)
	checked := uintptr(0)
	if p.checked {
		checked = 1
	}
	procSendMessage.Call(w.check, bmSetCheck, checked, 0)
	setVisible(w.check, p.check != "")
	setVisible(w.ok, p.ok != "")
	setVisible(w.cancel, p.cancel)
	setVisible(w.progress, p.busy)
	marquee := uintptr(0)
	if p.busy {
		marquee = 1
	}
	procSendMessage.Call(w.progress, pbmSetMarquee, marquee, 30)
	w.busy = p.busy
	w.placeButtons()
	if p.ok != "" {
		procSetFocus.Call(w.ok)
	}
}

// checked reports whether the checkbox is ticked.
func (w *window) checked() bool {
	r, _, _ := procSendMessage.Call(w.check, bmGetCheck, 0, 0)
	return r == 1
}

// setText replaces a control's text.
func (w *window) setText(h uintptr, s string) {
	procSetWindowText.Call(h, uintptr(unsafe.Pointer(windows.StringToUTF16Ptr(s))))
}

// do runs f on the window thread; any goroutine may call it.
func (w *window) do(f func()) {
	w.mu.Lock()
	w.queue = append(w.queue, f)
	w.mu.Unlock()
	procPostMessage.Call(w.hwnd, wmRun, 0, 0)
}

// quit closes the window, which ends runWindow with code.
func (w *window) quit(code int) {
	w.exit = code
	procDestroyWindow.Call(w.hwnd)
}

// wndProc handles the window's messages.
// Notes: Enter and Esc arrive as IDOK and IDCANCEL through
// IsDialogMessage; while the work runs, neither they nor the close box do
// anything, since the install or uninstall cannot stop halfway.
func wndProc(hwnd, message, wParam, lParam uintptr) uintptr {
	w := ui
	switch message {
	case wmCommand:
		switch wParam & 0xffff {
		case idOK:
			if !w.busy && w.onOK != nil {
				w.onOK()
			}
			return 0
		case idCancel:
			if !w.busy && w.onClose != nil {
				w.onClose()
			}
			return 0
		}
	case wmClose:
		if !w.busy && w.onClose != nil {
			w.onClose()
		}
		return 0
	case wmTimer:
		if w.onTimer != nil {
			w.onTimer()
		}
		return 0
	case wmDestroy:
		procPostQuitMessage.Call(0)
		return 0
	case wmRun:
		w.mu.Lock()
		q := w.queue
		w.queue = nil
		w.mu.Unlock()
		for _, f := range q {
			f()
		}
		return 0
	case wmCtlColorStc:
		// Static texts and the checkbox draw on the window's own background;
		// the note is grey.
		procSetBkMode.Call(wParam, 1) // TRANSPARENT
		if lParam == w.note {
			c, _, _ := procGetSysColor.Call(colorGrayText)
			procSetTextColor.Call(wParam, c)
		}
		b, _, _ := procGetSysColorBrush.Call(colorWindow)
		return b
	case wmDpiChanged:
		suggested := *(*rect)(unsafe.Add(unsafe.Pointer(nil), lParam))
		w.layout(int(wParam & 0xffff))
		w.place(&suggested)
		return 0
	}
	r, _, _ := procDefWindowProc.Call(hwnd, message, wParam, lParam)
	return r
}

// createFont creates Segoe UI at pt points and weight for dpi.
func createFont(pt, weight, dpi int) uintptr {
	height := -(pt*dpi + 36) / 72
	f, _, _ := procCreateFont.Call(uintptr(int32(height)), 0, 0, 0, uintptr(weight), 0, 0, 0,
		1, 0, 0, 5, 0, uintptr(unsafe.Pointer(windows.StringToUTF16Ptr("Segoe UI")))) // DEFAULT_CHARSET, CLEARTYPE_QUALITY
	return f
}

// setVisible shows or hides a control.
func setVisible(h uintptr, show bool) {
	cmd := uintptr(swHide)
	if show {
		cmd = swShow
	}
	procShowWindow.Call(h, cmd)
}

// visible reports whether a control is shown (WS_VISIBLE).
func visible(h uintptr) bool {
	return windows.IsWindowVisible(windows.HWND(h))
}

// errorBox shows text in an error dialog.
func errorBox(text string) {
	windows.MessageBox(0, windows.StringToUTF16Ptr(text), windows.StringToUTF16Ptr(caption), windows.MB_OK|windows.MB_ICONERROR)
}

// attachConsole gives -quiet output a place to go when setup (a GUI
// program) was started from a console without redirected output: it
// attaches to the parent's console.
// Notes: with redirected output (scripts, the in-app update's log) the
// inherited handles are used as they are.
func attachConsole() {
	if h, err := windows.GetStdHandle(windows.STD_OUTPUT_HANDLE); err == nil && h != 0 && h != windows.InvalidHandle {
		return
	}
	if r, _, _ := procAttachConsole.Call(uintptr(^uint32(0))); r == 0 { // ATTACH_PARENT_PROCESS
		return
	}
	if f, err := os.OpenFile("CONOUT$", os.O_WRONLY, 0); err == nil {
		os.Stdout, os.Stderr = f, f
	}
}

// sentence turns a step or an error into a sentence for the window: first
// letter upper case, ending in a full stop.
func sentence(s string) string {
	s = strings.TrimSpace(s)
	if s == "" {
		return s
	}
	r, n := utf8.DecodeRuneInString(s)
	s = string(unicode.ToUpper(r)) + s[n:]
	if last, _ := utf8.DecodeLastRuneInString(s); !strings.ContainsRune(".!?…", last) {
		s += "."
	}
	return s
}
