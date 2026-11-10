package syncd

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"math"
	"unicode/utf8"
)

const maxFrame = 1 << 20

// validateJSON checks the shared framing boundary before decoding an RPC message.
func validateJSON(b []byte) error {
	if !utf8.Valid(b) {
		return errors.New("invalid UTF-8")
	}
	d := json.NewDecoder(bytes.NewReader(b))
	if err := jsonValue(d, 0); err != nil {
		return err
	}
	if _, err := d.Token(); err != io.EOF {
		return errors.New("trailing JSON")
	}
	return nil
}

// jsonValue rejects duplicate keys, excessive depth and nonfinite numbers recursively.
func jsonValue(d *json.Decoder, depth int) error {
	t, err := d.Token()
	if err != nil {
		return err
	}
	if n, ok := t.(float64); ok && (math.IsNaN(n) || math.IsInf(n, 0)) {
		return errors.New("nonfinite number")
	}
	if delim, ok := t.(json.Delim); ok {
		if depth >= 64 {
			return errors.New("JSON depth exceeded")
		}
		keys := map[string]bool{}
		for d.More() {
			if delim == '{' {
				key, err := d.Token()
				if err != nil {
					return err
				}
				s, ok := key.(string)
				if !ok || keys[s] {
					return errors.New("duplicate JSON key")
				}
				keys[s] = true
			}
			if err := jsonValue(d, depth+1); err != nil {
				return err
			}
		}
		_, err = d.Token()
		return err
	}
	return nil
}

// readFrame bounds allocation and handles fragmented pipe reads.
func readFrame(r io.Reader) ([]byte, error) {
	var h [4]byte
	if _, err := io.ReadFull(r, h[:]); err != nil {
		return nil, err
	}
	n := binary.LittleEndian.Uint32(h[:])
	if n == 0 || n > maxFrame {
		return nil, errors.New("invalid frame length")
	}
	b := make([]byte, n)
	if _, err := io.ReadFull(r, b); err != nil {
		return nil, err
	}
	return b, validateJSON(b)
}

// writeFrame writes one bounded JSON message, including partial writer progress.
func writeFrame(w io.Writer, value any) error {
	b, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if len(b) == 0 || len(b) > maxFrame {
		return errors.New("invalid frame length")
	}
	frame := make([]byte, 4+len(b))
	binary.LittleEndian.PutUint32(frame, uint32(len(b)))
	copy(frame[4:], b)
	for len(frame) > 0 {
		n, err := w.Write(frame)
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		frame = frame[n:]
	}
	return nil
}
