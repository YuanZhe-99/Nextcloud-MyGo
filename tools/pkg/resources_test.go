package main

import (
	"bytes"
	"debug/pe"
	"encoding/binary"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

// TestResourceObject checks the .syso that gives setup.exe and uninstall.exe
// their icon, version details and manifest (packaging.md): a COFF object of
// the package's machine with one .rsrc section, the resource types Explorer
// and the loader read, one image-relative relocation per data entry, and the
// version and asInvoker manifest in the data.
func TestResourceObject(t *testing.T) {
	ico, err := os.ReadFile(filepath.Join("..", "..", "backend", "platform", "windows", "syncd.ico"))
	if err != nil {
		t.Fatal(err)
	}
	images := int(binary.LittleEndian.Uint16(ico[4:]))
	for _, c := range []struct {
		arch    string
		machine uint16
		reloc   uint16
	}{{"ARM64", machineARM64, 2}, {"x64", machineAMD64, 3}} {
		obj, err := resourceObject(c.arch, "1.2.3", "Nextcloud MyGo Setup", "setup.exe", ico)
		if err != nil {
			t.Fatal(err)
		}
		f, err := pe.NewFile(bytes.NewReader(obj))
		if err != nil {
			t.Fatalf("%s: %v", c.arch, err)
		}
		if f.Machine != c.machine || len(f.Sections) != 1 || f.Sections[0].Name != ".rsrc" || len(f.Symbols) != 1 {
			t.Fatalf("%s: machine %#x, sections %d, symbols %d", c.arch, f.Machine, len(f.Sections), len(f.Symbols))
		}
		s := f.Sections[0]
		data, err := s.Data()
		if err != nil {
			t.Fatal(err)
		}
		var types []uint32
		total := 0
		for i := range binary.LittleEndian.Uint16(data[14:]) {
			e := data[16+8*uint32(i):]
			types = append(types, binary.LittleEndian.Uint32(e))
			sub := binary.LittleEndian.Uint32(e[4:]) &^ (1 << 31)
			total += int(binary.LittleEndian.Uint16(data[sub+14:]))
		}
		if !slices.Equal(types, []uint32{rtIcon, rtGroupIcon, rtVersion, rtManifest}) || total != images+3 {
			t.Errorf("%s: types %v with %d resources, want %d", c.arch, types, total, images+3)
		}
		if len(s.Relocs) != total {
			t.Errorf("%s: %d relocations for %d resources", c.arch, len(s.Relocs), total)
		}
		for _, r := range s.Relocs {
			if r.Type != c.reloc || r.SymbolTableIndex != 0 {
				t.Errorf("%s: relocation %+v", c.arch, r)
			}
			if at := binary.LittleEndian.Uint32(data[r.VirtualAddress:]); at%8 != 0 || at >= uint32(len(data)) {
				t.Errorf("%s: data entry points at %#x", c.arch, at)
			}
		}
		for _, want := range [][]byte{utf16z("Nextcloud MyGo Setup"), utf16z("1.2.3"), []byte(`level="asInvoker"`)} {
			if !bytes.Contains(data, want) {
				t.Errorf("%s: section lacks %q", c.arch, want)
			}
		}
	}
	if _, err := resourceObject("x64", "1.2", "d", "f.exe", ico); err == nil {
		t.Error("accepted a version that is not X.Y.Z")
	}
	if _, err := resourceObject("x64", "1.2.3", "d", "f.exe", []byte("not an icon")); err == nil {
		t.Error("accepted a file that is not an .ico")
	}
}

// TestNeedsIcon names the package files the check requires an icon for.
func TestNeedsIcon(t *testing.T) {
	for name, want := range map[string]bool{
		syncdExe:                               true,
		"uninstall.exe":                        true,
		"Nextcloud-MyGo-0.1.3-arm64-setup.exe": true,
		"Nextcloud-MyGo-0.1.3-testversion-x64-setup.exe": true,
		"Nextcloud-MyGo.exe":                             false,
		"Qt6Core.dll":                                    false,
	} {
		if got := needsIcon(name); got != want {
			t.Errorf("needsIcon(%q) = %v", name, got)
		}
	}
}
