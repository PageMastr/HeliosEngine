// Package patchcdn is the CDN side of the patch pipeline (05 §7): the directory layout, a read path
// (a directory or an HTTP base URL), chunk objects, the full client-side verification of a channel, and
// publishing a build directory to a local CDN directory with a signed manifest and pointer
// (`helios-patch publish`, services/cmd/helios-patch).
//
//	/chunks/<aa>/<bb>/<blake2b-hex>.zst                 immutable: a chunk, one zstd frame
//	/manifests/<product>/<build-id>/<platform>.hman     immutable: the signed .hman
//	/channels/<product>/<channel>/<platform>.json       mutable: the signed pointer
//	/keys/<product>/keyset.json                         mutable: the root-signed keyset
//
// Packs and patches (/packs, /patches) are not written or read in v0. engine/patch (cdn.h) reads the same
// layout. Functions are safe for concurrent use; one Publish at a time may write a CDN directory.
package patchcdn

import (
	"context"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/klauspost/compress/zstd"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
)

// MaxChunkObject bounds a stored chunk: zstd's bound for a MaxSize chunk (MaxSize + MaxSize/256) fits.
const MaxChunkObject = cdc.MaxSize + 4096

// MaxManifestObject bounds a .hman file: the header, the largest body and zstd's worst-case expansion.
const MaxManifestObject = int64(manifest.HeaderSize + manifest.MaxBodySize + manifest.MaxBodySize/128 + 4096)

// KeysetPath is the product's keyset.
func KeysetPath(product string) string { return "keys/" + product + "/keyset.json" }

// PointerPath is a channel's pointer for one platform.
func PointerPath(product, channel, platform string) string {
	return "channels/" + product + "/" + channel + "/" + platform + ".json"
}

// ManifestPath is a build's manifest for one platform.
func ManifestPath(product, build, platform string) string {
	return "manifests/" + product + "/" + build + "/" + platform + ".hman"
}

// ChunkPath is a chunk object: chunks/<aa>/<bb>/<64 hex digits>.zst.
func ChunkPath(h cdc.Hash) string {
	x := h.String()
	return "chunks/" + x[0:2] + "/" + x[2:4] + "/" + x + ".zst"
}

// ErrNotFound is returned (wrapped) by a Source for an object that does not exist.
var ErrNotFound = errors.New("patchcdn: not found")

// Source reads CDN objects by layout path. Fetch fails for objects larger than limit.
type Source interface {
	Fetch(ctx context.Context, path string, limit int64) ([]byte, error)
}

// checkPath accepts relative '/'-separated paths without empty, "." or ".." segments or backslashes.
func checkPath(path string) error {
	for _, seg := range strings.Split(path, "/") {
		if seg == "" || seg == "." || seg == ".." || strings.ContainsAny(seg, "\\:") {
			return fmt.Errorf("patchcdn: bad object path %q", path)
		}
	}
	return nil
}

// DirSource reads a CDN directory on disk.
type DirSource struct{ Root string }

// Fetch reads Root/path.
func (d DirSource) Fetch(_ context.Context, path string, limit int64) ([]byte, error) {
	if err := checkPath(path); err != nil {
		return nil, err
	}
	f, err := os.Open(filepath.Join(d.Root, filepath.FromSlash(path)))
	if errors.Is(err, fs.ErrNotExist) {
		return nil, fmt.Errorf("%w: %s", ErrNotFound, path)
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return readLimited(f, path, limit)
}

func readLimited(r io.Reader, path string, limit int64) ([]byte, error) {
	b, err := io.ReadAll(io.LimitReader(r, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > limit {
		return nil, fmt.Errorf("patchcdn: %s is larger than %d bytes", path, limit)
	}
	return b, nil
}

// HTTPSource reads a CDN over HTTP(S): Base + "/" + path.
type HTTPSource struct {
	Base   string       // e.g. https://cdn1.example; no trailing slash needed
	Client *http.Client // nil: a client with a 60 s timeout
}

// Fetch GETs the object; 404 is ErrNotFound, any other non-200 status an error.
func (h HTTPSource) Fetch(ctx context.Context, path string, limit int64) ([]byte, error) {
	if err := checkPath(path); err != nil {
		return nil, err
	}
	c := h.Client
	if c == nil {
		c = &http.Client{Timeout: 60 * time.Second}
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, strings.TrimSuffix(h.Base, "/")+"/"+path, nil)
	if err != nil {
		return nil, err
	}
	resp, err := c.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	switch {
	case resp.StatusCode == http.StatusNotFound:
		return nil, fmt.Errorf("%w: %s", ErrNotFound, path)
	case resp.StatusCode != http.StatusOK:
		return nil, fmt.Errorf("patchcdn: GET %s: %s", path, resp.Status)
	}
	return readLimited(resp.Body, path, limit)
}

// The chunk codec's encoder and decoder: EncodeAll and DecodeAll are safe for concurrent use, and creating a
// level-19 encoder per chunk would cost more than compressing a small chunk.
var (
	chunkEncoder = sync.OnceValues(func() (*zstd.Encoder, error) {
		return zstd.NewWriter(nil, zstd.WithEncoderLevel(zstd.SpeedBestCompression), zstd.WithEncoderConcurrency(1))
	})
	// MaxMemory bounds what DecodeAll produces (a chunk is at most cdc.MaxSize bytes) and, in this decoder,
	// also refuses a frame declaring a larger window; MaxWindow states that window bound outright, as
	// engine/patch's kMaxChunkWindowLog. EncodeAll declares a window of at most max(content size, 1 KiB) (a
	// single-segment frame from 256 bytes, a 1 KiB window below), so every chunk object it writes fits.
	chunkDecoder = sync.OnceValues(func() (*zstd.Decoder, error) {
		return zstd.NewReader(nil, zstd.WithDecoderConcurrency(0), zstd.WithDecoderMaxMemory(cdc.MaxSize),
			zstd.WithDecoderMaxWindow(cdc.MaxSize))
	})
)

// EncodeChunk stores a chunk: one zstd frame at level 19 (05 §7) with the content size and a checksum.
func EncodeChunk(raw []byte) ([]byte, error) {
	enc, err := chunkEncoder()
	if err != nil {
		return nil, err
	}
	return enc.EncodeAll(raw, make([]byte, 0, len(raw)/2+64)), nil
}

// DecodeChunk decodes a chunk object that must hold exactly rawSize bytes. Output and the zstd window are
// bounded by cdc.MaxSize, so a hostile object cannot make it allocate more.
func DecodeChunk(stored []byte, rawSize uint32) ([]byte, error) {
	dec, err := chunkDecoder()
	if err != nil {
		return nil, err
	}
	out, err := dec.DecodeAll(stored, make([]byte, 0, rawSize))
	switch {
	case err != nil:
		return nil, err
	case len(out) != int(rawSize):
		return nil, fmt.Errorf("decodes to %d bytes, expected %d", len(out), rawSize)
	}
	return out, nil
}
