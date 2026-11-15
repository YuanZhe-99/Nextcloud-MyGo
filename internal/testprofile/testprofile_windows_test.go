package testprofile

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestHoldersNamesTheProcess checks the Restart Manager binding: a file this test process keeps open
// lists this executable, which is how Remove tells a daemon that was never stopped from a scanner.
func TestHoldersNamesTheProcess(t *testing.T) {
	path := filepath.Join(t.TempDir(), "control.sqlite")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	held, err := holders(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, h := range held {
		if strings.EqualFold(filepath.Clean(h), filepath.Clean(self)) {
			return
		}
	}
	t.Fatalf("holders of an open file %q do not include this process %s", held, self)
}
