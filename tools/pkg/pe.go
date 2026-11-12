package main

import (
	"bytes"
	"debug/pe"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// PE machine values (IMAGE_FILE_MACHINE_*).
const (
	machineI386    = 0x014c
	machineAMD64   = 0x8664
	machineARM64   = 0xaa64
	machineARM64EC = 0xa641 // object files only; images carry AMD64 plus hybrid metadata
)

// image is what the checker needs of one PE file.
type image struct {
	Machine uint16
	// Hybrid means the load configuration points at CHPE metadata: the image
	// carries ARM64EC code (an ARM64EC image, or an ARM64X image whose native
	// view is ARM64).
	Hybrid  bool
	Imports []string // DLL names, normal and delay-loaded, as written in the image
}

// describe names the code an image runs: ARM64, ARM64X, ARM64EC, x64 or x86.
func describe(img image) string {
	switch {
	case img.Machine == machineARM64 && img.Hybrid:
		return "ARM64X"
	case img.Machine == machineARM64:
		return "ARM64"
	case img.Machine == machineARM64EC, img.Machine == machineAMD64 && img.Hybrid:
		return "ARM64EC"
	case img.Machine == machineAMD64:
		return "x64"
	case img.Machine == machineI386:
		return "x86"
	}
	return fmt.Sprintf("machine %#04x", img.Machine)
}

// native reports whether the image at path runs as native code for arch.
// Returns: what the image is, and whether it passes.
// Notes: the one exception is the Visual C++ runtime, which Microsoft ships
// for ARM64 only as ARM64X: an ARM64 process maps its ARM64 view, never its
// ARM64EC code, so it runs natively. Every other ARM64X or ARM64EC image fails.
func native(path string, img image, arch string) (string, bool) {
	got := describe(img)
	return got, got == arch || arch == "ARM64" && got == "ARM64X" && mustShip(filepath.Base(path))
}

// inspect reads a file's PE machine, hybrid metadata and DLL imports.
// Returns: isPE=false with a nil error for a file that does not start with
// "MZ"; an error for an "MZ" file that is not a readable PE image.
func inspect(name string) (img image, isPE bool, err error) {
	f, err := os.Open(name)
	if err != nil {
		return img, false, err
	}
	defer f.Close()
	var mz [2]byte
	if _, err := io.ReadFull(f, mz[:]); err != nil || string(mz[:]) != "MZ" {
		return img, false, nil
	}
	p, err := pe.NewFile(f)
	if err != nil {
		return img, true, fmt.Errorf("%s: %w", name, err)
	}
	defer p.Close()
	img.Machine = p.FileHeader.Machine
	var dirs []pe.DataDirectory
	is64 := false
	switch oh := p.OptionalHeader.(type) {
	case *pe.OptionalHeader64:
		dirs, is64 = oh.DataDirectory[:min(oh.NumberOfRvaAndSizes, 16)], true
	case *pe.OptionalHeader32:
		dirs = oh.DataDirectory[:min(oh.NumberOfRvaAndSizes, 16)]
	default:
		return img, true, fmt.Errorf("%s: no optional header", name)
	}
	dir := func(i int) pe.DataDirectory {
		if i < len(dirs) {
			return dirs[i]
		}
		return pe.DataDirectory{}
	}
	// IMAGE_IMPORT_DESCRIPTOR is 20 bytes with the name RVA at 12;
	// ImgDelayDescr is 32 bytes with the name RVA at 4.
	for _, t := range []struct{ index, size, name int }{{pe.IMAGE_DIRECTORY_ENTRY_IMPORT, 20, 12}, {pe.IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, 32, 4}} {
		d := dir(t.index)
		if d.VirtualAddress == 0 {
			continue
		}
		table, err := at(p, d.VirtualAddress, max(d.Size, uint32(t.size)))
		if err != nil {
			return img, true, fmt.Errorf("%s: imports: %w", name, err)
		}
		for ; len(table) >= t.size; table = table[t.size:] {
			rva := binary.LittleEndian.Uint32(table[t.name:])
			if rva == 0 {
				break
			}
			s, err := at(p, rva, 260)
			if err != nil {
				return img, true, fmt.Errorf("%s: import name: %w", name, err)
			}
			if i := bytes.IndexByte(s, 0); i >= 0 {
				s = s[:i]
			}
			img.Imports = append(img.Imports, string(s))
		}
	}
	// CHPEMetadataPointer sits at offset 200 of IMAGE_LOAD_CONFIG_DIRECTORY64,
	// inside the structure when its Size field covers it.
	if d := dir(pe.IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG); is64 && d.VirtualAddress != 0 {
		lc, err := at(p, d.VirtualAddress, 208)
		if err != nil {
			return img, true, fmt.Errorf("%s: load configuration: %w", name, err)
		}
		if binary.LittleEndian.Uint32(lc) >= 208 {
			img.Hybrid = binary.LittleEndian.Uint64(lc[200:]) != 0
		}
	}
	return img, true, nil
}

// at reads n bytes of the image at a relative virtual address, zero-filled
// past the section's raw data as the loader maps it.
func at(p *pe.File, rva, n uint32) ([]byte, error) {
	for _, s := range p.Sections {
		size := max(s.VirtualSize, s.Size)
		if rva >= s.VirtualAddress && rva-s.VirtualAddress < size {
			b := make([]byte, n)
			if _, err := s.ReadAt(b, int64(rva-s.VirtualAddress)); err != nil && err != io.EOF {
				return nil, err
			}
			return b, nil
		}
	}
	return nil, fmt.Errorf("RVA %#x is outside every section", rva)
}

// mustShip reports whether a DLL is a Visual C++ runtime the package carries
// itself instead of relying on an installed redistributable.
func mustShip(dll string) bool {
	d := strings.ToLower(dll)
	for _, p := range []string{"msvcp140", "vcruntime140", "concrt140", "vccorlib140", "vcomp140"} {
		if strings.HasPrefix(d, p) {
			return true
		}
	}
	return false
}

// system reports whether Windows itself provides a DLL: an API set, or a
// file in System32 that is not a Visual C++ runtime.
func system(dll string) bool {
	d := strings.ToLower(dll)
	if strings.HasPrefix(d, "api-ms-") || strings.HasPrefix(d, "ext-ms-") {
		return true
	}
	if mustShip(d) || strings.ContainsAny(d, `/\`) {
		return false
	}
	_, err := os.Stat(filepath.Join(os.Getenv("SystemRoot"), "System32", d))
	return err == nil
}
