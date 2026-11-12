// active_crash.go verifies owned process crashes during observed real content transfers, never from UI status alone.
package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
)

// exerciseActiveCrash kills only a child it started after observing active content bytes, then verifies same-journal recovery.
func exerciseActiveCrash(ctx context.Context, r *syncd.Remote, s state) {
	folders := r.Snapshot().Folders
	if len(folders) != 2 {
		panic("active crash requires the owned two-folder fixture")
	}
	journals := fixtureJournals(s.Profile)
	payload := make([]byte, 4*1024*1024)
	if _, err := rand.Read(payload); err != nil {
		panic("owned crash content generation failed")
	}
	digest := sha256.Sum256(payload)
	home := fixtureHome()
	remote := home + folders[0].RemotePath[1:] + "/"
	retryCommand(ctx, r.PauseAll)
	retryCommand(ctx, r.Shutdown)
	await(ctx, func() bool { return !r.Snapshot().Daemon.Connected }, "active crash initial daemon did not stop")
	start := func() *exec.Cmd {
		cmd := exec.Command(os.Getenv("SYNCD_TEST_EXE"), "--profile", s.Profile)
		if cmd.Start() != nil {
			panic("owned active-transfer daemon launch failed")
		}
		await(ctx, func() bool { return r.Snapshot().Daemon.Connected }, "owned active-transfer daemon did not connect")
		return cmd
	}
	child := start()
	defer func() {
		if child != nil && child.ProcessState == nil {
			_ = child.Process.Release()
		}
	}()
	for _, direction := range []string{"upload", "download"} {
		baseline, err := r.Diagnostics(ctx)
		if err != nil {
			panic("active-transfer baseline diagnostics unavailable")
		}
		var uploadBefore, downloadBefore uint64
		for _, f := range baseline.Folders {
			if f.FolderID == folders[0].ID {
				uploadBefore = f.UploadBytes
				downloadBefore = f.DownloadBytes
			}
		}
		name := "active-" + direction + ".bin"
		if direction == "upload" {
			if os.WriteFile(filepath.Join(folders[0].LocalPath, name), payload, 0600) != nil {
				panic("owned active upload write failed")
			}
		} else {
			fixtureDAV("PUT", remote+name, payload, 201, 204)
		}
		retryCommand(ctx, r.ResumeAll)
		retryCommand(ctx, func() error { return r.SyncNow(folders[0].ID) })
		observeContent(ctx, r, folders[0].ID, direction, uploadBefore, downloadBefore)
		if child.Process.Kill() != nil {
			panic("owned active-transfer process kill failed")
		}
		_ = child.Wait()
		await(ctx, func() bool { return !r.Snapshot().Daemon.Connected }, "active-transfer crash did not disconnect pipe")
		child = start()
		if direction == "upload" {
			await(ctx, func() bool {
				content := fixtureDAVLimit("GET", remote+name, nil, 5*1024*1024, 200, 404)
				return sha256.Sum256(content) == digest
			}, "interrupted upload did not recover matching remote content")
		} else {
			await(ctx, func() bool {
				content, err := os.ReadFile(filepath.Join(folders[0].LocalPath, name))
				return err == nil && sha256.Sum256(content) == digest
			}, "interrupted download did not recover matching local content")
		}
		await(ctx, func() bool {
			for _, f := range r.Snapshot().Folders {
				if f.State != syncd.FolderUpToDate {
					return false
				}
			}
			return true
		}, "active-transfer restart did not reconcile all mappings")
		after := fixtureJournals(s.Profile)
		for path, before := range journals {
			if current, ok := after[path]; !ok || !os.SameFile(before, current) {
				panic("active-transfer crash recreated a journal")
			}
		}
		retryCommand(ctx, r.PauseAll)
	}
	retryCommand(ctx, r.ResumeAll)
	fmt.Println("PASS owned process kills during actual positive-byte unfinished upload/download, same-journal rediscovery and matching recovered content")
}

// observeContent waits for real unfinished content bytes, never treating elapsed UI status as transfer evidence.
func observeContent(ctx context.Context, r *syncd.Remote, folder, direction string, uploadBefore, downloadBefore uint64) {
	observation, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	for observation.Err() == nil {
		d, err := r.Diagnostics(observation)
		if err != nil {
			panic("active-transfer diagnostics unavailable")
		}
		for _, f := range d.Folders {
			if f.FolderID == folder && ((direction == "upload" && f.ActiveUploads > 0 && f.UploadBytes >= uploadBefore+65536) || (direction == "download" && f.ActiveDownloads > 0 && f.DownloadBytes >= downloadBefore+65536)) {
				return
			}
		}
		select {
		case <-observation.Done():
		case <-time.After(5 * time.Millisecond):
		}
	}
	panic("real unfinished content transfer with positive bytes was not observed")
}

// exerciseActiveSwitch checks explicit cancellation followed by verified cross-origin recovery without changing journal identities (INV-02/05/08/09/13).
func exerciseActiveSwitch(ctx context.Context, r *syncd.Remote, s state, account string) {
	alternate := os.Getenv("NEXTCLOUD_TEST_ALTERNATE_URL")
	if alternate == "" {
		panic("cross-origin transfer acceptance requires two approved origins")
	}
	var staged syncd.Staging
	retryCommand(ctx, func() error {
		var err error
		staged, err = r.StageEndpoint(account, "Owned transfer alternate", alternate)
		return err
	})
	var target string
	retryCommand(ctx, func() error { var err error; target, err = r.ApproveEndpoint(staged.ID); return err })
	primary := ""
	for _, e := range r.Snapshot().Endpoints {
		if e.AccountID == account && e.Main {
			primary = e.ID
		}
	}
	if primary == "" {
		panic("cross-origin primary missing")
	}
	retryCommand(ctx, func() error { return r.SetPolicy(account, syncd.PolicyAuto, "") })
	folders := r.Snapshot().Folders
	journals := fixtureJournals(s.Profile)
	payload := make([]byte, 4*1024*1024)
	if _, err := rand.Read(payload); err != nil {
		panic("owned cross-origin content generation failed")
	}
	digest := sha256.Sum256(payload)
	remote := fixtureHome() + folders[0].RemotePath[1:] + "/"
	for _, direction := range []string{"upload", "download"} {
		retryCommand(ctx, r.PauseAll)
		baseline, err := r.Diagnostics(ctx)
		if err != nil {
			panic("cross-origin baseline diagnostics unavailable")
		}
		var uploadBefore, downloadBefore uint64
		for _, f := range baseline.Folders {
			if f.FolderID == folders[0].ID {
				uploadBefore = f.UploadBytes
				downloadBefore = f.DownloadBytes
			}
		}
		name := "cross-" + direction + ".bin"
		if direction == "upload" {
			if os.WriteFile(filepath.Join(folders[0].LocalPath, name), payload, 0600) != nil {
				panic("owned cross-origin upload write failed")
			}
		} else {
			fixtureDAV("PUT", remote+name, payload, 201, 204)
		}
		retryCommand(ctx, r.ResumeAll)
		retryCommand(ctx, func() error { return r.SyncNow(folders[0].ID) })
		observeContent(ctx, r, folders[0].ID, direction, uploadBefore, downloadBefore)
		retryCommand(ctx, r.PauseAll)
		var operation string
		retryCommand(ctx, func() error { var err error; operation, err = r.RequestSwitch(account, target); return err })
		await(ctx, func() bool {
			for _, e := range r.Snapshot().Endpoints {
				if e.ID == target {
					return e.InUse
				}
			}
			return false
		}, "cancelled transfer did not activate verified cross-origin binding")
		retryCommand(ctx, r.ResumeAll)
		if waitOperation(ctx, r, operation) != nil {
			panic("cross-origin interrupted transfer did not reconcile")
		}
		content, err := os.ReadFile(filepath.Join(folders[0].LocalPath, name))
		if err != nil || sha256.Sum256(content) != digest || sha256.Sum256(fixtureDAVLimit("GET", remote+name, nil, 5*1024*1024, 200)) != digest {
			panic("cross-origin recovered content hash mismatch")
		}
		after := fixtureJournals(s.Profile)
		for path, before := range journals {
			if current, ok := after[path]; !ok || !os.SameFile(before, current) {
				panic("cross-origin recovery recreated a journal")
			}
		}
		target = primary
	}
	retryCommand(ctx, func() error { return r.SetPolicy(account, syncd.PolicyFixed, primary) })
	var alternateID string
	for _, e := range r.Snapshot().Endpoints {
		if e.AccountID == account && !e.Main {
			alternateID = e.ID
		}
	}
	if alternateID == "" {
		panic("owned transfer alternate disappeared")
	}
	retryCommand(ctx, func() error { return r.RemoveEndpoint(alternateID) })
	fmt.Println("PASS actual unfinished upload/download cancellation, verified cross-origin recovery in both directions, matching hashes and original journals")
}
