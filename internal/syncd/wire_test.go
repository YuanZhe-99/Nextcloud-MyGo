package syncd

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"io"
	"os"
	"strings"
	"testing"
)

// TestSharedWireFixtures enforces the same hostile-input boundary as the C++ gateway (§20 IPC).
func TestSharedWireFixtures(t *testing.T) {
	b, err := os.ReadFile("../../protocol/fixtures.json")
	if err != nil {
		t.Fatal(err)
	}
	var fixtures []struct {
		Name  string
		Valid bool
		JSON  string
	}
	if err := json.Unmarshal(b, &fixtures); err != nil {
		t.Fatal(err)
	}
	for _, f := range fixtures {
		t.Run(f.Name, func(t *testing.T) {
			if (validateJSON([]byte(f.JSON)) == nil) != f.Valid {
				t.Fatal("fixture acceptance mismatch")
			}
		})
	}
	for _, depth := range []int{64, 65} {
		b := []byte(strings.Repeat("[", depth) + "0" + strings.Repeat("]", depth))
		if (validateJSON(b) == nil) != (depth == 64) {
			t.Fatalf("depth %d", depth)
		}
	}
	if validateJSON([]byte{'[', '"', 0xff, '"', ']'}) == nil {
		t.Fatal("invalid UTF-8 accepted")
	}
}

// byteReader forces fragmented headers and payloads at the transport boundary.
type byteReader struct{ io.Reader }

// Read returns at most one byte to simulate a fragmented byte-stream pipe.
func (r byteReader) Read(p []byte) (int, error) {
	if len(p) > 1 {
		p = p[:1]
	}
	return r.Reader.Read(p)
}

// TestFramingBounds proves fragmentation, adjacent frames, truncation and allocation limits (§20 IPC).
func TestFramingBounds(t *testing.T) {
	var b bytes.Buffer
	for range 2 {
		if err := writeFrame(&b, map[string]any{"jsonrpc": "2.0", "id": "x", "result": true}); err != nil {
			t.Fatal(err)
		}
	}
	r := byteReader{&b}
	for range 2 {
		if _, err := readFrame(r); err != nil {
			t.Fatal(err)
		}
	}
	for _, n := range []uint32{0, maxFrame + 1, 20} {
		var h [4]byte
		binary.LittleEndian.PutUint32(h[:], n)
		if _, err := readFrame(bytes.NewReader(h[:])); err == nil {
			t.Fatalf("invalid length %d accepted", n)
		}
	}
}
