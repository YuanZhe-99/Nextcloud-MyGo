package gui

import (
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/syncd"
)

// refusingClient refuses to add one remote folder and records the others,
// checking that each local folder's parent exists by then.
type refusingClient struct {
	*syncd.Mock
	refuse string
	added  []string
	t      *testing.T
}

func (c *refusingClient) AddFolder(account, remote, local string, mode ...syncd.FolderMode) error {
	if _, err := os.Stat(filepath.Dir(local)); err != nil {
		c.t.Errorf("%s added before its parent existed: %v", remote, err)
	}
	if remote == c.refuse {
		return errors.New("refused")
	}
	c.added = append(c.added, remote)
	return nil
}

// TestAddFoldersCreatesBaseAndGoesOn: a base folder that does not exist yet
// is created (SyncD creates only the folder itself), and a folder that
// fails does not keep the ones after it from being added.
func TestAddFoldersCreatesBaseAndGoesOn(t *testing.T) {
	base := filepath.Join(t.TempDir(), "Nextcloud")
	c := &refusingClient{Mock: syncd.NewMock(false, nil), refuse: "/Photos", t: t}
	add := [][2]string{{"/Documents", filepath.Join(base, "Documents")}, {"/Photos", filepath.Join(base, "Photos")}, {"/Music", filepath.Join(base, "Music")}}
	err := addFolders(c, "account", add)
	var fe folderError
	if !errors.As(err, &fe) || !reflect.DeepEqual(fe.names, []string{"Photos"}) {
		t.Fatalf("addFolders = %v, want Photos named as failed", err)
	}
	if want := []string{"/Documents", "/Music"}; !reflect.DeepEqual(c.added, want) {
		t.Fatalf("added %v, want %v", c.added, want)
	}
	if fi, err := os.Stat(base); err != nil || !fi.IsDir() {
		t.Fatalf("base folder not created: %v", err)
	}
}

// TestHoldingFiles names only chosen folders that exist and hold something:
// those are merged with the server's, which setup says before going on.
func TestHoldingFiles(t *testing.T) {
	base := t.TempDir()
	for _, d := range []string{"Empty", "Full"} {
		if err := os.Mkdir(filepath.Join(base, d), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(base, "Full", "a.txt"), []byte("a"), 0o644); err != nil {
		t.Fatal(err)
	}
	add := [][2]string{{"/Missing", filepath.Join(base, "Missing")}, {"/Empty", filepath.Join(base, "Empty")}, {"/Full", filepath.Join(base, "Full")}}
	if got := holdingFiles(add); !reflect.DeepEqual(got, []string{"Full"}) {
		t.Fatalf("holdingFiles = %v, want [Full]", got)
	}
}
