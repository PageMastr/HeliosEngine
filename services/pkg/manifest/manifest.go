// Package manifest reads and writes .hman v0 build manifests (05 §7, 08 §2.5–2.6): the files of one build
// of a product on one platform, their FastCDC chunks (BLAKE2b-256 IDs, package cdc), install tiers and
// tags, the pack index and patch-from deltas.
//
// The C++ reader and writer in engine/patch (helios::patch, manifest.h) produce and accept the same bytes;
// the shared vectors in services/testdata/vectors/hman/ pin both, and engine/patch/README.md is the
// specification. Layout, all integers little-endian:
//
//	[0, 352)    header: magic "HMAN", version 0, headerSize 352, flags, codec, sequence, createdAt,
//	            expiresAt, compatEpoch, bodySize, payloadSize, bodyHash, productId, platform, buildId,
//	            keyId, headerHash (BLAKE2b-256 of bytes [0, 256)), signature (bytes [288, 352))
//	[352, end)  payload: the body raw (codec 0) or as zstd frames (codec 1)
//	body        counts, then files[80 B], refs[16 B], chunks[56 B], packs[40 B], patches[80 B], paths
//
// The body is canonical (sorted, unique, every chunk and pack referenced, reserved bytes zero), so both
// languages' writers emit identical bodies; the zstd payload differs by encoder and bodyHash covers the
// decoded body. Part 2 of WP-0.16 signs bytes [0, 256) with Ed25519 into the signature field; v0 writes
// it as given (zeros by default) and does not interpret it.
//
// Hostile input: Parse checks the header hash, every field, the decoded body's size and hash, and every
// count, offset and size against the limits and each other; it returns an error wrapping ErrCorrupt,
// ErrVersion, ErrUnsupported or ErrLimit and never panics. A zstd payload decodes into a buffer that
// grows only as bytes decode, up to bodySize.
//
// Functions are safe for concurrent use; a Builder belongs to one goroutine.
package manifest

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"

	"github.com/klauspost/compress/zstd"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
)

// Format constants (engine/patch/include/helios/patch/manifest.h, namespace hman).
const (
	Magic          uint32 = 0x4E414D48 // "HMAN" as little-endian bytes
	Version        uint16 = 0
	HeaderSize            = 352
	SignedBytes           = 256 // headerHash and (part 2) the signature cover [0, 256)
	headerHashOff         = 256
	signatureOff          = 288
	SignatureSize         = 64
	KeyIDSize             = 16
	bodyHeaderSize        = 32
	fileEntrySize         = 80
	refEntrySize          = 16
	chunkEntrySize        = 56
	packEntrySize         = 40
	patchEntrySize        = 80
	productIDMax          = 32
	platformMax           = 32
	buildIDMax            = 64
	MaxBodySize    uint64 = 256 << 20
	MaxFiles              = 1 << 20
	MaxChunks             = 1 << 24
	MaxRefs               = 1 << 24
	MaxPacks              = 1 << 20
	MaxPatches            = 1 << 20
	MaxStringBytes        = 64 << 20
	MaxPathBytes          = 1024
	MaxPackSize    uint64 = 1 << 30
	MaxPatchSize   uint64 = 1 << 40
	MaxTier               = 2
	NoPack         uint32 = 0xFFFFFFFF // Chunk.Pack of a chunk stored loose on the CDN
	maxStoredSize         = cdc.MaxSize + 4096
	maxZstdWindow         = 1 << 25 // readers refuse frames needing a larger window (32 MiB)
)

// Codec says how the payload is stored.
type Codec uint8

const (
	CodecNone Codec = 0 // the body as is
	CodecZstd Codec = 1 // zstd frames that decode to the body
)

// FileFlags are a file's tags (08 §2.6; R05-P2-25). Unknown bits are rejected.
type FileFlags uint16

const (
	FlagOptional   FileFlags = 1 << 0 // optional content: not installed by default
	FlagVaulted    FileFlags = 1 << 1 // vaulted content: kept in the manifest, not installed or streamed
	FlagExecutable FileFlags = 1 << 2 // gets the executable bit on POSIX installs
	knownFlags               = FlagOptional | FlagVaulted | FlagExecutable
)

// Errors returned (wrapped) by Parse, Validate and the writers.
var (
	ErrCorrupt     = errors.New("manifest: corrupt")
	ErrVersion     = errors.New("manifest: unsupported version")
	ErrUnsupported = errors.New("manifest: unsupported feature")
	ErrLimit       = errors.New("manifest: limit exceeded")
	ErrInvalid     = errors.New("manifest: invalid")
)

// Header holds the header fields a manifest carries besides its tables.
type Header struct {
	ProductID   string // ^[a-z][a-z0-9-]{2,31}$ (08 §2.10.1)
	Platform    string // ^[a-z][a-z0-9_-]{1,31}$
	BuildID     string // ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$: the CDN path segment (05 §7)
	Sequence    uint64 // monotonic per product, channel and platform (anti-rollback)
	CreatedAt   uint64 // unix seconds
	ExpiresAt   uint64 // unix seconds; 0 = never, else after CreatedAt
	CompatEpoch uint32 // content compat epoch (05 §1.14.1)
	KeyID       [KeyIDSize]byte
	Signature   [SignatureSize]byte
}

// File is one installed file.
type File struct {
	Path     string // relative, '/'-separated, ASCII [A-Za-z0-9._+-] segments
	Size     uint64
	Hash     cdc.Hash // BLAKE2b-256 of the whole file
	FirstRef uint32   // its first entry in Manifest.Refs; its refs are contiguous and in file order
	RefCount uint32   // 0 exactly when Size is 0
	Group    uint64   // tier-2 streaming group (zone record hash; 0 = none)
	Language uint32   // 0 = neutral
	Tier     uint8    // 0 launcher/client/login area, 1 common, 2 streamable
	Flags    FileFlags
}

// ChunkRef is one chunk of a file, in file order.
type ChunkRef struct {
	Chunk  uint32 // index into Manifest.Chunks
	Offset uint64 // the sum of the previous chunks' sizes
}

// Chunk is a unique chunk.
type Chunk struct {
	Hash       cdc.Hash // the chunk ID
	RawSize    uint32   // 1..cdc.MaxSize
	StoredSize uint32   // bytes of its zstd object; 0 = not recorded (loose only)
	Pack       uint32   // index into Manifest.Packs, or NoPack
	PackOffset uint64   // 0 when loose
}

// Pack is a pack of small chunks (05 §7).
type Pack struct {
	Hash cdc.Hash
	Size uint64 // 1..MaxPackSize
}

// Patch is a zstd --patch-from delta from an older version of a file (05 §7).
type Patch struct {
	File      uint32 // index into Manifest.Files (the target)
	FromHash  cdc.Hash
	PatchHash cdc.Hash
	PatchSize uint64 // 1..MaxPatchSize
}

// Manifest is a decoded, canonical manifest. Validate states the invariants.
type Manifest struct {
	Header  Header
	Files   []File // sorted by path, unique ignoring ASCII case
	Refs    []ChunkRef
	Chunks  []Chunk // sorted by hash, unique
	Packs   []Pack  // sorted by hash, unique
	Patches []Patch // sorted by (File, FromHash), unique
}

// HeaderInfo is a validated fixed header.
type HeaderInfo struct {
	Header      Header
	Codec       Codec
	BodySize    uint64
	PayloadSize uint64
	BodyHash    cdc.Hash
	HeaderHash  cdc.Hash // the manifest's identity
}

func errorf(kind error, format string, args ...any) error {
	return fmt.Errorf("%w: %s", kind, fmt.Sprintf(format, args...))
}

func isLowerAlnum(c byte) bool { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') }
func isAlnum(c byte) bool      { return isLowerAlnum(c) || (c >= 'A' && c <= 'Z') }

func validProductID(s string) bool {
	if len(s) < 3 || len(s) > productIDMax || s[0] < 'a' || s[0] > 'z' {
		return false
	}
	for i := 1; i < len(s); i++ {
		if !isLowerAlnum(s[i]) && s[i] != '-' {
			return false
		}
	}
	return true
}

func validPlatform(s string) bool {
	if len(s) < 2 || len(s) > platformMax || s[0] < 'a' || s[0] > 'z' {
		return false
	}
	for i := 1; i < len(s); i++ {
		if !isLowerAlnum(s[i]) && s[i] != '-' && s[i] != '_' {
			return false
		}
	}
	return true
}

func validBuildID(s string) bool {
	if len(s) == 0 || len(s) > buildIDMax || !isAlnum(s[0]) {
		return false
	}
	for i := 1; i < len(s); i++ {
		if c := s[i]; !isAlnum(c) && c != '.' && c != '_' && c != '-' {
			return false
		}
	}
	return true
}

func asciiLower(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'A' && c <= 'Z' {
			b[i] = c - 'A' + 'a'
		}
	}
	return string(b)
}

// windowsDeviceName reports CON, PRN, AUX, NUL, COM0-9 and LPT0-9, with any extension.
func windowsDeviceName(seg string) bool {
	stem := seg
	if i := strings.IndexByte(seg, '.'); i >= 0 {
		stem = seg[:i]
	}
	low := asciiLower(stem)
	switch low {
	case "con", "prn", "aux", "nul":
		return true
	}
	return len(low) == 4 && (low[:3] == "com" || low[:3] == "lpt") && low[3] >= '0' && low[3] <= '9'
}

func validSegment(seg string) bool {
	if seg == "" || seg == "." || seg == ".." || seg[len(seg)-1] == '.' {
		return false
	}
	for i := 0; i < len(seg); i++ {
		if c := seg[i]; !isAlnum(c) && c != '.' && c != '_' && c != '-' && c != '+' {
			return false
		}
	}
	return !windowsDeviceName(seg)
}

// ValidPath reports whether path is a valid manifest path on its own (no ordering or collision checks):
// 1..1024 bytes of '/'-separated segments of [A-Za-z0-9._+-], none empty, "." or "..", none ending in '.',
// none a Windows device name.
func ValidPath(path string) bool {
	if len(path) == 0 || len(path) > MaxPathBytes {
		return false
	}
	for _, seg := range strings.Split(path, "/") {
		if !validSegment(seg) {
			return false
		}
	}
	return true
}

func bodySizeFor(files, refs, chunks, packs, patches, stringBytes uint64) uint64 {
	return bodyHeaderSize + files*fileEntrySize + refs*refEntrySize + chunks*chunkEntrySize +
		packs*packEntrySize + patches*patchEntrySize + stringBytes
}

func maxPayload(bodySize uint64) uint64 { return bodySize + bodySize/128 + 4096 }

func validateHeader(h *Header) error {
	switch {
	case !validProductID(h.ProductID):
		return errorf(ErrInvalid, "productId %q is not ^[a-z][a-z0-9-]{2,31}$", h.ProductID)
	case !validPlatform(h.Platform):
		return errorf(ErrInvalid, "platform %q is not ^[a-z][a-z0-9_-]{1,31}$", h.Platform)
	case !validBuildID(h.BuildID):
		return errorf(ErrInvalid, "buildId %q is not ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$", h.BuildID)
	case h.ExpiresAt != 0 && h.ExpiresAt <= h.CreatedAt:
		return errorf(ErrInvalid, "expiresAt %d is not after createdAt %d", h.ExpiresAt, h.CreatedAt)
	}
	return nil
}

func patchLess(a, b *Patch) bool {
	if a.File != b.File {
		return a.File < b.File
	}
	return bytes.Compare(a.FromHash[:], b.FromHash[:]) < 0
}

// Validate checks every invariant of the format (engine/patch/README.md "Validation"). It returns an error
// wrapping ErrLimit for a limit and ErrInvalid otherwise.
func (m *Manifest) Validate() error {
	if err := validateHeader(&m.Header); err != nil {
		return err
	}
	files, refs, chunks := uint64(len(m.Files)), uint64(len(m.Refs)), uint64(len(m.Chunks))
	packs, patches := uint64(len(m.Packs)), uint64(len(m.Patches))
	if files > MaxFiles || chunks > MaxChunks || refs > MaxRefs || packs > MaxPacks || patches > MaxPatches {
		return errorf(ErrLimit, "%d files, %d chunks, %d refs, %d packs, %d patches exceed the limits",
			files, chunks, refs, packs, patches)
	}
	var stringBytes uint64
	for i := range m.Files {
		stringBytes += uint64(len(m.Files[i].Path))
	}
	if stringBytes > MaxStringBytes {
		return errorf(ErrLimit, "%d path bytes exceed the limit", stringBytes)
	}
	if size := bodySizeFor(files, refs, chunks, packs, patches, stringBytes); size > MaxBodySize {
		return errorf(ErrLimit, "the body would be %d bytes, above %d", size, MaxBodySize)
	}

	lowered := make(map[string]struct{}, len(m.Files))
	chunkUsed := make([]bool, len(m.Chunks))
	var refAt uint64
	for i := range m.Files {
		f := &m.Files[i]
		if !ValidPath(f.Path) {
			return errorf(ErrInvalid, "file %d: %q is not a valid manifest path", i, f.Path)
		}
		if i > 0 && !(m.Files[i-1].Path < f.Path) {
			return errorf(ErrInvalid, "file %d: %q is not after %q (sorted, unique paths)", i, f.Path, m.Files[i-1].Path)
		}
		low := asciiLower(f.Path)
		if _, dup := lowered[low]; dup {
			return errorf(ErrInvalid, "file %d: %q differs from another path only in case", i, f.Path)
		}
		lowered[low] = struct{}{}
		if f.Tier > MaxTier {
			return errorf(ErrInvalid, "file %q: tier %d is not 0, 1 or 2", f.Path, f.Tier)
		}
		if f.Flags&^knownFlags != 0 {
			return errorf(ErrInvalid, "file %q: unknown flags %#x", f.Path, uint16(f.Flags))
		}
		if uint64(f.FirstRef) != refAt {
			return errorf(ErrInvalid, "file %q: first ref %d is not %d", f.Path, f.FirstRef, refAt)
		}
		if uint64(f.RefCount) > refs-refAt {
			return errorf(ErrInvalid, "file %q: %d refs run past the ref table", f.Path, f.RefCount)
		}
		var offset uint64
		for k := uint64(0); k < uint64(f.RefCount); k++ {
			r := m.Refs[refAt+k]
			if uint64(r.Chunk) >= chunks {
				return errorf(ErrInvalid, "file %q ref %d: chunk %d does not exist", f.Path, k, r.Chunk)
			}
			if r.Offset != offset {
				return errorf(ErrInvalid, "file %q ref %d: offset %d is not %d", f.Path, k, r.Offset, offset)
			}
			offset += uint64(m.Chunks[r.Chunk].RawSize)
			chunkUsed[r.Chunk] = true
		}
		if offset != f.Size {
			return errorf(ErrInvalid, "file %q: its chunks hold %d bytes, its size is %d", f.Path, offset, f.Size)
		}
		refAt += uint64(f.RefCount)
	}
	if refAt != refs {
		return errorf(ErrInvalid, "%d refs belong to no file", refs-refAt)
	}
	for i := range m.Files { // a file may not also be a directory of another file
		low := asciiLower(m.Files[i].Path)
		for at := 0; ; {
			j := strings.IndexByte(low[at:], '/')
			if j < 0 {
				break
			}
			dir := low[:at+j]
			if _, clash := lowered[dir]; clash {
				return errorf(ErrInvalid, "%q is a file and also a directory of %q", dir, m.Files[i].Path)
			}
			at += j + 1
		}
	}

	packUsed := make([]bool, len(m.Packs))
	for i := range m.Chunks {
		c := &m.Chunks[i]
		if i > 0 && bytes.Compare(m.Chunks[i-1].Hash[:], c.Hash[:]) >= 0 {
			return errorf(ErrInvalid, "chunk %d: IDs are not sorted and unique", i)
		}
		if c.RawSize == 0 || c.RawSize > cdc.MaxSize {
			return errorf(ErrInvalid, "chunk %d: raw size %d is out of range", i, c.RawSize)
		}
		if c.StoredSize > maxStoredSize {
			return errorf(ErrInvalid, "chunk %d: stored size %d is out of range", i, c.StoredSize)
		}
		if !chunkUsed[i] {
			return errorf(ErrInvalid, "chunk %s belongs to no file", c.Hash)
		}
		if c.Pack == NoPack {
			if c.PackOffset != 0 {
				return errorf(ErrInvalid, "chunk %d: a loose chunk has a pack offset", i)
			}
			continue
		}
		if uint64(c.Pack) >= packs {
			return errorf(ErrInvalid, "chunk %d: pack %d does not exist", i, c.Pack)
		}
		packSize := m.Packs[c.Pack].Size
		if c.StoredSize == 0 || c.PackOffset > packSize || uint64(c.StoredSize) > packSize-c.PackOffset {
			return errorf(ErrInvalid, "chunk %d: %d stored bytes at %d do not fit pack %d (%d bytes)",
				i, c.StoredSize, c.PackOffset, c.Pack, packSize)
		}
		packUsed[c.Pack] = true
	}
	for i := range m.Packs {
		p := &m.Packs[i]
		if i > 0 && bytes.Compare(m.Packs[i-1].Hash[:], p.Hash[:]) >= 0 {
			return errorf(ErrInvalid, "pack %d: IDs are not sorted and unique", i)
		}
		if p.Size == 0 || p.Size > MaxPackSize {
			return errorf(ErrInvalid, "pack %d: size %d is out of range", i, p.Size)
		}
		if !packUsed[i] {
			return errorf(ErrInvalid, "pack %s holds no chunk", p.Hash)
		}
	}
	for i := range m.Patches {
		q := &m.Patches[i]
		if uint64(q.File) >= files {
			return errorf(ErrInvalid, "patch %d: file %d does not exist", i, q.File)
		}
		if i > 0 && !patchLess(&m.Patches[i-1], q) {
			return errorf(ErrInvalid, "patch %d: not sorted by (file, fromHash) and unique", i)
		}
		if q.FromHash == m.Files[q.File].Hash {
			return errorf(ErrInvalid, "patch %d: it patches %q into itself", i, m.Files[q.File].Path)
		}
		if q.PatchSize == 0 || q.PatchSize > MaxPatchSize {
			return errorf(ErrInvalid, "patch %d: size %d is out of range", i, q.PatchSize)
		}
	}
	return nil
}

// FileRefs returns the chunk refs of the file at index i.
func (m *Manifest) FileRefs(i int) []ChunkRef {
	f := &m.Files[i]
	if uint64(f.FirstRef)+uint64(f.RefCount) > uint64(len(m.Refs)) {
		return nil
	}
	return m.Refs[f.FirstRef : f.FirstRef+f.RefCount]
}

// FindFile returns the index of the file with exactly this path, or -1.
func (m *Manifest) FindFile(path string) int {
	i := sort.Search(len(m.Files), func(i int) bool { return m.Files[i].Path >= path })
	if i < len(m.Files) && m.Files[i].Path == path {
		return i
	}
	return -1
}

// FindChunk returns the index of the chunk with this ID, or -1.
func (m *Manifest) FindChunk(h cdc.Hash) int {
	i := sort.Search(len(m.Chunks), func(i int) bool { return bytes.Compare(m.Chunks[i].Hash[:], h[:]) >= 0 })
	if i < len(m.Chunks) && m.Chunks[i].Hash == h {
		return i
	}
	return -1
}

// EncodeBody returns the canonical body of a valid manifest (what bodyHash covers).
func (m *Manifest) EncodeBody() ([]byte, error) {
	if err := m.Validate(); err != nil {
		return nil, err
	}
	var stringBytes uint64
	for i := range m.Files {
		stringBytes += uint64(len(m.Files[i].Path))
	}
	size := bodySizeFor(uint64(len(m.Files)), uint64(len(m.Refs)), uint64(len(m.Chunks)), uint64(len(m.Packs)),
		uint64(len(m.Patches)), stringBytes)
	b := make([]byte, size)
	le := binary.LittleEndian
	le.PutUint32(b[0:], uint32(len(m.Files)))
	le.PutUint32(b[4:], uint32(len(m.Chunks)))
	le.PutUint32(b[8:], uint32(len(m.Refs)))
	le.PutUint32(b[12:], uint32(len(m.Packs)))
	le.PutUint32(b[16:], uint32(len(m.Patches)))
	le.PutUint32(b[20:], uint32(stringBytes))
	e := b[bodyHeaderSize:]
	strs := b[size-stringBytes:]
	var pathAt uint32
	for i := range m.Files {
		f := &m.Files[i]
		le.PutUint32(e[0:], pathAt)
		le.PutUint32(e[4:], uint32(len(f.Path)))
		le.PutUint64(e[8:], f.Size)
		copy(e[16:48], f.Hash[:])
		le.PutUint32(e[48:], f.FirstRef)
		le.PutUint32(e[52:], f.RefCount)
		le.PutUint64(e[56:], f.Group)
		le.PutUint32(e[64:], f.Language)
		e[68] = f.Tier
		le.PutUint16(e[70:], uint16(f.Flags))
		copy(strs[pathAt:], f.Path)
		pathAt += uint32(len(f.Path))
		e = e[fileEntrySize:]
	}
	for _, r := range m.Refs {
		le.PutUint32(e[0:], r.Chunk)
		le.PutUint64(e[8:], r.Offset)
		e = e[refEntrySize:]
	}
	for i := range m.Chunks {
		c := &m.Chunks[i]
		copy(e[0:32], c.Hash[:])
		le.PutUint32(e[32:], c.RawSize)
		le.PutUint32(e[36:], c.StoredSize)
		le.PutUint32(e[40:], c.Pack)
		le.PutUint64(e[48:], c.PackOffset)
		e = e[chunkEntrySize:]
	}
	for _, p := range m.Packs {
		copy(e[0:32], p.Hash[:])
		le.PutUint64(e[32:], p.Size)
		e = e[packEntrySize:]
	}
	for i := range m.Patches {
		q := &m.Patches[i]
		le.PutUint32(e[0:], q.File)
		copy(e[8:40], q.FromHash[:])
		copy(e[40:72], q.PatchHash[:])
		le.PutUint64(e[72:], q.PatchSize)
		e = e[patchEntrySize:]
	}
	if uint64(len(e)) != stringBytes {
		return nil, errorf(ErrInvalid, "internal error: the body layout is off by %d bytes", int64(len(e))-int64(stringBytes))
	}
	return b, nil
}

// WriteOptions choose the payload codec. ZstdLevel is a zstd level, 1..19 (0 means 19).
type WriteOptions struct {
	Codec     Codec
	ZstdLevel int
}

func putID(dst []byte, s string) { copy(dst, s) }

// Marshal returns a complete .hman file for a valid manifest.
func (m *Manifest) Marshal(opts WriteOptions) ([]byte, error) {
	level := opts.ZstdLevel
	if level == 0 {
		level = 19
	}
	if opts.Codec != CodecNone && opts.Codec != CodecZstd {
		return nil, errorf(ErrInvalid, "unknown codec %d", opts.Codec)
	}
	if opts.Codec == CodecZstd && (level < 1 || level > 19) {
		return nil, errorf(ErrInvalid, "zstd level %d is not 1..19", level)
	}
	body, err := m.EncodeBody()
	if err != nil {
		return nil, err
	}
	payload := body
	if opts.Codec == CodecZstd {
		enc, err := zstd.NewWriter(nil, zstd.WithEncoderLevel(zstd.EncoderLevelFromZstd(level)),
			zstd.WithEncoderConcurrency(1), zstd.WithWindowSize(1<<22))
		if err != nil {
			return nil, err
		}
		payload = enc.EncodeAll(body, nil)
		_ = enc.Close()
	}
	out := make([]byte, HeaderSize+len(payload))
	le := binary.LittleEndian
	le.PutUint32(out[0:], Magic)
	le.PutUint16(out[4:], Version)
	le.PutUint16(out[6:], HeaderSize)
	out[12] = byte(opts.Codec)
	le.PutUint64(out[16:], m.Header.Sequence)
	le.PutUint64(out[24:], m.Header.CreatedAt)
	le.PutUint64(out[32:], m.Header.ExpiresAt)
	le.PutUint32(out[40:], m.Header.CompatEpoch)
	le.PutUint64(out[48:], uint64(len(body)))
	le.PutUint64(out[56:], uint64(len(payload)))
	bodyHash := cdc.Sum(body)
	copy(out[64:96], bodyHash[:])
	putID(out[96:96+productIDMax], m.Header.ProductID)
	putID(out[128:128+platformMax], m.Header.Platform)
	putID(out[160:160+buildIDMax], m.Header.BuildID)
	copy(out[224:240], m.Header.KeyID[:])
	headerHash := cdc.Sum(out[:SignedBytes])
	copy(out[headerHashOff:headerHashOff+32], headerHash[:])
	copy(out[signatureOff:signatureOff+SignatureSize], m.Header.Signature[:])
	copy(out[HeaderSize:], payload)
	return out, nil
}

func allZero(b []byte) bool {
	for _, c := range b {
		if c != 0 {
			return false
		}
	}
	return true
}

// getID reads a zero-padded identifier field: the bytes before the first zero, all later bytes zero.
func getID(field []byte) (string, bool) {
	end := bytes.IndexByte(field, 0)
	if end < 0 {
		end = len(field)
	}
	if !allZero(field[end:]) {
		return "", false
	}
	return string(field[:end]), true
}

// ParseHeader validates the header of a .hman file (its first 352 bytes and the payload size) without
// decoding the payload. maxBodySize 0 means MaxBodySize; larger values are capped at it.
func ParseHeader(file []byte, maxBodySize uint64) (HeaderInfo, error) {
	var info HeaderInfo
	if len(file) < HeaderSize {
		return info, errorf(ErrCorrupt, "%d bytes is shorter than the .hman header", len(file))
	}
	le := binary.LittleEndian
	h := file[:HeaderSize]
	if magic := le.Uint32(h[0:]); magic != Magic {
		return info, errorf(ErrCorrupt, "not a .hman file (magic %#08x)", magic)
	}
	if v := le.Uint16(h[4:]); v != Version {
		return info, errorf(ErrVersion, ".hman version %d (this reader: %d)", v, Version)
	}
	if hs := le.Uint16(h[6:]); hs != HeaderSize {
		return info, errorf(ErrCorrupt, "header size %d is not %d", hs, HeaderSize)
	}
	if cdc.Sum(h[:SignedBytes]) != cdc.Hash(h[headerHashOff:headerHashOff+32]) {
		return info, errorf(ErrCorrupt, "the header hash does not match")
	}
	if flags := le.Uint32(h[8:]); flags != 0 {
		return info, errorf(ErrUnsupported, "unknown header flags %#x", flags)
	}
	codec := h[12]
	if codec > byte(CodecZstd) {
		return info, errorf(ErrUnsupported, "unknown codec %d", codec)
	}
	if !allZero(h[13:16]) || le.Uint32(h[44:]) != 0 || !allZero(h[240:256]) {
		return info, errorf(ErrCorrupt, "reserved header bytes are not zero")
	}
	product, ok1 := getID(h[96 : 96+productIDMax])
	platform, ok2 := getID(h[128 : 128+platformMax])
	build, ok3 := getID(h[160 : 160+buildIDMax])
	if !ok1 || !ok2 || !ok3 {
		return info, errorf(ErrCorrupt, "an identifier field has bytes after its end")
	}
	info.Header = Header{ProductID: product, Platform: platform, BuildID: build,
		Sequence: le.Uint64(h[16:]), CreatedAt: le.Uint64(h[24:]), ExpiresAt: le.Uint64(h[32:]),
		CompatEpoch: le.Uint32(h[40:])}
	copy(info.Header.KeyID[:], h[224:240])
	copy(info.Header.Signature[:], h[signatureOff:signatureOff+SignatureSize])
	if err := validateHeader(&info.Header); err != nil {
		return info, errorf(ErrCorrupt, "%v", err)
	}
	maxBody := MaxBodySize
	if maxBodySize != 0 && maxBodySize < maxBody {
		maxBody = maxBodySize
	}
	bodySize, payloadSize := le.Uint64(h[48:]), le.Uint64(h[56:])
	if bodySize < bodyHeaderSize {
		return info, errorf(ErrCorrupt, "bodySize %d is shorter than the body header", bodySize)
	}
	if bodySize > maxBody {
		return info, errorf(ErrLimit, "bodySize %d is above %d", bodySize, maxBody)
	}
	if payloadSize != uint64(len(file)-HeaderSize) {
		return info, errorf(ErrCorrupt, "payloadSize %d but %d bytes follow the header", payloadSize, len(file)-HeaderSize)
	}
	if (codec == byte(CodecNone) && payloadSize != bodySize) ||
		(codec == byte(CodecZstd) && (payloadSize == 0 || payloadSize > maxPayload(bodySize))) {
		return info, errorf(ErrCorrupt, "payloadSize %d does not fit bodySize %d for codec %d", payloadSize, bodySize, codec)
	}
	info.Codec = Codec(codec)
	info.BodySize = bodySize
	info.PayloadSize = payloadSize
	copy(info.BodyHash[:], h[64:96])
	copy(info.HeaderHash[:], h[headerHashOff:headerHashOff+32])
	return info, nil
}

func decompress(payload []byte, bodySize uint64) ([]byte, error) {
	dec, err := zstd.NewReader(bytes.NewReader(payload), zstd.WithDecoderConcurrency(1),
		zstd.WithDecoderLowmem(true), zstd.WithDecoderMaxWindow(maxZstdWindow))
	if err != nil {
		return nil, errorf(ErrCorrupt, "zstd payload: %v", err)
	}
	defer dec.Close()
	// io.ReadAll grows only as bytes decode; one byte past bodySize catches a payload that decodes to more.
	out, err := io.ReadAll(io.LimitReader(dec, int64(bodySize)+1))
	if err != nil {
		return nil, errorf(ErrCorrupt, "zstd payload: %v", err)
	}
	if uint64(len(out)) != bodySize {
		return nil, errorf(ErrCorrupt, "the zstd payload decodes to %d bytes or more, bodySize is %d", len(out), bodySize)
	}
	return out, nil
}

// decodeBody decodes the tables (layout and reserved-field checks only; Validate does the rest).
func decodeBody(b []byte, m *Manifest) error {
	if len(b) < bodyHeaderSize {
		return errorf(ErrCorrupt, "the body is %d bytes, shorter than its header", len(b))
	}
	le := binary.LittleEndian
	fileCount, chunkCount, refCount := le.Uint32(b[0:]), le.Uint32(b[4:]), le.Uint32(b[8:])
	packCount, patchCount, stringBytes := le.Uint32(b[12:]), le.Uint32(b[16:]), le.Uint32(b[20:])
	if le.Uint64(b[24:]) != 0 {
		return errorf(ErrCorrupt, "reserved body-header bytes are not zero")
	}
	if fileCount > MaxFiles || chunkCount > MaxChunks || refCount > MaxRefs || packCount > MaxPacks ||
		patchCount > MaxPatches || stringBytes > MaxStringBytes {
		return errorf(ErrLimit, "counts %d files, %d chunks, %d refs, %d packs, %d patches, %d path bytes exceed the limits",
			fileCount, chunkCount, refCount, packCount, patchCount, stringBytes)
	}
	want := bodySizeFor(uint64(fileCount), uint64(refCount), uint64(chunkCount), uint64(packCount),
		uint64(patchCount), uint64(stringBytes))
	if want != uint64(len(b)) {
		return errorf(ErrCorrupt, "the body is %d bytes, its counts need %d", len(b), want)
	}
	e := b[bodyHeaderSize:]
	filesB := e[:uint64(fileCount)*fileEntrySize]
	e = e[len(filesB):]
	refsB := e[:uint64(refCount)*refEntrySize]
	e = e[len(refsB):]
	chunksB := e[:uint64(chunkCount)*chunkEntrySize]
	e = e[len(chunksB):]
	packsB := e[:uint64(packCount)*packEntrySize]
	e = e[len(packsB):]
	patchesB := e[:uint64(patchCount)*patchEntrySize]
	strs := e[len(patchesB):]

	m.Files = make([]File, fileCount)
	var pathAt uint64
	for i := range m.Files {
		x := filesB[i*fileEntrySize:]
		f := &m.Files[i]
		pathOffset, pathLength := le.Uint32(x[0:]), le.Uint32(x[4:])
		if uint64(pathOffset) != pathAt {
			return errorf(ErrCorrupt, "file %d: path offset %d is not %d (paths in file order)", i, pathOffset, pathAt)
		}
		if pathLength == 0 || pathLength > MaxPathBytes || uint64(pathLength) > uint64(stringBytes)-pathAt {
			return errorf(ErrCorrupt, "file %d: path length %d is out of range", i, pathLength)
		}
		f.Path = string(strs[pathAt : pathAt+uint64(pathLength)])
		pathAt += uint64(pathLength)
		f.Size = le.Uint64(x[8:])
		copy(f.Hash[:], x[16:48])
		f.FirstRef, f.RefCount = le.Uint32(x[48:]), le.Uint32(x[52:])
		f.Group, f.Language = le.Uint64(x[56:]), le.Uint32(x[64:])
		f.Tier, f.Flags = x[68], FileFlags(le.Uint16(x[70:]))
		if x[69] != 0 || le.Uint64(x[72:]) != 0 {
			return errorf(ErrCorrupt, "file %d: reserved bytes are not zero", i)
		}
	}
	if pathAt != uint64(stringBytes) {
		return errorf(ErrCorrupt, "%d path bytes are not any file's path", uint64(stringBytes)-pathAt)
	}
	m.Refs = make([]ChunkRef, refCount)
	for i := range m.Refs {
		x := refsB[i*refEntrySize:]
		m.Refs[i] = ChunkRef{Chunk: le.Uint32(x[0:]), Offset: le.Uint64(x[8:])}
		if le.Uint32(x[4:]) != 0 {
			return errorf(ErrCorrupt, "ref %d: reserved bytes are not zero", i)
		}
	}
	m.Chunks = make([]Chunk, chunkCount)
	for i := range m.Chunks {
		x := chunksB[i*chunkEntrySize:]
		c := &m.Chunks[i]
		copy(c.Hash[:], x[0:32])
		c.RawSize, c.StoredSize, c.Pack = le.Uint32(x[32:]), le.Uint32(x[36:]), le.Uint32(x[40:])
		c.PackOffset = le.Uint64(x[48:])
		if le.Uint32(x[44:]) != 0 {
			return errorf(ErrCorrupt, "chunk %d: reserved bytes are not zero", i)
		}
	}
	m.Packs = make([]Pack, packCount)
	for i := range m.Packs {
		x := packsB[i*packEntrySize:]
		copy(m.Packs[i].Hash[:], x[0:32])
		m.Packs[i].Size = le.Uint64(x[32:])
	}
	m.Patches = make([]Patch, patchCount)
	for i := range m.Patches {
		x := patchesB[i*patchEntrySize:]
		q := &m.Patches[i]
		q.File = le.Uint32(x[0:])
		if le.Uint32(x[4:]) != 0 {
			return errorf(ErrCorrupt, "patch %d: reserved bytes are not zero", i)
		}
		copy(q.FromHash[:], x[8:40])
		copy(q.PatchHash[:], x[40:72])
		q.PatchSize = le.Uint64(x[72:])
	}
	return nil
}

// Parse reads and validates a whole .hman file. maxBodySize 0 means MaxBodySize.
func Parse(file []byte, maxBodySize uint64) (*Manifest, error) {
	info, err := ParseHeader(file, maxBodySize)
	if err != nil {
		return nil, err
	}
	body := file[HeaderSize:]
	if info.Codec == CodecZstd {
		if body, err = decompress(body, info.BodySize); err != nil {
			return nil, err
		}
	}
	if cdc.Sum(body) != info.BodyHash {
		return nil, errorf(ErrCorrupt, "the body hash does not match")
	}
	m := &Manifest{Header: info.Header}
	if err := decodeBody(body, m); err != nil {
		return nil, err
	}
	if err := m.Validate(); err != nil {
		if errors.Is(err, ErrLimit) {
			return nil, err
		}
		return nil, fmt.Errorf("%w: %v", ErrCorrupt, err)
	}
	return m, nil
}
