package patchcdn

import (
	"context"
	"errors"
	"fmt"

	"github.com/PageMastr/scifi-test/services/pkg/manifest"
	"github.com/PageMastr/scifi-test/services/pkg/patchtrust"
)

// Verified is what Verify accepted.
type Verified struct {
	Keyset   *patchtrust.Keyset
	Pointer  *patchtrust.Pointer
	Manifest *manifest.Manifest
	State    patchtrust.State // the ratchets after accepting the keyset and pointer (saved to the store)
	Chunks   int              // chunk objects checked
	RawBytes uint64           // their decoded bytes
}

// Verify runs the client-side check of a channel (05 §7 "Launcher protocol" step 2, 08 §2.5 step 1) against
// a CDN, in order: the keyset against the root pair, the pointer against the keyset, the manifest named by
// the pointer, then every chunk the manifest lists. The ratchets are loaded from store and, once the
// manifest verifies, saved advanced (before the chunks, which an install fetches over hours). A rejection is
// a *patchtrust.Error naming the check; a missing keyset, pointer or manifest is a plain error wrapping
// ErrNotFound. Packed chunks are refused as unsupported (publish v0 stores every chunk loose).
func Verify(ctx context.Context, src Source, v *patchtrust.Verifier, now uint64,
	store patchtrust.StateStore) (*Verified, error) {
	st, err := store.Load()
	if err != nil {
		return nil, fmt.Errorf("patchcdn: load state: %w", err)
	}
	t := v.Target()
	b, err := src.Fetch(ctx, KeysetPath(t.ProductID), patchtrust.MaxKeysetSize)
	if err != nil {
		return nil, fmt.Errorf("patchcdn: keyset: %w", err)
	}
	out := &Verified{}
	if out.Keyset, err = v.VerifyKeyset(b, st); err != nil {
		return nil, err
	}
	if b, err = src.Fetch(ctx, PointerPath(t.ProductID, t.Channel, t.Platform), patchtrust.MaxPointerSize); err != nil {
		return nil, fmt.Errorf("patchcdn: pointer: %w", err)
	}
	if out.Pointer, err = v.VerifyPointer(b, out.Keyset, now, st); err != nil {
		return nil, err
	}
	p := out.Pointer
	if b, err = src.Fetch(ctx, ManifestPath(t.ProductID, p.BuildID, t.Platform), MaxManifestObject); err != nil {
		return nil, fmt.Errorf("patchcdn: manifest: %w", err)
	}
	if out.Manifest, err = v.VerifyManifest(b, out.Keyset, p.ManifestRef, now); err != nil {
		return nil, err
	}
	out.State = patchtrust.Advance(st, out.Keyset, p)
	if err := store.Save(out.State); err != nil {
		return nil, fmt.Errorf("patchcdn: save state: %w", err)
	}
	for _, c := range out.Manifest.Chunks {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if c.Pack != manifest.NoPack {
			return nil, fmt.Errorf("patchcdn: chunk %x is packed; packs are not supported in v0", c.Hash[:])
		}
		raw, err := FetchChunk(ctx, src, c)
		if err != nil {
			return nil, err
		}
		out.Chunks++
		out.RawBytes += uint64(len(raw))
	}
	return out, nil
}

// FetchChunk reads a loose chunk object and checks it against its manifest entry: chunk-missing if the
// object does not exist, chunk-corrupt if its size differs from a recorded storedSize or it does not decode
// to rawSize bytes, chunk-hash if the bytes do not hash to the ID.
func FetchChunk(ctx context.Context, src Source, c manifest.Chunk) ([]byte, error) {
	stored, err := src.Fetch(ctx, ChunkPath(c.Hash), MaxChunkObject)
	switch {
	case errors.Is(err, ErrNotFound):
		return nil, &patchtrust.Error{Check: patchtrust.CheckChunkMissing, Detail: err.Error()}
	case err != nil:
		return nil, fmt.Errorf("patchcdn: chunk %x: %w", c.Hash[:], err)
	case c.StoredSize != 0 && len(stored) != int(c.StoredSize):
		return nil, &patchtrust.Error{Check: patchtrust.CheckChunkCorrupt,
			Detail: fmt.Sprintf("chunk %x is stored in %d bytes, the manifest says %d", c.Hash[:], len(stored), c.StoredSize)}
	}
	raw, err := DecodeChunk(stored, c.RawSize)
	if err != nil {
		return nil, &patchtrust.Error{Check: patchtrust.CheckChunkCorrupt,
			Detail: fmt.Sprintf("chunk %x: %v", c.Hash[:], err)}
	}
	if err := patchtrust.VerifyChunk(c, raw); err != nil {
		return nil, err
	}
	return raw, nil
}

// MemoryStateStore keeps State in memory (tests, and a verify without a state file).
type MemoryStateStore struct{ State patchtrust.State }

func (m *MemoryStateStore) Load() (patchtrust.State, error) { return m.State, nil }
func (m *MemoryStateStore) Save(s patchtrust.State) error   { m.State = s; return nil }
