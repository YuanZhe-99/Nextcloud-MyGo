package syncd

import (
	"context"
	"crypto/sha256"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"

	"golang.org/x/sys/windows"
)

// pipeName binds a profile to the current Windows user, session and protocol major.
func pipeName(profile string) (string, error) {
	u, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return "", err
	}
	var session uint32
	if err := windows.ProcessIdToSessionId(windows.GetCurrentProcessId(), &session); err != nil {
		return "", err
	}
	hash := sha256.Sum256([]byte(profile))
	return fmt.Sprintf(`\\.\pipe\Nextcloud-MyGo-%s-%d-%x-v1`, u.User.Sid.String(), session, hash[:12]), nil
}

// logonSID extracts the session's logon identity, not merely the user's account SID.
func logonSID(token windows.Token) (string, error) {
	groups, err := token.GetTokenGroups()
	if err != nil {
		return "", err
	}
	for _, group := range groups.AllGroups() {
		if group.Attributes&windows.SE_GROUP_LOGON_ID == windows.SE_GROUP_LOGON_ID {
			return group.Sid.String(), nil
		}
	}
	return "", ErrRefused
}

// dialPipe opens only a local pipe and verifies its server process before sending any RPC.
func dialPipe(ctx context.Context, profile, executable string) (io.ReadWriteCloser, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	name, err := pipeName(profile)
	if err != nil {
		return nil, err
	}
	path, err := windows.UTF16PtrFromString(name)
	if err != nil {
		return nil, err
	}
	// Exclude FILE_CREATE_PIPE_INSTANCE from access, and prevent server impersonation beyond identity queries.
	h, err := windows.CreateFile(path, 0x0012019b, 0, nil, windows.OPEN_EXISTING,
		windows.FILE_FLAG_OVERLAPPED|windows.SECURITY_SQOS_PRESENT|windows.SECURITY_IDENTIFICATION, 0)
	if err != nil {
		return nil, err
	}
	if err := verifyPipeServer(h, executable); err != nil {
		windows.CloseHandle(h)
		return nil, err
	}
	return os.NewFile(uintptr(h), name), nil
}

// verifyPipeServer checks the actual pipe owner, user, logon session and controlled executable path.
func verifyPipeServer(pipe windows.Handle, executable string) error {
	var pid, session, ownSession uint32
	if err := windows.GetNamedPipeServerProcessId(pipe, &pid); err != nil {
		return err
	}
	if err := windows.ProcessIdToSessionId(pid, &session); err != nil {
		return err
	}
	if err := windows.ProcessIdToSessionId(windows.GetCurrentProcessId(), &ownSession); err != nil {
		return err
	}
	if session != ownSession {
		return ErrRefused
	}
	process, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
	if err != nil {
		return err
	}
	defer windows.CloseHandle(process)
	var token windows.Token
	if err := windows.OpenProcessToken(process, windows.TOKEN_QUERY, &token); err != nil {
		return err
	}
	defer token.Close()
	user, err := token.GetTokenUser()
	if err != nil {
		return err
	}
	ownUser, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return err
	}
	if !user.User.Sid.Equals(ownUser.User.Sid) {
		return ErrRefused
	}
	logon, err := logonSID(token)
	if err != nil {
		return err
	}
	ownLogon, err := logonSID(windows.GetCurrentProcessToken())
	if err != nil {
		return err
	}
	if logon != ownLogon {
		return ErrRefused
	}
	buffer := make([]uint16, 32768)
	size := uint32(len(buffer))
	if err := windows.QueryFullProcessImageName(process, 0, &buffer[0], &size); err != nil {
		return err
	}
	expected, err := filepath.Abs(executable)
	if err != nil {
		return err
	}
	actual := windows.UTF16ToString(buffer[:size])
	if !strings.EqualFold(filepath.Clean(actual), filepath.Clean(expected)) {
		return ErrRefused
	}
	return nil
}

// startDaemon launches only the paired executable; it never installs startup or Shell integration.
func startDaemon(profile, executable string) error {
	if !filepath.IsAbs(executable) {
		return ErrRefused
	}
	command := exec.Command(executable, "--profile", profile)
	command.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: windows.CREATE_NO_WINDOW}
	if err := command.Start(); err != nil {
		return err
	}
	return command.Process.Release()
}
