// Package cdc implements Helios's content-defined chunking: FastCDC with normalized chunking (level 2)
// and the parameters of 05 §7 (min 16 KiB, average 64 KiB, max 256 KiB), with BLAKE2b-256 chunk IDs.
//
// The C++ implementation in engine/patch (helios::patch::fastcdc) is the same algorithm. Both are pinned
// by the shared vectors in services/testdata/vectors/fastcdc.json, which go test and doctest both check, so
// the two never disagree on a chunk boundary or a chunk ID (08 §2.5). The definition, which the plan leaves
// open beyond the three sizes, is in engine/patch/README.md:
//
//   - gear table: the first 256 outputs of SplitMix64 seeded with GearSeed;
//   - rolling hash: fp = fp<<1 + gear[b] (mod 2^64), started at zero at the min-size cut-point skip;
//   - a cut follows the first byte at which fp&MaskS == 0 before AvgSize (18 bits: harder) or
//     fp&MaskL == 0 from AvgSize on (14 bits: easier); MaskS and MaskL are the top 18 and 14 bits;
//   - a chunk is at most MaxSize bytes; only a file's last chunk may be shorter than MinSize + 1.
//
// Boundaries depend only on the bytes from a chunk's start to at most MaxSize past it, so chunking a
// stream gives the same chunks as chunking the whole buffer.
//
// Everything here is safe for concurrent use except a Chunker, which belongs to one goroutine.
package cdc

import (
	"errors"
	"fmt"
	"hash"
	"io"

	"golang.org/x/crypto/blake2b"
)

// Chunk-size parameters (05 §7) and the normalized-chunking masks.
const (
	MinSize = 16 * 1024  // no cut before this many bytes (cut-point skipping)
	AvgSize = 64 * 1024  // the normal size: MaskS before it, MaskL from it on
	MaxSize = 256 * 1024 // a chunk never exceeds this

	MaskS uint64 = 0xFFFFC00000000000 // top 18 bits: log2(AvgSize) + 2
	MaskL uint64 = 0xFFFC000000000000 // top 14 bits: log2(AvgSize) - 2

	// GearSeed seeds the SplitMix64 stream whose first 256 outputs are the gear table ("FASTCDC" in ASCII).
	GearSeed uint64 = 0x0046415354434443

	// HashSize is the size of a chunk ID: BLAKE2b-256 (05 §7).
	HashSize = 32
)

// maxIdleReads bounds consecutive (0, nil) reads, as bufio does.
const maxIdleReads = 100

// Hash is a BLAKE2b-256 digest: a chunk ID, or a file hash.
type Hash [HashSize]byte

// String returns the lowercase hex form.
func (h Hash) String() string { return fmt.Sprintf("%x", h[:]) }

// Sum returns the BLAKE2b-256 of data.
func Sum(data []byte) Hash { return blake2b.Sum256(data) }

// SplitMix64 is the generator behind the gear table and the shared test vectors' inputs (Steele, Lea and
// Flood; the same as helios::SplitMix64 in engine/core).
type SplitMix64 struct{ State uint64 }

// Next returns the next output.
func (s *SplitMix64) Next() uint64 {
	s.State += 0x9E3779B97F4A7C15
	z := s.State
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
	z = (z ^ (z >> 27)) * 0x94D049BB133111EB
	return z ^ (z >> 31)
}

var gear = func() (g [256]uint64) {
	s := SplitMix64{State: GearSeed}
	for i := range g {
		g[i] = s.Next()
	}
	return g
}()

// Gear returns a copy of the gear table.
func Gear() [256]uint64 { return gear }

// Cut returns the length of the chunk that starts at data[0]. data must hold MaxSize bytes or run to the
// end of the input: the result is then the same as for the whole input. Cut(nil) is 0.
func Cut(data []byte) int {
	n := len(data)
	if n <= MinSize {
		return n
	}
	if n > MaxSize {
		n = MaxSize
	}
	normal := AvgSize
	if n < normal {
		normal = n
	}
	var fp uint64
	i := MinSize
	for ; i < normal; i++ {
		fp = fp<<1 + gear[data[i]]
		if fp&MaskS == 0 {
			return i + 1
		}
	}
	for ; i < n; i++ {
		fp = fp<<1 + gear[data[i]]
		if fp&MaskL == 0 {
			return i + 1
		}
	}
	return n
}

// Chunk is one chunk of a file: where it is, how long it is and its ID.
type Chunk struct {
	Offset uint64
	Size   uint32
	Hash   Hash
}

// Boundaries returns the chunk lengths of data, in order (none for empty data).
func Boundaries(data []byte) []int {
	var out []int
	for len(data) > 0 {
		n := Cut(data)
		out = append(out, n)
		data = data[n:]
	}
	return out
}

// Split chunks data and hashes every chunk.
func Split(data []byte) []Chunk {
	var out []Chunk
	var off uint64
	for len(data) > 0 {
		n := Cut(data)
		out = append(out, Chunk{Offset: off, Size: uint32(n), Hash: Sum(data[:n])})
		off += uint64(n)
		data = data[n:]
	}
	return out
}

// File is a whole input's chunking: its size, its BLAKE2b-256 and its chunks.
type File struct {
	Size   uint64
	Hash   Hash
	Chunks []Chunk
}

// Chunker splits a stream into chunks. It holds at most 2*MaxSize bytes and reads ahead at most MaxSize
// bytes past the chunk it returns. A Chunker is not safe for concurrent use.
type Chunker struct {
	r     io.Reader
	buf   []byte // buf[start:] is read but not yet returned
	start int
	off   uint64 // stream offset of buf[start]
	eof   bool
	err   error
	whole hash.Hash // BLAKE2b-256 of every byte returned so far
}

// NewChunker returns a Chunker that reads r.
func NewChunker(r io.Reader) *Chunker {
	h, _ := blake2b.New256(nil) // fails only for an over-long key
	return &Chunker{r: r, buf: make([]byte, 0, 2*MaxSize), whole: h}
}

// fill reads until MaxSize bytes are buffered past start or the stream ends. It may move the buffered
// bytes to the front, which invalidates the bytes Next returned last.
func (c *Chunker) fill() {
	idle := 0
	for !c.eof && c.err == nil && len(c.buf)-c.start < MaxSize {
		if len(c.buf) == cap(c.buf) {
			n := copy(c.buf, c.buf[c.start:])
			c.buf = c.buf[:n]
			c.start = 0
		}
		n, err := c.r.Read(c.buf[len(c.buf):cap(c.buf)])
		if n < 0 || n > cap(c.buf)-len(c.buf) {
			c.err = errors.New("reader returned an invalid count")
			return
		}
		c.buf = c.buf[:len(c.buf)+n]
		switch {
		case err == io.EOF:
			c.eof = true
		case err != nil:
			c.err = err
		case n == 0:
			if idle++; idle >= maxIdleReads {
				c.err = io.ErrNoProgress
			}
		default:
			idle = 0
		}
	}
}

// Next returns the next chunk and its bytes, which stay valid until the next call. At the end of the
// stream it returns io.EOF. A read error is returned wrapped, and again on every later call; the bytes
// read before it are not chunked.
func (c *Chunker) Next() (Chunk, []byte, error) {
	c.fill()
	if c.err != nil {
		return Chunk{}, nil, fmt.Errorf("cdc: read after offset %d: %w", c.off+uint64(len(c.buf)-c.start), c.err)
	}
	if c.start == len(c.buf) {
		return Chunk{}, nil, io.EOF
	}
	n := Cut(c.buf[c.start:])
	data := c.buf[c.start : c.start+n]
	ch := Chunk{Offset: c.off, Size: uint32(n), Hash: Sum(data)}
	_, _ = c.whole.Write(data)
	c.off += uint64(n)
	c.start += n
	return ch, data, nil
}

// ChunkReader chunks all of r and hashes the whole stream.
func ChunkReader(r io.Reader) (File, error) {
	c := NewChunker(r)
	var f File
	for {
		ch, _, err := c.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return File{}, err
		}
		f.Chunks = append(f.Chunks, ch)
	}
	f.Size = c.off
	copy(f.Hash[:], c.whole.Sum(nil))
	return f, nil
}
