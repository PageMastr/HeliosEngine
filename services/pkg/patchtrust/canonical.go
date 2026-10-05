package patchtrust

import (
	"encoding/hex"
	"fmt"
	"strconv"
)

// The canonical JSON subset keysets and pointers use (engine/patch/README.md, "Canonical JSON"). Readers
// accept exactly one encoding of each document, so the bytes on the CDN, the bytes a signer signs and the
// bytes a verifier re-encodes are the same:
//
//   - no whitespace; one object, nothing after it;
//   - members in a fixed order (byte order of their keys), each exactly once; optional members absent;
//   - strings: printable ASCII 0x20..0x7E except '"' and '\' (no escapes);
//   - numbers: unsigned integers without leading zeros, at most MaxInt (2^53-1, exact in every JSON reader);
//   - true and false; no null, no fractions, no exponents.
//
// The readers below are schema-directed: they expect the literal member names in order, so there is no
// generic object model, no recursion and nothing to bound beyond the input size.

// MaxInt is the largest integer a document may hold.
const MaxInt = 1<<53 - 1

type reader struct {
	b   []byte
	pos int
	err error
}

func (r *reader) fail(format string, args ...any) {
	if r.err == nil {
		r.err = fmt.Errorf("at byte %d: %s", r.pos, fmt.Sprintf(format, args...))
	}
}

// lit consumes s or fails.
func (r *reader) lit(s string) {
	if r.err != nil {
		return
	}
	if len(r.b)-r.pos < len(s) || string(r.b[r.pos:r.pos+len(s)]) != s {
		r.fail("expected %q", s)
		return
	}
	r.pos += len(s)
}

// peek reports whether the input continues with s.
func (r *reader) peek(s string) bool {
	return r.err == nil && len(r.b)-r.pos >= len(s) && string(r.b[r.pos:r.pos+len(s)]) == s
}

// key consumes `"name":`, preceded by ',' unless first.
func (r *reader) key(name string, first bool) {
	if !first {
		r.lit(",")
	}
	r.lit(`"` + name + `":`)
}

func (r *reader) str(max int) string {
	r.lit(`"`)
	if r.err != nil {
		return ""
	}
	start := r.pos
	for r.pos < len(r.b) && r.b[r.pos] != '"' {
		c := r.b[r.pos]
		if c < 0x20 || c > 0x7E || c == '\\' {
			r.fail("byte 0x%02x is not allowed in a string", c)
			return ""
		}
		if r.pos-start >= max {
			r.fail("string longer than %d bytes", max)
			return ""
		}
		r.pos++
	}
	s := string(r.b[start:r.pos])
	r.lit(`"`)
	return s
}

func (r *reader) uint() uint64 {
	if r.err != nil {
		return 0
	}
	start := r.pos
	for r.pos < len(r.b) && r.b[r.pos] >= '0' && r.b[r.pos] <= '9' {
		if r.pos-start >= 16 {
			r.fail("integer above %d", MaxInt)
			return 0
		}
		r.pos++
	}
	digits := string(r.b[start:r.pos])
	switch {
	case digits == "":
		r.fail("expected an unsigned integer")
		return 0
	case len(digits) > 1 && digits[0] == '0':
		r.fail("integer with a leading zero")
		return 0
	}
	v, err := strconv.ParseUint(digits, 10, 64)
	if err != nil || v > MaxInt {
		r.fail("integer above %d", MaxInt)
		return 0
	}
	return v
}

func (r *reader) boolean() bool {
	switch {
	case r.peek("true"):
		r.pos += 4
		return true
	case r.peek("false"):
		r.pos += 5
		return false
	}
	r.fail("expected true or false")
	return false
}

// hexBytes reads a string of exactly 2*len(dst) lowercase hex digits into dst.
func (r *reader) hexBytes(dst []byte) {
	s := r.str(2 * len(dst))
	if r.err != nil {
		return
	}
	if len(s) != 2*len(dst) {
		r.fail("expected %d hex digits, got %d", 2*len(dst), len(s))
		return
	}
	for i := 0; i < len(s); i++ {
		if c := s[i]; !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			r.fail("%q is not lowercase hex", s)
			return
		}
	}
	_, _ = hex.Decode(dst, []byte(s))
}

// end checks that the input is used up.
func (r *reader) end() {
	if r.err == nil && r.pos != len(r.b) {
		r.fail("%d bytes after the document", len(r.b)-r.pos)
	}
}

type writer struct{ b []byte }

func (w *writer) raw(s string) { w.b = append(w.b, s...) }

func (w *writer) key(name string, first bool) {
	if !first {
		w.b = append(w.b, ',')
	}
	w.b = append(w.b, '"')
	w.b = append(w.b, name...)
	w.b = append(w.b, '"', ':')
}

// str writes s, which validation has restricted to the string alphabet.
func (w *writer) str(s string) {
	w.b = append(w.b, '"')
	w.b = append(w.b, s...)
	w.b = append(w.b, '"')
}

func (w *writer) uint(v uint64) { w.b = strconv.AppendUint(w.b, v, 10) }

func (w *writer) boolean(v bool) {
	if v {
		w.raw("true")
	} else {
		w.raw("false")
	}
}

func (w *writer) hexBytes(b []byte) {
	w.b = append(w.b, '"')
	w.b = hex.AppendEncode(w.b, b)
	w.b = append(w.b, '"')
}

// printable reports whether s fits the string alphabet.
func printable(s string) bool {
	for i := 0; i < len(s); i++ {
		if c := s[i]; c < 0x20 || c > 0x7E || c == '"' || c == '\\' {
			return false
		}
	}
	return true
}
