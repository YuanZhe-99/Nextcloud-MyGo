package syncd

import (
	"bytes"
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"golang.org/x/sys/windows"
)

// awaitConnected waits for a real negotiated snapshot with a bounded test deadline.
func awaitConnected(t *testing.T, r *Remote, condition func(Snapshot) bool) {
	t.Helper()
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		s := r.Snapshot()
		if s.Daemon.Connected && condition(s) {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("real pipe did not reach the expected snapshot")
}

// TestRealPipeUnavailableStopsINV11INV12 exercises repeated corrupt-storage boots through the actual service entrypoint.
// Side effects: creates one unique disposable profile; no credentials, server requests or integration registration.
func TestRealPipeUnavailableStopsINV11INV12(t *testing.T) {
	exe := os.Getenv("SYNCD_TEST_EXE")
	if exe == "" {
		t.Skip("set SYNCD_TEST_EXE to the compiled native daemon")
	}
	exe, err := filepath.Abs(exe)
	if err != nil {
		t.Fatal(err)
	}
	profile := "ipc-unavailable-" + operationKey()
	hash := sha256.Sum256([]byte(profile))
	dir := filepath.Join(os.Getenv("LOCALAPPDATA"), "Nextcloud-MyGo", "syncd", fmt.Sprintf("%x", hash[:12]))
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := os.RemoveAll(dir); err != nil {
			t.Error(err)
		}
	})
	corrupt := []byte("owned corrupt Control database; preserve exactly")
	database := filepath.Join(dir, "control.sqlite")
	if err := os.WriteFile(database, corrupt, 0600); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	priorParams := map[string]any{"operationKey": operationKey(), "reason": "user"}
	var prior operation
	for boot := 0; boot < 2; boot++ {
		command := exec.CommandContext(ctx, exe, "--profile", profile)
		if err := command.Start(); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = command.Process.Kill(); _ = command.Wait() })
		r := NewRemote(ctx, profile, exe)
		t.Cleanup(r.Close)
		awaitTestSnapshot(t, r, func(s Snapshot) bool { return s.Daemon.StartupState == StartupDatabaseUnavailable })
		assertUnknownSnapshot(t, r.Snapshot(), StartupDatabaseUnavailable)
		diagnostics, err := r.Diagnostics(ctx)
		if err != nil || diagnostics.StateAvailable || diagnostics.StartupState != StartupDatabaseUnavailable || diagnostics.Folders != nil {
			t.Fatalf("unavailable real-pipe diagnostics: %+v %v", diagnostics, err)
		}
		if err := r.PauseAll(); !errors.Is(err, ErrStateUnavailable) {
			t.Fatalf("corrupt storage accepted application mutation: %v", err)
		}
		if boot != 0 {
			var historical operation
			if err := r.call(ctx, "daemon.shutdown", priorParams, &historical); err != nil || historical.ID != prior.ID || historical.State != "succeeded" {
				t.Fatalf("old shutdown lost its real-pipe receipt: %+v %v", historical, err)
			}
			if err := r.WaitOperation(ctx, prior.ID); err != nil {
				t.Fatal(err)
			}
			if _, err := r.Diagnostics(ctx); err != nil {
				t.Fatalf("historical retry stopped the new boot: %v", err)
			}
		}
		params := priorParams
		if boot != 0 {
			params = map[string]any{"operationKey": operationKey(), "reason": "user"}
		}
		var receipt operation
		if err := r.call(ctx, "daemon.shutdown", params, &receipt); err != nil || receipt.State != "succeeded" || receipt.ID == "" || (boot != 0 && receipt.ID == prior.ID) {
			t.Fatalf("diagnostic boot did not accept its fresh stop: %+v %v", receipt, err)
		}
		prior = receipt
		r.Close()
		if err := command.Wait(); err != nil {
			t.Fatal(err)
		}
		preserved, err := os.ReadFile(database)
		if err != nil || !bytes.Equal(preserved, corrupt) {
			t.Fatalf("diagnostic shutdown changed corrupt database: %v", err)
		}
	}
}

// TestRealPipeINV12INV13 exercises the compiled C++ daemon, reconnect, durable pause and explicit shutdown.
// Side effects: owns and removes only a uniquely named local test profile; no account or server writes.
func TestRealPipeINV12INV13(t *testing.T) {
	exe := os.Getenv("SYNCD_TEST_EXE")
	if exe == "" {
		t.Skip("set SYNCD_TEST_EXE to the compiled native daemon")
	}
	exe, err := filepath.Abs(exe)
	if err != nil {
		t.Fatal(err)
	}
	profile := "ipc-test-" + operationKey()
	hash := sha256.Sum256([]byte(profile))
	dir := filepath.Join(os.Getenv("LOCALAPPDATA"), "Nextcloud-MyGo", "syncd", fmt.Sprintf("%x", hash[:12]))
	t.Cleanup(func() {
		if err := os.RemoveAll(dir); err != nil {
			t.Error(err)
		}
	})
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	start := func() *exec.Cmd {
		cmd := exec.CommandContext(ctx, exe, "--profile", profile)
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = cmd.Process.Kill(); _ = cmd.Wait() })
		return cmd
	}
	command := start()
	r := NewRemote(ctx, profile, exe)
	t.Cleanup(r.Close)
	awaitConnected(t, r, func(Snapshot) bool { return true })
	// The server process must match the controlled executable, even within the same user/session.
	r.mu.Lock()
	pipe := windows.Handle(r.conn.(*os.File).Fd())
	r.mu.Unlock()
	if err := verifyPipeServer(pipe, filepath.Join(filepath.Dir(exe), "not-the-paired-daemon.exe")); err == nil {
		t.Fatal("wrong server executable accepted")
	}
	if r.Snapshot().Daemon.Demo || r.Snapshot().Daemon.Capabilities["fullSync"] {
		t.Fatal("daemon advertises unsupported workflow")
	}
	if server := os.Getenv("NEXTCLOUD_TEST_URL"); server != "" {
		login, err := r.StartLogin(server)
		if err != nil {
			t.Fatal("real login start failed", err)
		}
		if login.ID == "" || login.LoginURL == "" || !login.Expires.After(time.Now()) {
			t.Fatal("invalid login session result")
		}
		if err := r.CancelLogin(login.ID); err != nil {
			t.Fatal("real login cancellation failed", err)
		}
		if _, err := r.WaitLogin(ctx, login.ID); err == nil {
			t.Fatal("cancelled login returned an account")
		}
	}
	if err := r.PauseAll(); err != nil {
		t.Fatal(err)
	}
	awaitConnected(t, r, func(s Snapshot) bool { return s.Daemon.AllPaused })
	r.Close()
	// Closing the UI transport must leave the same daemon running (INV-11).
	r = NewRemote(ctx, profile, exe)
	t.Cleanup(r.Close)
	awaitConnected(t, r, func(s Snapshot) bool { return s.Daemon.AllPaused })
	if err := r.Shutdown(); err != nil {
		t.Fatal(err)
	}
	r.Close()
	if err := command.Wait(); err != nil {
		t.Fatal(err)
	}
	command = start()
	r = NewRemote(ctx, profile, exe)
	t.Cleanup(r.Close)
	awaitConnected(t, r, func(s Snapshot) bool { return s.Daemon.AllPaused })
	if err := r.ResumeAll(); err != nil {
		t.Fatal(err)
	}
	awaitConnected(t, r, func(s Snapshot) bool { return !s.Daemon.AllPaused })
	if err := r.Shutdown(); err != nil {
		t.Fatal(err)
	}
	r.Close()
	if err := command.Wait(); err != nil {
		t.Fatal(err)
	}
}
