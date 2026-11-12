// Command pkg stages a Windows package of the UI and SyncD with only the DLLs
// they load, and checks that every PE file in a package is native code for
// its architecture (PLAN.md §15.2, §16.1).
//
//	go run ./tools/pkg stage -arch ARM64 -out <dir> -search <dir;dir> -plugins <Qt plugins> \
//	    -plugin tls/qopensslbackend.dll -file Nextcloud-MyGo.exe=<path> ...
//	go run ./tools/pkg check -arch ARM64 <dir or file>...
//
// It needs no server and no Windows APIs, so CI runs it anywhere.
package main

import (
	"archive/zip"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"

	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/release"
	"github.com/YuanZhe-99/Nextcloud-MyGo/internal/version"
)

// main dispatches the stage and check commands; a failed check exits 1.
func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: pkg stage|check -arch ARM64|x64 ...")
		os.Exit(2)
	}
	var err error
	switch os.Args[1] {
	case "stage":
		err = stageCommand(os.Args[2:])
	case "check":
		err = checkCommand(os.Args[2:])
	default:
		err = fmt.Errorf("unknown command %q", os.Args[1])
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "pkg:", err)
		os.Exit(1)
	}
}

// list collects a repeated flag.
type list []string

func (l *list) String() string     { return strings.Join(*l, ",") }
func (l *list) Set(v string) error { *l = append(*l, v); return nil }

// archFlag validates the -arch value.
func archFlag(arch string) error {
	if arch != "ARM64" && arch != "x64" {
		return fmt.Errorf("-arch must be ARM64 or x64, not %q", arch)
	}
	return nil
}

// checkCommand checks packages or single files and prints each problem.
func checkCommand(args []string) error {
	fl := flag.NewFlagSet("check", flag.ExitOnError)
	arch := fl.String("arch", "", "ARM64 or x64")
	fl.Parse(args)
	if err := archFlag(*arch); err != nil {
		return err
	}
	if fl.NArg() == 0 {
		return errors.New("check needs a package directory or file")
	}
	var problems []string
	files := 0
	for _, p := range fl.Args() {
		n, ps, err := check(p, *arch)
		if err != nil {
			return err
		}
		files += n
		problems = append(problems, ps...)
	}
	for _, p := range problems {
		fmt.Println("FAIL", p)
	}
	if len(problems) > 0 {
		return fmt.Errorf("%d problem(s) in %d PE file(s)", len(problems), files)
	}
	fmt.Printf("ok: %d PE file(s), all native %s (no ARM64EC or other foreign code), every import resolved\n", files, *arch)
	return nil
}

// check inspects every file under root (or root itself, when it is a file).
// Returns: the number of PE files and one line per problem: code that is not
// native for arch, or an import that neither the package nor Windows provides.
// Notes: a DLL a PE file imports resolves from the package root or the
// importing file's own directory, as the loader finds it there first.
func check(root, arch string) (int, []string, error) {
	st, err := os.Stat(root)
	if err != nil {
		return 0, nil, err
	}
	base := root
	if !st.IsDir() {
		base = filepath.Dir(root)
	}
	type entry struct {
		rel string
		img image
	}
	var pes []entry
	dlls := map[string]bool{} // slash path of each directory + "/" + lower-case DLL name
	walk := func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		img, ok, err := inspect(p)
		if err != nil {
			return err
		}
		if ok {
			rel, _ := filepath.Rel(base, p)
			rel = filepath.ToSlash(rel)
			pes = append(pes, entry{rel, img})
			dlls[strings.ToLower(rel)] = true
		}
		return nil
	}
	if st.IsDir() {
		err = filepath.WalkDir(root, walk)
	} else {
		err = walk(root, fs.FileInfoToDirEntry(st), nil)
	}
	if err != nil {
		return 0, nil, err
	}
	var problems []string
	for _, e := range pes {
		if got, ok := native(e.rel, e.img, arch); !ok {
			problems = append(problems, fmt.Sprintf("%s is %s, not %s", e.rel, got, arch))
		} else if got != arch {
			fmt.Printf("note: %s is %s (the Visual C++ runtime; ARM64 processes map its ARM64 code)\n", e.rel, got)
		}
		dir := strings.ToLower(filepath.ToSlash(filepath.Dir(e.rel)))
		for _, imp := range e.img.Imports {
			name := strings.ToLower(imp)
			if dlls[name] || dlls[dir+"/"+name] || system(name) {
				continue
			}
			problems = append(problems, fmt.Sprintf("%s imports %s, which neither the package nor Windows provides", e.rel, imp))
		}
	}
	return len(pes), problems, nil
}

// stageCommand copies the named files, the Qt plugins they need and the
// transitive DLL closure into a package directory with qt.conf and the
// manifest, then checks the result.
// Side effects: replaces -out.
func stageCommand(args []string) error {
	fl := flag.NewFlagSet("stage", flag.ExitOnError)
	arch := fl.String("arch", "", "ARM64 or x64")
	out := fl.String("out", "", "package directory to create (replaced)")
	search := fl.String("search", "", "semicolon-separated directories searched in order for imported DLLs")
	plugins := fl.String("plugins", "", "the target Qt's plugins directory")
	contract := fl.String("contract", "protocol/contract.json", "protocol manifest")
	control := fl.String("control", "backend/control.cpp", "source that sets the Control schema (PRAGMA user_version)")
	zipTo := fl.String("zip", "", "also write the checked package as this zip (the installer payload)")
	var files, plugs, runtimes list
	fl.Var(&files, "file", "dest=source: a file to package, by slash path in the package (repeatable)")
	fl.Var(&plugs, "plugin", "a Qt plugin, relative to -plugins (repeatable)")
	fl.Var(&runtimes, "runtime", "a DLL loaded at run time instead of imported, such as the libssl Qt's OpenSSL backend opens, found in -search (repeatable)")
	fl.Parse(args)
	if err := archFlag(*arch); err != nil {
		return err
	}
	if *out == "" || len(files) == 0 {
		return errors.New("stage needs -out and -file")
	}
	m := release.Manifest{
		Product: "Nextcloud-MyGo", Version: version.Version, Arch: *arch, TestBuild: true,
		UI:    release.Half{Executable: "Nextcloud-MyGo.exe"},
		SyncD: release.Half{Executable: "nextcloud-mygo-syncd.exe"},
	}
	var err error
	if m.UI.Protocol, err = protocol(*contract); err != nil {
		return err
	}
	m.SyncD.Protocol = m.UI.Protocol
	if m.SyncD.ControlSchema, err = controlSchema(*control); err != nil {
		return err
	}
	if err := os.RemoveAll(*out); err != nil {
		return err
	}
	var queue []string // package-relative slash paths of PE files to resolve
	add := func(dest, src string) error {
		if !release.Local(dest) {
			return fmt.Errorf("unsafe package path %q", dest)
		}
		if err := copyFile(src, filepath.Join(*out, filepath.FromSlash(dest))); err != nil {
			return err
		}
		if _, ok, err := inspect(src); err != nil {
			return err
		} else if ok {
			queue = append(queue, dest)
		}
		return nil
	}
	for _, f := range files {
		dest, src, ok := strings.Cut(f, "=")
		if !ok {
			return fmt.Errorf("-file %q is not dest=source", f)
		}
		if err := add(dest, src); err != nil {
			return err
		}
	}
	for _, p := range plugs {
		if err := add("plugins/"+filepath.ToSlash(p), filepath.Join(*plugins, p)); err != nil {
			return err
		}
	}
	dirs := strings.FieldsFunc(*search, func(r rune) bool { return r == ';' })
	find := func(name string) string {
		for _, d := range dirs {
			if _, err := os.Stat(filepath.Join(d, name)); err == nil {
				return filepath.Join(d, name)
			}
		}
		return ""
	}
	for _, name := range runtimes {
		src := find(name)
		if src == "" {
			return fmt.Errorf("run-time DLL %s is in no -search directory", name)
		}
		if err := add(name, src); err != nil {
			return err
		}
	}
	for len(queue) > 0 {
		rel := queue[0]
		queue = queue[1:]
		img, _, err := inspect(filepath.Join(*out, filepath.FromSlash(rel)))
		if err != nil {
			return err
		}
		if got, ok := native(rel, img, *arch); !ok {
			return fmt.Errorf("%s is %s, not %s", rel, got, *arch)
		}
		for _, imp := range img.Imports {
			if _, err := os.Stat(filepath.Join(*out, imp)); err == nil {
				continue
			}
			src := find(imp)
			switch {

			case src != "":
				fmt.Printf("  %s <- %s\n", imp, src)
				if err := add(imp, src); err != nil {
					return err
				}
			case !system(imp):
				return fmt.Errorf("%s imports %s, found in no -search directory", rel, imp)
			}
		}
	}
	// Qt resolves plugins inside the version directory only, never beside an
	// older version or from the parent of the install (PLAN.md §16.1).
	if err := os.WriteFile(filepath.Join(*out, "qt.conf"), []byte("[Paths]\r\nPrefix = .\r\nPlugins = plugins\r\n"), 0o644); err != nil {
		return err
	}
	err = filepath.WalkDir(*out, func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(*out, p)
		size, sum, err := release.Hash(p)
		m.Files = append(m.Files, release.File{Path: filepath.ToSlash(rel), Size: size, SHA256: sum})
		return err
	})
	if err != nil {
		return err
	}
	slices.SortFunc(m.Files, func(a, b release.File) int { return strings.Compare(a.Path, b.Path) })
	data, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(*out, release.ManifestName), append(data, '\n'), 0o644); err != nil {
		return err
	}
	fmt.Printf("staged %d files, protocol %d.%d, Control schema %d\n", len(m.Files), m.UI.Protocol.Major, m.UI.Protocol.Minor, m.SyncD.ControlSchema)
	if err := checkCommand([]string{"-arch", *arch, *out}); err != nil {
		return err
	}
	if *zipTo == "" {
		return nil
	}
	return writeZip(*out, *zipTo)
}

// writeZip archives dir's files under their slash paths, the installer's payload.
func writeZip(dir, name string) error {
	f, err := os.Create(name)
	if err != nil {
		return err
	}
	w := zip.NewWriter(f)
	err = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(dir, p)
		e, err := w.CreateHeader(&zip.FileHeader{Name: filepath.ToSlash(rel), Method: zip.Deflate})
		if err != nil {
			return err
		}
		in, err := os.Open(p)
		if err != nil {
			return err
		}
		defer in.Close()
		_, err = io.Copy(e, in)
		return err
	})
	if err == nil {
		err = w.Close()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return err
}

// protocol reads the protocol version both halves of this tree speak.
func protocol(path string) (release.Protocol, error) {
	var c struct {
		Protocol release.Protocol `json:"protocol"`
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return c.Protocol, err
	}
	if err := json.Unmarshal(data, &c); err != nil || c.Protocol.Major == 0 {
		return c.Protocol, fmt.Errorf("%s: no protocol version", path)
	}
	return c.Protocol, nil
}

// controlSchema reads the Control DB schema SyncD migrates to from its source.
// Notes: fails unless exactly one assignment exists, so a refactoring of the
// migration cannot silently record a stale schema. SyncD reporting it
// itself would replace this (doc/en-us/packaging.md).
func controlSchema(path string) (int, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	all := regexp.MustCompile(`PRAGMA user_version=(\d+)`).FindAllSubmatch(data, -1)
	if len(all) != 1 {
		return 0, fmt.Errorf("%s: %d Control schema assignments, want 1", path, len(all))
	}
	var n int
	fmt.Sscan(string(all[0][1]), &n)
	return n, nil
}

// copyFile copies one file, creating its directory.
func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}
