package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"os"
	"slices"
	"strconv"
	"strings"
	"unicode/utf16"
)

// Resource types (RT_*) and the language every resource is filed under.
const (
	rtIcon     = 3
	rtVersion  = 16
	rtManifest = 24
	langEnUS   = 0x0409
)

// resourcesCommand writes the COFF object (.syso) that gives setup.exe or
// uninstall.exe its icon, version details and application manifest; `go
// build` links a .syso in the package directory.
// Inputs: -arch picks the object's machine; -version is the package version
// (X.Y.Z); -description is the FileDescription Explorer shows; -file is the
// OriginalFilename; -icon an .ico file.
// Side effects: writes -out.
func resourcesCommand(args []string) error {
	fl := flag.NewFlagSet("resources", flag.ContinueOnError)
	arch := fl.String("arch", "", "ARM64 or x64")
	ver := fl.String("version", "", "the package version, X.Y.Z")
	desc := fl.String("description", "", "the file description")
	file := fl.String("file", "", "the original file name")
	icon := fl.String("icon", "", "the .ico file")
	out := fl.String("out", "", "the .syso to write")
	if err := fl.Parse(args); err != nil {
		return err
	}
	if err := archFlag(*arch); err != nil {
		return err
	}
	if *desc == "" || *file == "" || *icon == "" || *out == "" {
		return errors.New("-description, -file, -icon and -out are required")
	}
	ico, err := os.ReadFile(*icon)
	if err != nil {
		return err
	}
	obj, err := resourceObject(*arch, *ver, *desc, *file, ico)
	if err != nil {
		return err
	}
	return os.WriteFile(*out, obj, 0o644)
}

// resource is one entry of the resource tree.
type resource struct {
	typ, id uint16
	data    []byte
}

// resourceObject builds the .syso: the icon images and their group, the
// version information and the manifest in one .rsrc section.
// Returns: an error for a version that is not X.Y.Z or an unreadable icon.
func resourceObject(arch, ver, desc, file string, ico []byte) ([]byte, error) {
	var v [3]uint16
	parts := strings.Split(ver, ".")
	if len(parts) != 3 {
		return nil, fmt.Errorf("version %q is not X.Y.Z", ver)
	}
	for i, p := range parts {
		n, err := strconv.ParseUint(p, 10, 16)
		if err != nil {
			return nil, fmt.Errorf("version %q is not X.Y.Z", ver)
		}
		v[i] = uint16(n)
	}
	res, err := iconResources(ico)
	if err != nil {
		return nil, err
	}
	res = append(res,
		resource{rtVersion, 1, versionInfo(v, desc, file)},
		resource{rtManifest, 1, []byte(manifestXML)})
	machine := map[string]uint16{"ARM64": machineARM64, "x64": machineAMD64}[arch]
	reloc := map[string]uint16{"ARM64": 2, "x64": 3}[arch] // IMAGE_REL_*_ADDR32NB
	section, relocs := rsrcSection(res)
	return coffObject(machine, reloc, section, relocs), nil
}

// iconResources splits an .ico file into its images (RT_ICON 1…n) and the
// group that lists them (RT_GROUP_ICON 1, the icon Explorer shows).
// Notes: an ICONDIRENTRY is 16 bytes ending in the image's file offset; the
// group's GRPICONDIRENTRY keeps the first 12 bytes and ends in the RT_ICON ID.
func iconResources(ico []byte) ([]resource, error) {
	if len(ico) < 6 || binary.LittleEndian.Uint16(ico[2:]) != 1 {
		return nil, errors.New("the icon is not an .ico file")
	}
	n := int(binary.LittleEndian.Uint16(ico[4:]))
	if n == 0 || len(ico) < 6+16*n {
		return nil, errors.New("the icon has no images")
	}
	group := slices.Clone(ico[:6])
	var out []resource
	for i := range n {
		e := ico[6+16*i : 22+16*i]
		size, off := binary.LittleEndian.Uint32(e[8:]), binary.LittleEndian.Uint32(e[12:])
		if uint64(off)+uint64(size) > uint64(len(ico)) {
			return nil, errors.New("the icon is truncated")
		}
		out = append(out, resource{rtIcon, uint16(i + 1), ico[off : off+size]})
		group = binary.LittleEndian.AppendUint16(append(group, e[:12]...), uint16(i+1))
	}
	return append(out, resource{rtGroupIcon, 1, group}), nil
}

// versionInfo builds the VS_VERSIONINFO resource Explorer's Details tab reads.
func versionInfo(v [3]uint16, desc, file string) []byte {
	ver := fmt.Sprintf("%d.%d.%d", v[0], v[1], v[2])
	fixed := make([]byte, 0, 52)
	for _, d := range []uint32{
		0xfeef04bd, 0x00010000, // signature, structure version
		uint32(v[0])<<16 | uint32(v[1]), uint32(v[2]) << 16, // file version
		uint32(v[0])<<16 | uint32(v[1]), uint32(v[2]) << 16, // product version
		0x3f, 0, // flags mask, flags
		0x00040004, 1, 0, // VOS_NT_WINDOWS32, VFT_APP, no subtype
		0, 0, // date
	} {
		fixed = binary.LittleEndian.AppendUint32(fixed, d)
	}
	var strs [][]byte
	for _, kv := range [][2]string{
		{"FileDescription", desc},
		{"FileVersion", ver},
		{"InternalName", strings.TrimSuffix(file, ".exe")},
		{"LegalCopyright", "Nextcloud MyGo contributors. Licensed under the GNU GPL-3.0."},
		{"OriginalFilename", file},
		{"ProductName", "Nextcloud MyGo"},
		{"ProductVersion", ver},
	} {
		value := utf16z(kv[1])
		strs = append(strs, versionNode(kv[0], 1, uint16(len(value)/2), value))
	}
	table := versionNode("040904B0", 1, 0, nil, strs...)
	translation := binary.LittleEndian.AppendUint16(binary.LittleEndian.AppendUint16(nil, langEnUS), 1200)
	return versionNode("VS_VERSION_INFO", 0, 52, fixed,
		versionNode("StringFileInfo", 1, 0, nil, table),
		versionNode("VarFileInfo", 1, 0, nil, versionNode("Translation", 0, 4, translation)))
}

// versionNode builds one node of a VS_VERSIONINFO tree: wLength,
// wValueLength, wType, the key, then the value and the children, each
// starting on a 32-bit boundary.
// Inputs: valueLen counts WCHARs for a text value (typ 1), bytes otherwise.
func versionNode(key string, typ, valueLen uint16, value []byte, children ...[]byte) []byte {
	b := make([]byte, 6)
	binary.LittleEndian.PutUint16(b[2:], valueLen)
	binary.LittleEndian.PutUint16(b[4:], typ)
	b = append(b, utf16z(key)...)
	b = pad(b, 4)
	b = append(b, value...)
	for _, c := range children {
		b = append(pad(b, 4), c...)
	}
	binary.LittleEndian.PutUint16(b, uint16(len(b)))
	return b
}

// rsrcSection lays out the resource tree (type, ID, language) followed by
// the data entries and the data.
// Returns: the section bytes and the offsets of each data entry's
// OffsetToData field, which the linker relocates to an RVA.
func rsrcSection(res []resource) ([]byte, []uint32) {
	slices.SortFunc(res, func(a, b resource) int {
		if a.typ != b.typ {
			return int(a.typ) - int(b.typ)
		}
		return int(a.id) - int(b.id)
	})
	var types []uint16
	for _, r := range res {
		if len(types) == 0 || types[len(types)-1] != r.typ {
			types = append(types, r.typ)
		}
	}
	dirSize := func(entries int) uint32 { return 16 + 8*uint32(entries) }
	// Root, one directory per type, one per resource (its language), then
	// the 16-byte data entries.
	off := dirSize(len(types))
	typeDir := map[uint16]uint32{}
	for _, t := range types {
		typeDir[t] = off
		off += dirSize(countType(res, t))
	}
	langDir := make([]uint32, len(res))
	for i := range res {
		langDir[i] = off
		off += dirSize(1)
	}
	entries := off
	off += 16 * uint32(len(res))
	dataAt := make([]uint32, len(res))
	for i, r := range res {
		off = (off + 7) &^ 7
		dataAt[i] = off
		off += uint32(len(r.data))
	}
	b := make([]byte, (off+7)&^7)
	put := func(at uint32, vals ...uint32) {
		for i, v := range vals {
			binary.LittleEndian.PutUint32(b[at+4*uint32(i):], v)
		}
	}
	dir := func(at uint32, n int) uint32 {
		binary.LittleEndian.PutUint16(b[at+14:], uint16(n)) // NumberOfIdEntries
		return at + 16
	}
	e := dir(0, len(types))
	for _, t := range types {
		put(e, uint32(t), typeDir[t]|1<<31)
		e += 8
	}
	var relocs []uint32
	for i, r := range res {
		if i == 0 || res[i-1].typ != r.typ {
			e = dir(typeDir[r.typ], countType(res, r.typ))
		}
		put(e, uint32(r.id), langDir[i]|1<<31)
		e += 8
		put(dir(langDir[i], 1), langEnUS, entries+16*uint32(i))
		put(entries+16*uint32(i), dataAt[i], uint32(len(r.data)))
		relocs = append(relocs, entries+16*uint32(i))
		copy(b[dataAt[i]:], r.data)
	}
	return b, relocs
}

// countType counts the resources of one type.
func countType(res []resource, t uint16) int {
	n := 0
	for _, r := range res {
		if r.typ == t {
			n++
		}
	}
	return n
}

// coffObject wraps a .rsrc section in a COFF object file with one section
// symbol, which each relocation (ADDR32NB, an image-relative address) names.
func coffObject(machine, relocType uint16, section []byte, relocs []uint32) []byte {
	const headers = 20 + 40
	relocAt := headers + uint32(len(section))
	symbols := relocAt + 10*uint32(len(relocs))
	var b bytes.Buffer
	w := func(vals ...any) {
		for _, v := range vals {
			binary.Write(&b, binary.LittleEndian, v)
		}
	}
	// IMAGE_FILE_HEADER: machine, one section, no timestamp, the symbol
	// table, one symbol, no optional header, no characteristics.
	w(machine, uint16(1), uint32(0), symbols, uint32(1), uint16(0), uint16(0))
	// IMAGE_SECTION_HEADER: initialized, readable data.
	w([8]byte{'.', 'r', 's', 'r', 'c'}, uint32(0), uint32(0), uint32(len(section)), uint32(headers),
		relocAt, uint32(0), uint16(len(relocs)), uint16(0), uint32(0x40000040))
	b.Write(section)
	for _, r := range relocs {
		w(r, uint32(0), relocType)
	}
	// The section symbol: value 0, section 1, no type, IMAGE_SYM_CLASS_STATIC.
	w([8]byte{'.', 'r', 's', 'r', 'c'}, uint32(0), int16(1), uint16(0), uint8(3), uint8(0))
	w(uint32(4)) // empty string table
	return b.Bytes()
}

// utf16z encodes a string as NUL-terminated UTF-16LE.
func utf16z(s string) []byte {
	var b []byte
	for _, u := range utf16.Encode([]rune(s + "\x00")) {
		b = binary.LittleEndian.AppendUint16(b, u)
	}
	return b
}

// pad appends zero bytes up to a multiple of n.
func pad(b []byte, n int) []byte {
	for len(b)%n != 0 {
		b = append(b, 0)
	}
	return b
}

// manifestXML is the application manifest of setup.exe and uninstall.exe:
// Common Controls v6 (themed controls and the marquee progress bar),
// per-monitor DPI awareness and asInvoker, so Windows never elevates them
// (its installer detection would otherwise ask for administrator rights for
// a program named setup).
const manifestXML = `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2, PerMonitor</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
`
