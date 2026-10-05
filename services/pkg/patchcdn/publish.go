package patchcdn

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"time"

	"golang.org/x/crypto/blake2b"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
	"github.com/PageMastr/scifi-test/services/pkg/patchtrust"
)

// PublishOptions describe one publish of a build directory to a channel.
type PublishOptions struct {
	CDNRoot     string            // the CDN directory (dev: helios-data/cdn, 05 §5)
	BuildDir    string            // the build to publish
	Target      patchtrust.Target // product, channel, platform
	BuildID     string            // "" derives one: 16 hex digits of BLAKE2b(body || compat epoch)
	CompatEpoch uint32
	MinLauncher string   // "" means "0"
	MinClient   string   // "" means "0"
	CDNHosts    []string // the pointer's cdn_hosts
	RolloutPct  uint8    // 0..100
	Tier0       []string // path prefixes of tier-0 files (08 §2.6); nil means {"bin/"}
	Now         time.Time
	Lifetime    time.Duration // the pointer's; 0 means patchtrust.MaxPointerLifetime (7 days)
}

// PublishResult reports what a publish wrote.
type PublishResult struct {
	BuildID         string
	ManifestHash    cdc.Hash
	Sequence        uint64 // the channel pointer's sequence after the publish
	Files           int
	Chunks          int   // distinct chunks in the build
	ChunksWritten   int   // chunk objects written (the others already existed)
	BytesWritten    int64 // bytes of chunk objects written
	ManifestWritten bool  // false: the build was already published with this content
	PointerWritten  bool  // false: the pointer already named this build with these fields and is fresh
}

func isLoopbackHost(h string) bool {
	return strings.HasPrefix(h, "http://localhost") || strings.HasPrefix(h, "http://127.0.0.1") ||
		strings.HasPrefix(h, "http://[::1]")
}

// Publish chunks BuildDir (FastCDC, package cdc), writes each new chunk object once (deduplicated by ID),
// writes the signed .hman for BuildID (unless the build is already published with the same content), the
// keyset, and last a signed pointer with the next sequence, so a reader never sees a pointer to objects that
// are not there. Republishing an identical build to the same channel writes nothing (the pointer is
// re-signed with the next sequence once it is past half its lifetime). Immutable objects are never
// overwritten: a build ID already published with other content is an error.
func Publish(ctx context.Context, o PublishOptions, keys *Keys) (*PublishResult, error) {
	t := o.Target
	if o.MinLauncher == "" {
		o.MinLauncher = "0"
	}
	if o.MinClient == "" {
		o.MinClient = "0"
	}
	if o.Tier0 == nil {
		o.Tier0 = []string{"bin/"}
	}
	if o.Lifetime == 0 {
		o.Lifetime = patchtrust.MaxPointerLifetime * time.Second
	}
	now := uint64(o.Now.Unix())
	switch {
	case o.Now.Unix() <= 0:
		return nil, errors.New("patchcdn: publish needs a clock")
	case o.Lifetime < time.Minute || o.Lifetime > patchtrust.MaxPointerLifetime*time.Second:
		return nil, fmt.Errorf("patchcdn: pointer lifetime %v is outside 1m..7d", o.Lifetime)
	case keys.Keyset.ProductID != t.ProductID:
		return nil, fmt.Errorf("patchcdn: the keys are %q's, not %q's", keys.Keyset.ProductID, t.ProductID)
	case keys.Dev && t.Channel != "dev":
		return nil, fmt.Errorf("patchcdn: dev keys sign only the dev channel, not %q", t.Channel)
	}
	if keys.Dev {
		for _, h := range o.CDNHosts {
			if !isLoopbackHost(h) {
				return nil, fmt.Errorf("patchcdn: dev keys sign only for loopback CDN hosts, not %q", h)
			}
		}
	}
	if key := keys.Keyset.Key(keys.KeyID); key == nil || now < key.NotBefore || now >= key.NotAfter {
		return nil, fmt.Errorf("patchcdn: manifest key %x may not sign at %d", keys.KeyID, now)
	}

	res := &PublishResult{}
	b := manifest.NewBuilder(manifest.Header{ProductID: t.ProductID, Platform: t.Platform, BuildID: "pending",
		CreatedAt: now, CompatEpoch: o.CompatEpoch, KeyID: keys.KeyID})
	if err := addBuild(ctx, o, b, res); err != nil {
		return nil, err
	}
	m, err := b.Build()
	if err != nil {
		return nil, fmt.Errorf("patchcdn: %w", err)
	}
	body, err := m.EncodeBody()
	if err != nil {
		return nil, err
	}
	if m.Header.BuildID = o.BuildID; o.BuildID == "" {
		sum := cdc.Sum(binary.LittleEndian.AppendUint32(slices.Clip(body), o.CompatEpoch))
		m.Header.BuildID = hex.EncodeToString(sum[:8])
	}
	res.BuildID, res.Files, res.Chunks = m.Header.BuildID, len(m.Files), len(m.Chunks)

	prev, err := readPointer(o.CDNRoot, t)
	if err != nil {
		return nil, err
	}
	seq := uint64(1)
	if prev != nil {
		seq = prev.Sequence + 1
	}
	mpath := filepath.Join(o.CDNRoot, filepath.FromSlash(ManifestPath(t.ProductID, m.Header.BuildID, t.Platform)))
	res.ManifestHash, res.ManifestWritten, err = writeManifest(mpath, m, body, seq, keys)
	if err != nil {
		return nil, err
	}

	kpath := filepath.Join(o.CDNRoot, filepath.FromSlash(KeysetPath(t.ProductID)))
	if old, err := os.ReadFile(kpath); err != nil || !bytes.Equal(old, keys.KeysetBytes) {
		if err := patchtrust.WriteFileAtomic(kpath, keys.KeysetBytes, 0o644); err != nil {
			return nil, err
		}
	}

	p := &patchtrust.Pointer{ProductID: t.ProductID, Channel: t.Channel, Platform: t.Platform,
		ManifestRef: patchtrust.ManifestRef{BuildID: m.Header.BuildID, ManifestHash: res.ManifestHash,
			CompatEpoch: o.CompatEpoch},
		MinLauncher: o.MinLauncher, MinClient: o.MinClient, CDNHosts: o.CDNHosts, RolloutPct: o.RolloutPct,
		SignedAt: now, Expires: now + uint64(o.Lifetime/time.Second), KeyID: keys.KeyID}
	if prev != nil && samePointer(prev, p) && prev.Expires > now && prev.Expires-now > uint64(o.Lifetime/time.Second)/2 {
		res.Sequence = prev.Sequence
		return res, nil
	}
	p.Sequence = seq
	if err := p.Sign(keys.Manifest); err != nil {
		return nil, err
	}
	pb, err := p.Marshal()
	if err != nil {
		return nil, err
	}
	ppath := filepath.Join(o.CDNRoot, filepath.FromSlash(PointerPath(t.ProductID, t.Channel, t.Platform)))
	if err := patchtrust.WriteFileAtomic(ppath, pb, 0o644); err != nil {
		return nil, err
	}
	res.Sequence, res.PointerWritten = seq, true
	return res, nil
}

// samePointer compares everything but the sequence, times and signature.
func samePointer(a, b *patchtrust.Pointer) bool {
	return a.ManifestRef == b.ManifestRef && a.MinLauncher == b.MinLauncher && a.MinClient == b.MinClient &&
		slices.Equal(a.CDNHosts, b.CDNHosts) && a.RolloutPct == b.RolloutPct && a.Next == nil && !a.Rollback &&
		a.KeyID == b.KeyID
}

// readPointer reads the channel's current pointer, if any. It does not verify it: the sequence is the
// publisher's own, from its own CDN directory.
func readPointer(root string, t patchtrust.Target) (*patchtrust.Pointer, error) {
	b, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(PointerPath(t.ProductID, t.Channel, t.Platform))))
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	p, err := patchtrust.ParsePointer(b)
	if err != nil {
		return nil, fmt.Errorf("patchcdn: the current pointer does not parse (%v); fix or remove it", err)
	}
	if p.ProductID != t.ProductID || p.Channel != t.Channel || p.Platform != t.Platform {
		return nil, errors.New("patchcdn: the current pointer names another product, channel or platform")
	}
	return p, nil
}

// writeManifest writes the signed manifest unless the build is already published with the same body and
// compat epoch, signed by a manifest key of the current keyset; then it returns the existing one's hash.
func writeManifest(path string, m *manifest.Manifest, body []byte, seq uint64, keys *Keys) (cdc.Hash, bool, error) {
	if old, err := os.ReadFile(path); err == nil {
		info, err := manifest.ParseHeader(old, 0)
		if err != nil {
			return cdc.Hash{}, false, fmt.Errorf("patchcdn: %s exists and does not read: %w", path, err)
		}
		if info.BodyHash != cdc.Sum(body) || info.Header.CompatEpoch != m.Header.CompatEpoch {
			return cdc.Hash{}, false, fmt.Errorf("patchcdn: build %s is already published with other content",
				m.Header.BuildID)
		}
		if key := keys.Keyset.Key(patchtrust.KeyID(info.Header.KeyID)); key == nil || key.Role != patchtrust.RoleManifest {
			return cdc.Hash{}, false, fmt.Errorf("patchcdn: build %s was signed by a key the keyset no longer "+
				"lists; publish it under a new build ID", m.Header.BuildID)
		}
		return info.HeaderHash, false, nil
	} else if !errors.Is(err, fs.ErrNotExist) {
		return cdc.Hash{}, false, err
	}
	m.Header.Sequence = seq
	file, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: 19})
	if err != nil {
		return cdc.Hash{}, false, err
	}
	if err := patchtrust.SignManifest(file, keys.Manifest); err != nil {
		return cdc.Hash{}, false, err
	}
	info, err := manifest.ParseHeader(file, 0)
	if err != nil {
		return cdc.Hash{}, false, err
	}
	return info.HeaderHash, true, patchtrust.WriteFileAtomic(path, file, 0o644)
}

// addBuild walks the build directory in path order, chunks every regular file, writes the chunk objects
// that do not exist yet and adds the files to the builder.
func addBuild(ctx context.Context, o PublishOptions, b *manifest.Builder, res *PublishResult) error {
	var paths []string
	err := filepath.WalkDir(o.BuildDir, func(p string, d fs.DirEntry, err error) error {
		switch {
		case err != nil:
			return err
		case d.IsDir():
			return nil
		case !d.Type().IsRegular():
			return fmt.Errorf("patchcdn: %s is not a regular file (links and devices are not published)", p)
		}
		paths = append(paths, p)
		return nil
	})
	if err != nil {
		return err
	}
	seen := map[cdc.Hash]bool{}
	for _, p := range paths {
		if err := ctx.Err(); err != nil {
			return err
		}
		rel, err := filepath.Rel(o.BuildDir, p)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		in, err := chunkFile(o.CDNRoot, p, seen, b, res)
		if err != nil {
			return fmt.Errorf("patchcdn: %s: %w", rel, err)
		}
		in.Path = rel
		in.Tier = 1
		for _, prefix := range o.Tier0 {
			if strings.HasPrefix(rel, prefix) {
				in.Tier = 0
			}
		}
		if st, err := os.Stat(p); err == nil && st.Mode().Perm()&0o111 != 0 {
			in.Flags |= manifest.FlagExecutable
		}
		if err := b.AddFile(in); err != nil {
			return fmt.Errorf("patchcdn: %w", err)
		}
	}
	return nil
}

func chunkFile(cdnRoot, path string, seen map[cdc.Hash]bool, b *manifest.Builder,
	res *PublishResult) (manifest.FileInput, error) {
	f, err := os.Open(path)
	if err != nil {
		return manifest.FileInput{}, err
	}
	defer f.Close()
	whole, _ := blake2b.New256(nil)
	c := cdc.NewChunker(f)
	var in manifest.FileInput
	for {
		ch, data, err := c.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return in, err
		}
		_, _ = whole.Write(data)
		in.Content.Chunks = append(in.Content.Chunks, ch)
		in.Content.Size += uint64(ch.Size)
		if seen[ch.Hash] {
			continue
		}
		seen[ch.Hash] = true
		obj := filepath.Join(cdnRoot, filepath.FromSlash(ChunkPath(ch.Hash)))
		if st, err := os.Stat(obj); err == nil {
			b.SetStoredSize(ch.Hash, uint32(min(st.Size(), MaxChunkObject)))
			continue
		}
		stored, err := EncodeChunk(data)
		if err != nil {
			return in, err
		}
		if err := patchtrust.WriteFileAtomic(obj, stored, 0o644); err != nil {
			return in, err
		}
		b.SetStoredSize(ch.Hash, uint32(len(stored)))
		res.ChunksWritten++
		res.BytesWritten += int64(len(stored))
	}
	copy(in.Content.Hash[:], whole.Sum(nil))
	return in, nil
}
