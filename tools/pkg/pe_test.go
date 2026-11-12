package main

import (
	"bytes"
	"debug/pe"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// fakePE writes a minimal PE32+ image: one section holding an import table
// for imports and a load configuration whose CHPE metadata pointer is set
// when hybrid is.
func fakePE(t *testing.T, path string, machine uint16, hybrid bool, imports ...string) {
	t.Helper()
	const rva, raw, size = 0x1000, 0x400, 0x400
	sec := make([]byte, size)
	for i, name := range imports { // descriptors at 0, names from 0x100
		at := 0x100 + i*0x40
		copy(sec[at:], name)
		binary.LittleEndian.PutUint32(sec[i*20+12:], uint32(rva+at))
		binary.LittleEndian.PutUint32(sec[i*20+16:], uint32(rva+0x300)) // FirstThunk
	}
	binary.LittleEndian.PutUint32(sec[0x200:], 0x140) // load config Size
	if hybrid {
		binary.LittleEndian.PutUint64(sec[0x200+200:], rva+0x380)
	}
	var b bytes.Buffer
	dos := make([]byte, 0x40)
	copy(dos, "MZ")
	binary.LittleEndian.PutUint32(dos[0x3c:], 0x40)
	b.Write(dos)
	b.WriteString("PE\x00\x00")
	binary.Write(&b, binary.LittleEndian, pe.FileHeader{Machine: machine, NumberOfSections: 1, SizeOfOptionalHeader: 240, Characteristics: 0x22})
	oh := pe.OptionalHeader64{Magic: 0x20b, SectionAlignment: 0x1000, FileAlignment: 0x200, SizeOfImage: 0x2000, SizeOfHeaders: raw, NumberOfRvaAndSizes: 16, Subsystem: 3}
	oh.DataDirectory[pe.IMAGE_DIRECTORY_ENTRY_IMPORT] = pe.DataDirectory{VirtualAddress: rva, Size: uint32(20 * (len(imports) + 1))}
	oh.DataDirectory[pe.IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG] = pe.DataDirectory{VirtualAddress: rva + 0x200, Size: 0x140}
	binary.Write(&b, binary.LittleEndian, oh)
	sh := pe.SectionHeader32{VirtualSize: size, VirtualAddress: rva, SizeOfRawData: size, PointerToRawData: raw, Characteristics: 0x40000040}
	copy(sh.Name[:], ".rdata")
	binary.Write(&b, binary.LittleEndian, sh)
	b.Write(make([]byte, raw-b.Len()))
	b.Write(sec)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, b.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
}

// TestDescribe covers §15.2 items 1–4: native ARM64 and x64 images pass, and
// hybrid ARM64X/ARM64EC images are told apart from them.
func TestDescribe(t *testing.T) {
	dir := t.TempDir()
	for _, c := range []struct {
		machine uint16
		hybrid  bool
		want    string
	}{
		{machineARM64, false, "ARM64"},
		{machineARM64, true, "ARM64X"},
		{machineAMD64, true, "ARM64EC"},
		{machineAMD64, false, "x64"},
	} {
		p := filepath.Join(dir, c.want+".dll")
		fakePE(t, p, c.machine, c.hybrid, "api-ms-win-core-synch-l1-1-0.dll", "Qt6Core.dll")
		img, ok, err := inspect(p)
		if err != nil || !ok {
			t.Fatalf("%s: inspect = %v, %v", c.want, ok, err)
		}
		if got := describe(img); got != c.want {
			t.Errorf("describe(%#x, hybrid=%v) = %s, want %s", c.machine, c.hybrid, got, c.want)
		}
		if strings.Join(img.Imports, ",") != "api-ms-win-core-synch-l1-1-0.dll,Qt6Core.dll" {
			t.Errorf("%s imports = %v", c.want, img.Imports)
		}
	}
	text := filepath.Join(dir, "readme.txt")
	os.WriteFile(text, []byte("not an image"), 0o644)
	if _, ok, err := inspect(text); ok || err != nil {
		t.Errorf("inspect(text) = %v, %v; want not a PE", ok, err)
	}
}

// TestCheck covers R20: a package passes only when every PE file is native
// for its architecture and every import resolves inside it or in Windows.
func TestCheck(t *testing.T) {
	sys := "api-ms-win-core-synch-l1-1-0.dll"
	for _, c := range []struct {
		name  string
		files map[string]uint16 // path -> machine; "hybrid:" prefix marks hybrid
		fail  []string
	}{
		{"native", map[string]uint16{"app.exe": machineARM64, "Qt6Core.dll": machineARM64, "plugins/tls/q.dll": machineARM64}, nil},
		{"x64 DLL", map[string]uint16{"app.exe": machineARM64, "Qt6Core.dll": machineAMD64, "plugins/tls/q.dll": machineARM64}, []string{"Qt6Core.dll is x64"}},
		{"ARM64X Visual C++ runtime", map[string]uint16{"app.exe": machineARM64, "Qt6Core.dll": machineARM64, "hybrid:MSVCP140.dll": machineARM64}, nil},
		{"ARM64X DLL", map[string]uint16{"app.exe": machineARM64, "hybrid:Qt6Core.dll": machineARM64, "plugins/tls/q.dll": machineARM64}, []string{"Qt6Core.dll is ARM64X"}},
		{"ARM64EC plugin", map[string]uint16{"app.exe": machineARM64, "Qt6Core.dll": machineARM64, "hybrid:plugins/tls/q.dll": machineAMD64}, []string{"plugins/tls/q.dll is ARM64EC"}},
		{"missing DLL", map[string]uint16{"app.exe": machineARM64, "plugins/tls/q.dll": machineARM64}, []string{"app.exe imports Qt6Core.dll", "plugins/tls/q.dll imports Qt6Core.dll"}},
	} {
		t.Run(c.name, func(t *testing.T) {
			dir := t.TempDir()
			for p, m := range c.files {
				hybrid := strings.HasPrefix(p, "hybrid:")
				p = strings.TrimPrefix(p, "hybrid:")
				var imports []string
				if !strings.HasSuffix(p, "Qt6Core.dll") {
					imports = []string{sys, "Qt6Core.dll"}
				}
				fakePE(t, filepath.Join(dir, filepath.FromSlash(p)), m, hybrid, imports...)
			}
			n, problems, err := check(dir, "ARM64")
			if err != nil {
				t.Fatal(err)
			}
			if n != len(c.files) {
				t.Errorf("checked %d PE files, want %d", n, len(c.files))
			}
			if len(problems) != len(c.fail) {
				t.Fatalf("problems = %q, want %q", problems, c.fail)
			}
			for i, want := range c.fail {
				if !strings.HasPrefix(problems[i], want) {
					t.Errorf("problem %d = %q, want prefix %q", i, problems[i], want)
				}
			}
		})
	}
}

// TestMustShip keeps the Visual C++ runtime inside the package even where an
// installed redistributable would satisfy the import.
func TestMustShip(t *testing.T) {
	for _, dll := range []string{"MSVCP140.dll", "msvcp140_1.dll", "VCRUNTIME140.dll", "vcruntime140_1.dll"} {
		if system(dll) {
			t.Errorf("system(%s) = true; the package must carry it", dll)
		}
	}
	if !system("api-ms-win-crt-runtime-l1-1-0.dll") {
		t.Error("API sets come from Windows")
	}
}
