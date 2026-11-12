// reauth.go verifies real consent-based credential rotation without changing mapped identities.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
)

// exerciseReauthentication verifies cancellation, real credential rotation, native journal reuse and resumed transfers.
func exerciseReauthentication(ctx context.Context, r *syncd.Remote, account string, s state, file string) {
	profile := s.Profile
	folders := r.Snapshot().Folders
	journals := fixtureJournals(profile)
	if len(folders) != 2 {
		panic("renewal needs the owned two-folder fixture")
	}
	retryCommand(ctx, func() error { return r.PauseFolder(folders[0].ID) })
	retryCommand(ctx, r.PauseAll)
	await(ctx, func() bool {
		for _, f := range r.Snapshot().Folders {
			if f.State == syncd.FolderSyncing {
				return false
			}
		}
		return true
	}, "renewal fixture did not drain")
	var cancelled syncd.Login
	retryCommand(ctx, func() error { var err error; cancelled, err = r.Reauthenticate(account); return err })
	retryCommand(ctx, func() error { return r.CancelLogin(cancelled.ID) })
	if id, err := waitLogin(ctx, r, cancelled.ID); err == nil || id != "" {
		panic("cancelled renewal became successful")
	}
	if _, ok := r.Snapshot().Account(account); !ok {
		panic("renewal cancellation removed original account")
	}
	var login syncd.Login
	retryCommand(ctx, func() error { var err error; login, err = r.Reauthenticate(account); return err })
	s.Renewal = login.ID
	b, err := json.Marshal(s)
	if err != nil || os.WriteFile(file, b, 0600) != nil {
		panic("owned renewal cleanup identity could not be saved")
	}
	// The fixture parent consumes this one-time browser handoff only in memory, never in a saved diagnostic.
	fmt.Println(login.LoginURL)
	id, err := waitLogin(ctx, r, login.ID)
	if err != nil || id != account {
		fail(fmt.Sprintf("real renewal failed to preserve account or confirm prior-password revocation (%s, same account %v)", reasonWord(err), id == account))
	}
	if !r.Snapshot().Daemon.AllPaused {
		panic("renewal erased global pause")
	}
	for _, before := range folders {
		after, ok := r.Snapshot().Folder(before.ID)
		if !ok || after.AccountID != before.AccountID || after.LocalPath != before.LocalPath || after.RemotePath != before.RemotePath || after.State != syncd.FolderPaused {
			panic("renewal changed folder identity or resumed paused work")
		}
		if before.ID == folders[0].ID && !after.UserPaused {
			panic("renewal erased independent folder pause")
		}
	}
	retryCommand(ctx, r.ResumeAll)
	f, _ := r.Snapshot().Folder(folders[0].ID)
	if !f.UserPaused {
		panic("renewal resume all erased folder pause")
	}
	retryCommand(ctx, func() error { return r.ResumeFolder(folders[0].ID) })
	await(ctx, func() bool {
		for _, n := range r.Snapshot().Networks {
			if n.AccountID == account && n.Switching {
				return false
			}
		}
		for _, f := range r.Snapshot().Folders {
			if f.State != syncd.FolderUpToDate {
				return false
			}
		}
		return true
	}, "renewed credentials did not reconcile existing journals")
	for _, f := range folders {
		if os.WriteFile(filepath.Join(f.LocalPath, "renewed.txt"), []byte("renewed-owned"), 0600) != nil {
			panic("renewal upload fixture write failed")
		}
	}
	home := fixtureHome()
	await(ctx, func() bool {
		for _, f := range folders {
			if string(fixtureDAV("GET", home+f.RemotePath[1:]+"/renewed.txt", nil, 200, 404)) != "renewed-owned" {
				return false
			}
		}
		return true
	}, "new credential did not upload on existing mappings")
	retryCommand(ctx, r.PauseAll)
	retryCommand(ctx, r.Shutdown)
	await(ctx, func() bool { return !r.Snapshot().Daemon.Connected }, "renewed daemon did not stop")
	ownDaemon(profile)
	await(ctx, func() bool { return r.Snapshot().Daemon.Connected }, "renewed daemon did not reconnect")
	if restored, err := waitLogin(ctx, r, login.ID); err != nil || restored != account {
		panic("completed renewal session or installed credential did not survive restart")
	}
	for _, f := range folders {
		if os.WriteFile(filepath.Join(f.LocalPath, "renewed-after-restart.txt"), []byte("renewed-restart-owned"), 0600) != nil {
			panic("renewal restart fixture write failed")
		}
	}
	retryCommand(ctx, r.ResumeAll)
	await(ctx, func() bool {
		for _, f := range folders {
			if string(fixtureDAV("GET", home+f.RemotePath[1:]+"/renewed-after-restart.txt", nil, 200, 404)) != "renewed-restart-owned" {
				return false
			}
		}
		return true
	}, "restarted daemon could not use installed credentials")
	after := fixtureJournals(profile)
	for path, before := range journals {
		if current, ok := after[path]; !ok || !os.SameFile(before, current) {
			panic("renewal recreated an engine journal")
		}
	}
	fmt.Println("PASS real reauthentication consent, cancellation, unchanged account/folders/journals, pause preservation, old-password revocation and renewed uploads before/after restart")
}
