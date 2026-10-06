package manifest

import (
	"bytes"
	"sort"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
)

// FileInput is what a Builder takes per file.
type FileInput struct {
	Path     string
	Content  cdc.File // from cdc.Split / cdc.ChunkReader: size, hash and chunks in order
	Tier     uint8
	Flags    FileFlags
	Group    uint64
	Language uint32
}

type placement struct {
	chunk, pack cdc.Hash
	offset      uint64
	storedSize  uint32
	packed      bool
}

type pendingPatch struct {
	path                string
	fromHash, patchHash cdc.Hash
	patchSize           uint64
}

// Builder builds a canonical Manifest from files in any order: it sorts files by path, stores each
// distinct chunk once (sorted by ID), lays out refs, and resolves pack placements and patches given by ID
// and path. The same inputs give the same manifest in any order, here and in C++
// (helios::patch::ManifestBuilder). A Builder is not safe for concurrent use.
type Builder struct {
	header     Header
	files      []FileInput
	packs      []Pack
	placements []placement
	patches    []pendingPatch
}

// NewBuilder starts a manifest with the given header.
func NewBuilder(h Header) *Builder { return &Builder{header: h} }

// AddFile adds a file. It fails (ErrInvalid) on an invalid path or tags, or chunks that do not tile
// Content.Size; a duplicate path fails in Build.
func (b *Builder) AddFile(f FileInput) error {
	if !ValidPath(f.Path) {
		return errorf(ErrInvalid, "%q is not a valid manifest path", f.Path)
	}
	if f.Tier > MaxTier {
		return errorf(ErrInvalid, "%q: tier %d is not 0, 1 or 2", f.Path, f.Tier)
	}
	if f.Flags&^knownFlags != 0 {
		return errorf(ErrInvalid, "%q: unknown flags %#x", f.Path, uint16(f.Flags))
	}
	var offset uint64
	for _, c := range f.Content.Chunks {
		if c.Offset != offset || c.Size == 0 || c.Size > cdc.MaxSize {
			return errorf(ErrInvalid, "%q: chunk at %d (%d bytes) does not continue at %d", f.Path, c.Offset, c.Size, offset)
		}
		offset += uint64(c.Size)
	}
	if offset != f.Content.Size {
		return errorf(ErrInvalid, "%q: chunks cover %d bytes of %d", f.Path, offset, f.Content.Size)
	}
	b.files = append(b.files, f)
	return nil
}

// AddPack declares a pack object.
func (b *Builder) AddPack(hash cdc.Hash, size uint64) {
	b.packs = append(b.packs, Pack{Hash: hash, Size: size})
}

// PlaceChunk records that chunk is stored in pack at offset (storedSize bytes); resolved in Build.
func (b *Builder) PlaceChunk(chunk, pack cdc.Hash, offset uint64, storedSize uint32) {
	b.placements = append(b.placements, placement{chunk: chunk, pack: pack, offset: offset, storedSize: storedSize, packed: true})
}

// SetStoredSize records a loose chunk's stored (zstd) size; resolved in Build.
func (b *Builder) SetStoredSize(chunk cdc.Hash, storedSize uint32) {
	b.placements = append(b.placements, placement{chunk: chunk, storedSize: storedSize})
}

// AddPatch adds a patch to the file at path; resolved in Build.
func (b *Builder) AddPatch(path string, fromHash, patchHash cdc.Hash, patchSize uint64) {
	b.patches = append(b.patches, pendingPatch{path: path, fromHash: fromHash, patchHash: patchHash, patchSize: patchSize})
}

func hashLess(a, b cdc.Hash) bool { return bytes.Compare(a[:], b[:]) < 0 }

// Build returns the canonical manifest, validated. It fails on a duplicate path, a chunk ID seen with two
// sizes, a placement or patch naming an unknown chunk, pack or path, or anything Validate refuses.
func (b *Builder) Build() (*Manifest, error) {
	m := &Manifest{Header: b.header}
	order := make([]*FileInput, len(b.files))
	for i := range b.files {
		order[i] = &b.files[i]
	}
	sort.SliceStable(order, func(i, j int) bool { return order[i].Path < order[j].Path })
	for i := 1; i < len(order); i++ {
		if order[i-1].Path == order[i].Path {
			return nil, errorf(ErrInvalid, "%q is added twice", order[i].Path)
		}
	}

	var all []Chunk
	for _, f := range order {
		for _, c := range f.Content.Chunks {
			all = append(all, Chunk{Hash: c.Hash, RawSize: c.Size, Pack: NoPack})
		}
	}
	sort.Slice(all, func(i, j int) bool {
		if all[i].Hash != all[j].Hash {
			return hashLess(all[i].Hash, all[j].Hash)
		}
		return all[i].RawSize < all[j].RawSize
	})
	for _, c := range all {
		if n := len(m.Chunks); n > 0 && m.Chunks[n-1].Hash == c.Hash {
			if m.Chunks[n-1].RawSize != c.RawSize {
				return nil, errorf(ErrInvalid, "chunk %s has sizes %d and %d", c.Hash, m.Chunks[n-1].RawSize, c.RawSize)
			}
			continue
		}
		m.Chunks = append(m.Chunks, c)
	}
	for _, f := range order {
		mf := File{Path: f.Path, Size: f.Content.Size, Hash: f.Content.Hash, FirstRef: uint32(min(len(m.Refs), MaxRefs)),
			RefCount: uint32(min(len(f.Content.Chunks), MaxRefs)), Group: f.Group, Language: f.Language,
			Tier: f.Tier, Flags: f.Flags}
		for _, c := range f.Content.Chunks {
			m.Refs = append(m.Refs, ChunkRef{Chunk: uint32(m.FindChunk(c.Hash)), Offset: c.Offset})
		}
		m.Files = append(m.Files, mf)
	}

	m.Packs = append([]Pack(nil), b.packs...)
	sort.Slice(m.Packs, func(i, j int) bool { return hashLess(m.Packs[i].Hash, m.Packs[j].Hash) })
	for i := 1; i < len(m.Packs); i++ {
		if m.Packs[i-1].Hash == m.Packs[i].Hash {
			return nil, errorf(ErrInvalid, "pack %s is added twice", m.Packs[i].Hash)
		}
	}
	placed := make([]bool, len(m.Chunks))
	for _, p := range b.placements {
		ci := m.FindChunk(p.chunk)
		if ci < 0 {
			return nil, errorf(ErrInvalid, "chunk %s is placed but belongs to no file", p.chunk)
		}
		if placed[ci] {
			return nil, errorf(ErrInvalid, "chunk %s is placed twice", p.chunk)
		}
		placed[ci] = true
		c := &m.Chunks[ci]
		c.StoredSize = p.storedSize
		if p.packed {
			pi := sort.Search(len(m.Packs), func(i int) bool { return !hashLess(m.Packs[i].Hash, p.pack) })
			if pi == len(m.Packs) || m.Packs[pi].Hash != p.pack {
				return nil, errorf(ErrInvalid, "pack %s is not declared", p.pack)
			}
			c.Pack = uint32(pi)
			c.PackOffset = p.offset
		}
	}
	for _, q := range b.patches {
		fi := m.FindFile(q.path)
		if fi < 0 {
			return nil, errorf(ErrInvalid, "a patch names %q, which is not a file", q.path)
		}
		m.Patches = append(m.Patches, Patch{File: uint32(fi), FromHash: q.fromHash, PatchHash: q.patchHash, PatchSize: q.patchSize})
	}
	sort.Slice(m.Patches, func(i, j int) bool { return patchLess(&m.Patches[i], &m.Patches[j]) })
	if err := m.Validate(); err != nil {
		return nil, err
	}
	return m, nil
}
