package manifest_test

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/klauspost/compress/zstd"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/cdc/cdctest"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
)

// The shared vectors: doctest (engine/patch/tests/test_manifest.cpp) checks the same files.
var vectorsDir = filepath.Join("..", "..", "testdata", "vectors", "hman")

var update = flag.Bool("update", false, "rewrite pipeline.hman, pipeline.go-zstd.hman, deep-paths.hman and hostile.json")

type pipelineDesc struct {
	Header struct {
		ProductID   string `json:"productId"`
		Platform    string `json:"platform"`
		BuildID     string `json:"buildId"`
		Sequence    uint64 `json:"sequence"`
		CreatedAt   uint64 `json:"createdAt"`
		ExpiresAt   uint64 `json:"expiresAt"`
		CompatEpoch uint32 `json:"compatEpoch"`
		KeyID       string `json:"keyId"`
		Signature   string `json:"signature"`
	} `json:"header"`
	Files []struct {
		Path     string        `json:"path"`
		Input    cdctest.Input `json:"input"`
		Tier     uint8         `json:"tier"`
		Flags    uint16        `json:"flags"`
		Group    uint64        `json:"group"`
		Language uint32        `json:"language"`
	} `json:"files"`
	Packs []struct {
		Hash string `json:"hash"`
		Size uint64 `json:"size"`
	} `json:"packs"`
	Placements []struct {
		File       string `json:"file"`
		Chunk      int    `json:"chunk"`
		Pack       string `json:"pack"`
		Offset     uint64 `json:"offset"`
		StoredSize uint32 `json:"storedSize"`
	} `json:"placements"`
	StoredSizes []struct {
		File       string `json:"file"`
		Chunk      int    `json:"chunk"`
		StoredSize uint32 `json:"storedSize"`
	} `json:"storedSizes"`
	Patches []struct {
		Path      string `json:"path"`
		FromHash  string `json:"fromHash"`
		PatchHash string `json:"patchHash"`
		PatchSize uint64 `json:"patchSize"`
	} `json:"patches"`
}

func readJSON(t testing.TB, name string, v any) {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(vectorsDir, name))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(raw, v); err != nil {
		t.Fatalf("%s: %v", name, err)
	}
}

func mustHash(t testing.TB, s string) cdc.Hash {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil || len(b) != cdc.HashSize {
		t.Fatalf("bad hash %q", s)
	}
	return cdc.Hash(b)
}

func mustHex(t testing.TB, s string, n int) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil || len(b) != n {
		t.Fatalf("bad hex %q", s)
	}
	return b
}

// pipelineBuilder builds the shared pipeline manifest's Builder, adding files in the given order.
func pipelineBuilder(t testing.TB, order []int) *manifest.Builder {
	t.Helper()
	var d pipelineDesc
	readJSON(t, "pipeline.json", &d)
	h := manifest.Header{ProductID: d.Header.ProductID, Platform: d.Header.Platform, BuildID: d.Header.BuildID,
		Sequence: d.Header.Sequence, CreatedAt: d.Header.CreatedAt, ExpiresAt: d.Header.ExpiresAt,
		CompatEpoch: d.Header.CompatEpoch}
	copy(h.KeyID[:], mustHex(t, d.Header.KeyID, manifest.KeyIDSize))
	copy(h.Signature[:], mustHex(t, d.Header.Signature, manifest.SignatureSize))
	b := manifest.NewBuilder(h)
	chunks := map[string][]cdc.Chunk{}
	if order == nil {
		for i := range d.Files {
			order = append(order, i)
		}
	}
	for _, i := range order {
		f := d.Files[i]
		data, err := f.Input.Bytes()
		if err != nil {
			t.Fatal(err)
		}
		content, err := cdc.ChunkReader(bytes.NewReader(data))
		if err != nil {
			t.Fatal(err)
		}
		chunks[f.Path] = content.Chunks
		if err := b.AddFile(manifest.FileInput{Path: f.Path, Content: content, Tier: f.Tier,
			Flags: manifest.FileFlags(f.Flags), Group: f.Group, Language: f.Language}); err != nil {
			t.Fatal(err)
		}
	}
	for _, p := range d.Packs {
		b.AddPack(mustHash(t, p.Hash), p.Size)
	}
	for _, p := range d.Placements {
		b.PlaceChunk(chunks[p.File][p.Chunk].Hash, mustHash(t, p.Pack), p.Offset, p.StoredSize)
	}
	for _, s := range d.StoredSizes {
		b.SetStoredSize(chunks[s.File][s.Chunk].Hash, s.StoredSize)
	}
	for _, p := range d.Patches {
		b.AddPatch(p.Path, mustHash(t, p.FromHash), mustHash(t, p.PatchHash), p.PatchSize)
	}
	return b
}

func pipeline(t testing.TB) *manifest.Manifest {
	t.Helper()
	m, err := pipelineBuilder(t, nil).Build()
	if err != nil {
		t.Fatal(err)
	}
	return m
}

// pathsManifest returns n zero-size files with 1024-byte paths: prefix + "f%07d", where the prefix is "a/"
// 508 times when deep (508 directory levels: the deepest paths the limits allow) and otherwise four
// directories of the longest names (255, 255, 255 and 247 bytes of "a"). With n = 65536 the paths fill the
// 64 MiB path limit: deep-paths.hman, the worst case for the path-collision check (engine/patch/README.md
// "Validation"). The C++ tests build the same manifest.
func pathsManifest(n int, deep bool) *manifest.Manifest {
	prefix := strings.Repeat(strings.Repeat("a", manifest.MaxSegmentBytes)+"/", 3) + strings.Repeat("a", 247) + "/"
	if deep {
		prefix = strings.Repeat("a/", 508)
	}
	m := &manifest.Manifest{Header: manifest.Header{ProductID: "sample-game", Platform: "win64",
		BuildID: "deep-paths", Sequence: 1, CreatedAt: 1791072000}}
	m.Files = make([]manifest.File, n)
	empty := cdc.Sum(nil)
	for i := range m.Files {
		m.Files[i] = manifest.File{Path: fmt.Sprintf("%sf%07d", prefix, i), Hash: empty, Tier: 1}
	}
	return m
}

// shuffledDeepPaths is pathsManifest(n, true) with its collision keys in random order: the first 16
// directories are named "a" or "A" by the bits of a random permutation of the file index (SplitMix64(0x5EED)
// Fisher-Yates), and the files are then sorted by path. Lowered, the paths sort by file index; as bytes, by
// the permutation, so the collision check sorts n keys of 1 KiB from random order. As C++'s
// test::shuffledDeepPaths.
func shuffledDeepPaths(n int) *manifest.Manifest {
	if n > 1<<16 {
		panic("shuffledDeepPaths: at most 2^16 files")
	}
	m := pathsManifest(n, true)
	perm := make([]uint32, n)
	for i := range perm {
		perm[i] = uint32(i)
	}
	rng := cdc.SplitMix64{State: 0x5EED}
	for i := n; i > 1; i-- {
		j := rng.Next() % uint64(i)
		perm[i-1], perm[j] = perm[j], perm[i-1]
	}
	for i := range m.Files {
		p := []byte(m.Files[i].Path)
		for b := 0; b < 16; b++ {
			if perm[i]>>(15-b)&1 != 0 {
				p[2*b] = 'A'
			}
		}
		m.Files[i].Path = string(p)
	}
	slices.SortFunc(m.Files, func(a, b manifest.File) int { return strings.Compare(a.Path, b.Path) })
	return m
}

// deepPathFiles is the file count of deep-paths.hman: 64 MiB of 1024-byte paths.
const deepPathFiles = manifest.MaxStringBytes / manifest.MaxPathBytes

func goldenPath(name string) string { return filepath.Join(vectorsDir, name) }

func readGolden(t testing.TB, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(goldenPath(name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func errorKind(err error) string {
	switch {
	case err == nil:
		return "ok"
	case errors.Is(err, manifest.ErrVersion):
		return "version"
	case errors.Is(err, manifest.ErrUnsupported):
		return "unsupported"
	case errors.Is(err, manifest.ErrLimit):
		return "limit"
	case errors.Is(err, manifest.ErrCorrupt):
		return "corrupt"
	}
	return "other: " + err.Error()
}

// ---------------------------------------------------------------------------------------------
// Shared hostile cases: edits of pipeline.hman ("cases") and of pipeline.go-zstd.hman ("zstdCases"),
// each expected to fail one check with one error kind in both languages. Offsets are absolute; -update
// computes them from the goldens' layout.
// ---------------------------------------------------------------------------------------------

type hostileEdit struct {
	At     int    `json:"at"`
	Hex    string `json:"hex"`
	Insert bool   `json:"insert,omitempty"` // insert the bytes at At instead of overwriting
}

type hostileCase struct {
	Name   string        `json:"name"`
	Edits  []hostileEdit `json:"edits"`
	Size   int           `json:"size,omitempty"` // new file length (0 = unchanged); new bytes are zero
	Reseal string        `json:"reseal"`         // none | header (headerHash) | all (payload and body sizes, bodyHash, headerHash)
	Expect string        `json:"expect"`         // corrupt | version | unsupported | limit
	Rule   string        `json:"rule"`           // a substring of the error message: the one check the case breaks
}

func le32(v uint32) string {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, v)
	return hex.EncodeToString(b)
}
func le64(v uint64) string {
	b := make([]byte, 8)
	binary.LittleEndian.PutUint64(b, v)
	return hex.EncodeToString(b)
}

// applyHostile applies a case to a copy of golden: the new size first, then the edits in order, each at
// an offset into the bytes as the previous edits left them. Reseal "all" needs codec none.
func applyHostile(golden []byte, c hostileCase) []byte {
	b := append([]byte(nil), golden...)
	if c.Size != 0 {
		if c.Size < len(b) {
			b = b[:c.Size]
		} else {
			b = append(b, make([]byte, c.Size-len(b))...)
		}
	}
	for _, e := range c.Edits {
		v, _ := hex.DecodeString(e.Hex)
		if e.Insert {
			b = slices.Insert(b, e.At, v...)
		} else {
			copy(b[e.At:], v)
		}
	}
	if c.Reseal == "all" {
		payload := b[manifest.HeaderSize:]
		binary.LittleEndian.PutUint64(b[48:], uint64(len(payload)))
		binary.LittleEndian.PutUint64(b[56:], uint64(len(payload)))
		h := cdc.Sum(payload)
		copy(b[64:96], h[:])
	}
	if c.Reseal == "all" || c.Reseal == "header" {
		h := cdc.Sum(b[:manifest.SignedBytes])
		copy(b[256:288], h[:])
	}
	return b
}

// hostileCases derives the shared hostile cases from the golden's layout. Each case breaks exactly one
// check, named by its Rule (a message substring both languages share): where one edit would also break
// another check, the case edits the dependent fields together (a chunk's size with its file's size).
func hostileCases(t *testing.T, golden []byte, m *manifest.Manifest) []hostileCase {
	F, R, C, P := len(m.Files), len(m.Refs), len(m.Chunks), len(m.Packs)
	const H = manifest.HeaderSize
	body := H
	files := body + 32
	file := func(i int) int { return files + 80*i }
	refs := files + 80*F
	chunks := refs + 16*R
	chunk := func(i int) int { return chunks + 56*i }
	packs := chunks + 56*C
	patches := packs + 40*P
	patch := func(i int) int { return patches + 80*i }
	stringsAt := patches + 80*len(m.Patches)
	stringBytes := len(golden) - stringsAt
	// Body-header fields (README "Body"): the ref, pack and path-byte counts.
	refCount, packCount, stringBytesAt := body+8, body+12, body+20
	looseChunk, packedChunk := -1, -1
	for i, c := range m.Chunks {
		if c.Pack == manifest.NoPack && looseChunk < 0 {
			looseChunk = i
		}
		if c.Pack != manifest.NoPack && packedChunk < 0 {
			packedChunk = i
		}
	}
	// A file of one chunk that no other file shares: its chunk's size can change with the file's.
	soloFile, soloChunk := -1, -1
	uses := make([]int, C)
	for _, r := range m.Refs {
		uses[r.Chunk]++
	}
	for i, f := range m.Files {
		if f.RefCount == 1 && uses[m.Refs[f.FirstRef].Chunk] == 1 {
			soloFile, soloChunk = i, int(m.Refs[f.FirstRef].Chunk)
			break
		}
	}
	// A patch whose fromHash can become its target's hash without leaving (file, fromHash) order.
	selfPatch := -1
	for i, q := range m.Patches {
		target := m.Files[q.File].Hash
		after := i == 0 || m.Patches[i-1].File != q.File || bytes.Compare(m.Patches[i-1].FromHash[:], target[:]) < 0
		before := i == len(m.Patches)-1 || m.Patches[i+1].File != q.File ||
			bytes.Compare(target[:], m.Patches[i+1].FromHash[:]) < 0
		if after && before {
			selfPatch = i
			break
		}
	}
	ffs := strings.Repeat("ff", 32) // an ID after every pack's
	if looseChunk < 0 || packedChunk < 0 || soloFile < 0 || selfPatch < 0 || F < 2 ||
		hex.EncodeToString(m.Packs[P-1].Hash[:]) == ffs {
		t.Fatal("the pipeline vector lost the features the hostile cases need")
	}
	e := func(at int, hexs string) []hostileEdit { return []hostileEdit{{At: at, Hex: hexs}} }
	solo := func(size uint32) []hostileEdit { // the solo chunk's raw size and its file's size together
		return []hostileEdit{{At: chunk(soloChunk) + 32, Hex: le32(size)}, {At: file(soloFile) + 8, Hex: le64(uint64(size))}}
	}
	all, hdr, none := "all", "header", "none"
	fileHash := m.Files[m.Patches[selfPatch].File].Hash
	return []hostileCase{
		{"bad magic", e(0, "00"), 0, none, "corrupt", "not a .hman file"},
		{"version 1", e(4, "0100"), 0, hdr, "version", ".hman version 1"},
		{"header size 351", e(6, "5f01"), 0, hdr, "corrupt", "header size 351"},
		{"header hash mismatch", e(16, "ff"), 0, none, "corrupt", "the header hash does not match"},
		{"unknown header flag", e(8, "01000000"), 0, hdr, "unsupported", "unknown header flags"},
		{"unknown codec", e(12, "02"), 0, hdr, "unsupported", "unknown codec 2"},
		{"reserved header byte", e(13, "01"), 0, hdr, "corrupt", "reserved header bytes"},
		{"reserved header word", e(44, "01"), 0, hdr, "corrupt", "reserved header bytes"},
		{"reserved header tail", e(250, "01"), 0, hdr, "corrupt", "reserved header bytes"},
		{"product id upper case", e(96, "43"), 0, hdr, "corrupt", "is not ^[a-z][a-z0-9-]{2,31}$"},
		{"product id byte after its end", e(127, "61"), 0, hdr, "corrupt", "bytes after its end"},
		{"empty platform", e(128, strings.Repeat("00", 32)), 0, hdr, "corrupt", "is not ^[a-z][a-z0-9_-]{1,31}$"},
		{"build id with a slash", e(161, "2f"), 0, hdr, "corrupt", "is not ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"},
		{"expiry before creation", e(32, le64(m.Header.CreatedAt)), 0, hdr, "corrupt", "is not after createdAt"},
		{"body larger than the cap", e(48, le64(manifest.MaxBodySize+1)), 0, hdr, "limit", "is above"},
		{"body shorter than its header", e(48, le64(31)), 0, hdr, "corrupt", "shorter than the body header"},
		{"payload size mismatch", e(56, le64(uint64(len(golden)-H+1))), 0, hdr, "corrupt", "bytes follow the header"},
		{"body hash mismatch", e(stringsAt, "7a"), 0, hdr, "corrupt", "the body hash does not match"},
		{"truncated payload", nil, len(golden) - 1, none, "corrupt", "bytes follow the header"},
		{"trailing byte", nil, len(golden) + 1, all, "corrupt", "its counts need"},
		{"too many files", e(body, le32(manifest.MaxFiles+1)), 0, all, "limit", "exceed the limits"},
		{"counts disagree with the body size", e(body, le32(uint32(F+1))), 0, all, "corrupt", "its counts need"},
		{"reserved body header", e(body+24, "01"), 0, all, "corrupt", "reserved body-header bytes"},
		{"path offset out of order", e(file(1), le32(1)), 0, all, "corrupt", "file 1: path offset 1"},
		{"path length zero", e(file(0)+4, le32(0)), 0, all, "corrupt", "file 0: path length 0"},
		{"path byte of no file", []hostileEdit{{At: stringBytesAt, Hex: le32(uint32(stringBytes + 1))},
			{At: len(golden), Hex: "61"}}, len(golden) + 1, all, "corrupt", "1 path bytes are not any file's path"},
		{"file size disagrees with its chunks", e(file(1)+8, le64(m.Files[1].Size+1)), 0, all, "corrupt",
			fmt.Sprintf("its size is %d", m.Files[1].Size+1)},
		{"first ref out of order", e(file(1)+48, le32(m.Files[1].FirstRef+1)), 0, all, "corrupt",
			fmt.Sprintf("first ref %d", m.Files[1].FirstRef+1)},
		{"tier 3", e(file(0)+68, "03"), 0, all, "corrupt", "tier 3"},
		{"reserved file byte", e(file(0)+69, "01"), 0, all, "corrupt", "file 0: reserved bytes"},
		{"unknown file flag", e(file(0)+70, "0800"), 0, all, "corrupt", "unknown flags 0x8"},
		{"reserved file word", e(file(0)+72, "01"), 0, all, "corrupt", "file 0: reserved bytes"},
		{"backslash in a path", e(stringsAt+3, "5c"), 0, all, "corrupt", "is not a valid manifest path"},
		{"paths out of order", e(stringsAt, "7a"), 0, all, "corrupt", "(sorted, unique paths)"},
		{"ref to a missing chunk", e(refs, le32(uint32(C))), 0, all, "corrupt", fmt.Sprintf("chunk %d does not exist", C)},
		{"reserved ref word", e(refs+4, "01"), 0, all, "corrupt", "ref 0: reserved bytes"},
		{"ref offset wrong", e(refs+16+8, le64(m.Refs[1].Offset+1)), 0, all, "corrupt",
			fmt.Sprintf("ref 1: offset %d is not %d", m.Refs[1].Offset+1, m.Refs[1].Offset)},
		{"ref after the last file's, to chunk 0xFFFFFFFF", []hostileEdit{{At: refCount, Hex: le32(uint32(R + 1))},
			{At: chunks, Hex: le32(0xFFFFFFFF) + le32(0) + le64(0), Insert: true}}, 0, all, "corrupt",
			"1 refs belong to no file"},
		{"chunk raw size zero", solo(0), 0, all, "corrupt", fmt.Sprintf("chunk %d: raw size 0 is out of range", soloChunk)},
		{"chunk raw size above max", solo(cdc.MaxSize + 1), 0, all, "corrupt",
			fmt.Sprintf("chunk %d: raw size %d is out of range", soloChunk, cdc.MaxSize+1)},
		{"chunk stored size above max", e(chunk(looseChunk)+36, le32(cdc.MaxSize+4097)), 0, all, "corrupt",
			fmt.Sprintf("stored size %d is out of range", cdc.MaxSize+4097)},
		{"chunks out of order", e(chunk(0), strings.Repeat("ff", 32)), 0, all, "corrupt", "chunk 1: IDs are not sorted"},
		{"chunk in a missing pack", e(chunk(packedChunk)+40, le32(uint32(P))), 0, all, "corrupt",
			fmt.Sprintf("pack %d does not exist", P)},
		{"reserved chunk word", e(chunk(0)+44, "01"), 0, all, "corrupt", "chunk 0: reserved bytes"},
		{"loose chunk with a pack offset", e(chunk(looseChunk)+48, le64(1)), 0, all, "corrupt", "a loose chunk has a pack offset"},
		{"packed chunk without a stored size", e(chunk(packedChunk)+36, le32(0)), 0, all, "corrupt", ": 0 stored bytes at"},
		{"packed chunk past its pack", e(chunk(packedChunk)+48, le64(m.Packs[0].Size)), 0, all, "corrupt",
			fmt.Sprintf("stored bytes at %d do not fit pack", m.Packs[0].Size)},
		{"pack of size zero, which no chunk fits", e(packs+32, le64(0)), 0, all, "corrupt", "do not fit pack 0 (0 bytes)"},
		{"pack size above max", e(packs+32, le64(manifest.MaxPackSize+1)), 0, all, "corrupt",
			fmt.Sprintf("pack 0: size %d is out of range", manifest.MaxPackSize+1)},
		{"pack that holds no chunk", []hostileEdit{{At: packCount, Hex: le32(uint32(P + 1))},
			{At: patches, Hex: ffs + le64(1), Insert: true}}, 0, all, "corrupt", fmt.Sprintf("pack %s holds no chunk", ffs)},
		{"patch of a missing file", e(patch(0), le32(uint32(F))), 0, all, "corrupt", fmt.Sprintf("patch 0: file %d does not exist", F)},
		{"reserved patch word", e(patch(0)+4, "01"), 0, all, "corrupt", "patch 0: reserved bytes"},
		{"patches out of order", e(patch(0)+8, strings.Repeat("ff", 32)), 0, all, "corrupt", "patch 1: not sorted"},
		{"patch onto itself", e(patch(selfPatch)+8, hex.EncodeToString(fileHash[:])), 0, all, "corrupt",
			fmt.Sprintf("patch %d: it patches", selfPatch)},
		{"patch size zero", e(patch(0)+72, le64(0)), 0, all, "corrupt", "patch 0: size 0 is out of range"},
		{"patch size above max", e(patch(0)+72, le64(manifest.MaxPatchSize+1)), 0, all, "corrupt",
			fmt.Sprintf("patch 0: size %d is out of range", manifest.MaxPatchSize+1)},
	}
}

// hostileZstdCases derives the shared cases on pipeline.go-zstd.hman (codec 1, so reseal "header" only).
func hostileZstdCases(t *testing.T, z []byte) []hostileCase {
	bodySize := binary.LittleEndian.Uint64(z[48:])
	// The readers' cap on a zstd payload: bodySize + bodySize/128 + 4096 (README "zstd payloads"). One byte
	// more, as a skippable frame (RFC 8878 §3.1.2: magic 0x184D2A50, a u32 size, then that many bytes)
	// after the real one, which the decoders would otherwise skip.
	over := bodySize + bodySize/128 + 4096 + 1
	pad := int(over) - (len(z) - manifest.HeaderSize)
	if pad < 8 {
		t.Fatal("the zstd golden's payload is too close to its bound for a skippable frame")
	}
	frame := le32(0x184D2A50) + le32(uint32(pad-8))
	return []hostileCase{
		{"zstd payload above its bound (a trailing skippable frame)", []hostileEdit{{At: 56, Hex: le64(over)},
			{At: len(z), Hex: frame}}, manifest.HeaderSize + int(over), "header", "corrupt",
			fmt.Sprintf("payloadSize %d does not fit bodySize %d", over, bodySize)},
	}
}

func TestUpdateGoldens(t *testing.T) {
	if !*update {
		t.Skip("run with -update to rewrite the Go-written vectors")
	}
	m := pipeline(t)
	raw, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecNone})
	if err != nil {
		t.Fatal(err)
	}
	z, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: 19})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(goldenPath("pipeline.hman"), raw, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(goldenPath("pipeline.go-zstd.hman"), z, 0o644); err != nil {
		t.Fatal(err)
	}
	deep, err := pathsManifest(deepPathFiles, true).Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: 19})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(goldenPath("deep-paths.hman"), deep, 0o644); err != nil {
		t.Fatal(err)
	}
	var b strings.Builder
	b.WriteString("{\"comment\": \"Edits that Go pkg/manifest and C++ engine/patch must both reject with the given error " +
		"kind, failing the check whose message contains rule: cases edit pipeline.hman, zstdCases pipeline.go-zstd.hman. " +
		"size (if set) resizes the file first (new bytes are zero); then each edit, in order, overwrites the bytes at at, or " +
		"inserts them there if insert is set. reseal: none; header = headerHash recomputed; all = payloadSize and bodySize " +
		"set to the payload's length, bodyHash and headerHash recomputed (codec none only). " +
		"Regenerate with go test ./pkg/manifest -run TestUpdateGoldens -update.\",\n")
	for k, cases := range [][]hostileCase{hostileCases(t, raw, m), hostileZstdCases(t, z)} {
		fmt.Fprintf(&b, "%q: [\n", []string{"cases", "zstdCases"}[k])
		for i, c := range cases {
			line, _ := json.Marshal(c)
			sep := ","
			if i == len(cases)-1 {
				sep = ""
			}
			fmt.Fprintf(&b, "%s%s\n", line, sep)
		}
		b.WriteString([]string{"],\n", "]}\n"}[k])
	}
	if err := os.WriteFile(goldenPath("hostile.json"), []byte(b.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}

// Both writers produce pipeline.hman byte for byte from the shared description.
func TestPipelineGolden(t *testing.T) {
	m := pipeline(t)
	raw, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecNone})
	if err != nil {
		t.Fatal(err)
	}
	golden := readGolden(t, "pipeline.hman")
	if !bytes.Equal(raw, golden) {
		for i := range raw {
			if i >= len(golden) || raw[i] != golden[i] {
				t.Fatalf("the Go writer differs from pipeline.hman at byte %d (%d vs %d bytes)", i, len(raw), len(golden))
			}
		}
		t.Fatalf("the Go writer wrote %d bytes, pipeline.hman has %d", len(raw), len(golden))
	}
	// Files added in another order build the same manifest.
	rev, err := pipelineBuilder(t, []int{7, 6, 5, 4, 3, 2, 1, 0}).Build()
	if err != nil {
		t.Fatal(err)
	}
	again, _ := rev.Marshal(manifest.WriteOptions{Codec: manifest.CodecNone})
	if !bytes.Equal(again, golden) {
		t.Fatal("the builder's output depends on the order files were added")
	}
	if len(m.Files) != 8 || len(m.Packs) != 1 || len(m.Patches) != 2 || len(m.Chunks) >= len(m.Refs) {
		t.Fatalf("unexpected shape: %d files, %d chunks, %d refs", len(m.Files), len(m.Chunks), len(m.Refs))
	}
}

// Every golden, whichever language wrote it, reads back to the description's manifest.
func TestPipelineCrossLanguageRead(t *testing.T) {
	want := pipeline(t)
	for _, name := range []string{"pipeline.hman", "pipeline.go-zstd.hman", "pipeline.cpp-zstd.hman"} {
		t.Run(name, func(t *testing.T) {
			file := readGolden(t, name)
			got, err := manifest.Parse(file, 0)
			if err != nil {
				t.Fatal(err)
			}
			if !equalManifests(got, want) {
				t.Fatal("the manifest read differs from the description's")
			}
			info, err := manifest.ParseHeader(file, 0)
			if err != nil || info.Header != want.Header {
				t.Fatalf("header: %+v %v", info.Header, err)
			}
			if wantCodec := manifest.CodecZstd; name != "pipeline.hman" && info.Codec != wantCodec {
				t.Fatalf("codec %d", info.Codec)
			}
		})
	}
}

// deep-paths.hman (written by Go, read by both languages) is the deepest-paths manifest: it reads back to
// pathsManifest(deepPathFiles, true). perf_test.go times it against the read budget.
func TestSharedDeepPaths(t *testing.T) {
	want := pathsManifest(deepPathFiles, true)
	got, err := manifest.Parse(readGolden(t, "deep-paths.hman"), 0)
	if err != nil {
		t.Fatal(err)
	}
	if !equalManifests(got, want) {
		t.Fatal("deep-paths.hman differs from the deepest-paths manifest")
	}
}

// equalManifests compares field by field (a nil and an empty table are equal).
func equalManifests(a, b *manifest.Manifest) bool {
	return a.Header == b.Header && slices.Equal(a.Files, b.Files) && slices.Equal(a.Refs, b.Refs) &&
		slices.Equal(a.Chunks, b.Chunks) && slices.Equal(a.Packs, b.Packs) && slices.Equal(a.Patches, b.Patches)
}

type hostileFile struct {
	Cases     []hostileCase `json:"cases"`
	ZstdCases []hostileCase `json:"zstdCases"`
}

func TestSharedHostileCases(t *testing.T) {
	var f hostileFile
	readJSON(t, "hostile.json", &f)
	golden, z := readGolden(t, "pipeline.hman"), readGolden(t, "pipeline.go-zstd.hman")
	if len(f.Cases) < 50 || len(f.ZstdCases) < 1 {
		t.Fatalf("only %d + %d hostile cases", len(f.Cases), len(f.ZstdCases))
	}
	// The cases still match the goldens' layout (so a regenerated golden needs regenerated cases).
	for _, set := range []struct {
		name      string
		base      []byte
		got, want []hostileCase
	}{{"cases", golden, f.Cases, hostileCases(t, golden, pipeline(t))}, {"zstdCases", z, f.ZstdCases, hostileZstdCases(t, z)}} {
		for _, c := range set.got {
			_, err := manifest.Parse(applyHostile(set.base, c), 0)
			if got := errorKind(err); got != c.Expect {
				t.Errorf("%s: %s, want %s (%v)", c.Name, got, c.Expect, err)
			} else if c.Rule == "" || !strings.Contains(err.Error(), c.Rule) {
				t.Errorf("%s: failed another check than %q: %v", c.Name, c.Rule, err)
			}
		}
		if len(set.want) != len(set.got) {
			t.Fatalf("hostile.json has %d %s, the generator %d: run -update", len(set.got), set.name, len(set.want))
		}
		for i := range set.want {
			a, _ := json.Marshal(set.want[i])
			b, _ := json.Marshal(set.got[i])
			if !bytes.Equal(a, b) {
				t.Fatalf("hostile.json %s[%d] differs from the generator: run -update", set.name, i)
			}
		}
	}
}

type validity struct {
	Valid   []string `json:"valid"`
	Invalid []string `json:"invalid"`
}

func TestSharedNames(t *testing.T) {
	var raw struct {
		Paths      validity `json:"paths"`
		ProductIDs validity `json:"productIds"`
		Platforms  validity `json:"platforms"`
		BuildIDs   validity `json:"buildIds"`
		PathSets   struct {
			Valid   [][]string `json:"valid"`
			Invalid []struct {
				Paths []string `json:"paths"`
				Rule  string   `json:"rule"`
			} `json:"invalid"`
		} `json:"pathSets"`
	}
	readJSON(t, "names.json", &raw)
	// The longest path (255-byte segments), one byte more, and the longest segment first, inside and last.
	longest := strings.Repeat(strings.Repeat("a", manifest.MaxSegmentBytes)+"/", 3) + strings.Repeat("a", 254) + "/b"
	valid, invalid := append(raw.Paths.Valid, longest), append(raw.Paths.Invalid, longest+"b")
	for _, n := range []int{manifest.MaxSegmentBytes, manifest.MaxSegmentBytes + 1} {
		seg := strings.Repeat("s", n)
		for _, p := range []string{seg + "/x", "x/" + seg + "/y", "x/" + seg} {
			if n <= manifest.MaxSegmentBytes {
				valid = append(valid, p)
			} else {
				invalid = append(invalid, p)
			}
		}
	}
	if len(longest) != manifest.MaxPathBytes {
		t.Fatalf("the longest path has %d bytes", len(longest))
	}
	for _, p := range valid {
		if !manifest.ValidPath(p) {
			t.Errorf("path %q should be valid", p)
		}
	}
	for _, p := range invalid {
		if manifest.ValidPath(p) {
			t.Errorf("path %q should be invalid", p)
		}
	}
	base := pipeline(t).Header
	check := func(kind string, v validity, set func(*manifest.Header, string)) {
		if len(v.Valid) == 0 || len(v.Invalid) == 0 {
			t.Fatalf("%s: empty vector list", kind)
		}
		for _, want := range []bool{true, false} {
			list := v.Valid
			if !want {
				list = v.Invalid
			}
			for _, s := range list {
				h := base
				set(&h, s)
				m := &manifest.Manifest{Header: h}
				if err := m.Validate(); (err == nil) != want {
					t.Errorf("%s %q: valid=%v, got %v", kind, s, want, err)
				}
			}
		}
	}
	check("productId", raw.ProductIDs, func(h *manifest.Header, s string) { h.ProductID = s })
	check("platform", raw.Platforms, func(h *manifest.Header, s string) { h.Platform = s })
	check("buildId", raw.BuildIDs, func(h *manifest.Header, s string) { h.BuildID = s })

	if len(raw.PathSets.Valid) == 0 || len(raw.PathSets.Invalid) == 0 {
		t.Fatal("pathSets: empty vector list")
	}
	for _, paths := range raw.PathSets.Valid {
		if err := zeroSizeFiles(base, paths).Validate(); err != nil {
			t.Errorf("path set %q: %v", paths, err)
		}
	}
	for _, c := range raw.PathSets.Invalid {
		if err := zeroSizeFiles(base, c.Paths).Validate(); err == nil || !strings.Contains(err.Error(), c.Rule) {
			t.Errorf("path set %q: %v, want an error containing %q", c.Paths, err, c.Rule)
		}
	}
}

// zeroSizeFiles is a manifest of zero-size files with these paths, in this order.
func zeroSizeFiles(h manifest.Header, paths []string) *manifest.Manifest {
	m := &manifest.Manifest{Header: h}
	for _, p := range paths {
		m.Files = append(m.Files, manifest.File{Path: p, Tier: 1})
	}
	return m
}

// The sorted-key collision check agrees with the obvious one (every '/'-prefix of every lowered path looked
// up among the lowered paths) on random path sets over a small alphabet, so case, '-' and '.' (which sort
// before '/') and nesting all meet. engine/patch's tests run the same sets.
func TestPathCollisionsMatchNaive(t *testing.T) {
	const alphabet = "aAb-./"
	rng := cdc.SplitMix64{State: 0x0C011151}
	outcomes := map[bool]int{}
	for iter := 0; iter < 20000; iter++ {
		seen := map[string]bool{}
		var paths []string
		for n := 2 + int(rng.Next()%5); len(paths) < n; {
			b := make([]byte, 1+rng.Next()%6)
			for i := range b {
				b[i] = alphabet[rng.Next()%uint64(len(alphabet))]
			}
			if p := string(b); manifest.ValidPath(p) && !seen[p] {
				seen[p] = true
				paths = append(paths, p)
			}
		}
		slices.Sort(paths)
		want := naiveCollisionFree(paths)
		err := zeroSizeFiles(manifest.Header{ProductID: "sample-game", Platform: "win64", BuildID: "x"}, paths).Validate()
		if (err == nil) != want {
			t.Fatalf("%q: Validate says %v, the naive check %v", paths, err, want)
		}
		outcomes[want]++
	}
	if outcomes[true] < 1000 || outcomes[false] < 1000 {
		t.Fatalf("unbalanced cases: %v", outcomes)
	}
}

func naiveCollisionFree(paths []string) bool {
	lowered := map[string]bool{}
	for _, p := range paths {
		low := strings.ToLower(p)
		if lowered[low] {
			return false
		}
		lowered[low] = true
	}
	for low := range lowered {
		for i := 0; i < len(low); i++ {
			if low[i] == '/' && lowered[low[:i]] {
				return false
			}
		}
	}
	return true
}

// Every truncation and every single-byte change of the golden fails cleanly, except in the signature,
// which v0 does not interpret.
func TestTruncationsAndFlips(t *testing.T) {
	golden := readGolden(t, "pipeline.hman")
	for n := 0; n < len(golden); n++ {
		if _, err := manifest.Parse(golden[:n], 0); err == nil {
			t.Fatalf("a %d-byte prefix parsed", n)
		}
	}
	for i := range golden {
		b := append([]byte(nil), golden...)
		b[i] ^= 0x20
		_, err := manifest.Parse(b, 0)
		inSignature := i >= 288 && i < manifest.HeaderSize
		if inSignature != (err == nil) {
			t.Fatalf("flipping byte %d: %v", i, err)
		}
	}
	z := readGolden(t, "pipeline.go-zstd.hman")
	for n := manifest.HeaderSize; n < len(z); n += 7 {
		if _, err := manifest.Parse(z[:n], 0); err == nil {
			t.Fatalf("a %d-byte prefix of the zstd golden parsed", n)
		}
	}
}

// A zstd payload that decodes to more or fewer bytes than bodySize fails, and so does one that claims
// a huge body behind a few bytes (it decodes only what is there).
func TestZstdPayloadBounds(t *testing.T) {
	m := pipeline(t)
	z, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: 3})
	if err != nil {
		t.Fatal(err)
	}
	reseal := func(b []byte) []byte {
		h := cdc.Sum(b[:manifest.SignedBytes])
		copy(b[256:288], h[:])
		return b
	}
	for _, delta := range []int64{-1, 1, 1 << 20} {
		b := append([]byte(nil), z...)
		size := int64(binary.LittleEndian.Uint64(b[48:])) + delta
		binary.LittleEndian.PutUint64(b[48:], uint64(size))
		if _, err := manifest.Parse(reseal(b), 0); errorKind(err) != "corrupt" {
			t.Fatalf("bodySize %+d: %v", delta, err)
		}
	}
	// A body cap below the real size is LimitExceeded.
	if _, err := manifest.Parse(z, 64); errorKind(err) != "limit" {
		t.Fatalf("maxBodySize 64: %v", err)
	}
}

func TestWriterRefusesInvalidManifests(t *testing.T) {
	m := pipeline(t)
	mutate := func(f func(m *manifest.Manifest)) error {
		c := *m
		c.Files = append([]manifest.File(nil), m.Files...)
		c.Chunks = append([]manifest.Chunk(nil), m.Chunks...)
		c.Refs = append([]manifest.ChunkRef(nil), m.Refs...)
		c.Patches = append([]manifest.Patch(nil), m.Patches...)
		f(&c)
		_, err := c.Marshal(manifest.WriteOptions{Codec: manifest.CodecNone})
		return err
	}
	cases := map[string]func(m *manifest.Manifest){
		"bad product":    func(m *manifest.Manifest) { m.Header.ProductID = "X" },
		"unsorted files": func(m *manifest.Manifest) { m.Files[0], m.Files[1] = m.Files[1], m.Files[0] },
		"tier 3":         func(m *manifest.Manifest) { m.Files[0].Tier = 3 },
		"unreferenced": func(m *manifest.Manifest) {
			m.Chunks = append(m.Chunks, manifest.Chunk{Hash: cdc.Hash{0xff}, RawSize: 1, Pack: manifest.NoPack})
		},
		"patch to itself": func(m *manifest.Manifest) { m.Patches[0].FromHash = m.Files[m.Patches[0].File].Hash },
		"ref off by one":  func(m *manifest.Manifest) { m.Refs[1].Offset++ },
	}
	for name, f := range cases {
		if err := mutate(f); !errors.Is(err, manifest.ErrInvalid) {
			t.Errorf("%s: %v", name, err)
		}
	}
	if _, err := m.Marshal(manifest.WriteOptions{Codec: 7}); err == nil {
		t.Error("unknown codec accepted")
	}
	for _, level := range []int{-1, 20} {
		if _, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: level}); err == nil {
			t.Errorf("zstd level %d accepted", level)
		}
	}
}

// The zero WriteOptions write codec none, and with CodecZstd level 0 is level 19, as C++'s default
// ManifestWriteOptions (engine/patch/tests/test_manifest.cpp checks the same).
func TestWriteOptionDefaults(t *testing.T) {
	m := pipeline(t)
	plain, err := m.Marshal(manifest.WriteOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(plain, readGolden(t, "pipeline.hman")) {
		t.Fatal("the zero WriteOptions did not write codec none")
	}
	z0, err0 := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd})
	z19, err19 := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: 19})
	if err0 != nil || err19 != nil || !bytes.Equal(z0, z19) {
		t.Fatalf("level 0 is not level 19: %v %v", err0, err19)
	}
}

func TestBuilderRefusals(t *testing.T) {
	h := pipeline(t).Header
	data := cdctest.Random(1, 100000)
	content := cdc.Split(data)
	file := cdc.File{Size: uint64(len(data)), Hash: cdc.Sum(data), Chunks: content}
	add := func(b *manifest.Builder, path string) error {
		return b.AddFile(manifest.FileInput{Path: path, Content: file, Tier: 1})
	}
	b := manifest.NewBuilder(h)
	if err := add(b, "a/../b"); err == nil {
		t.Error("an invalid path was added")
	}
	if err := b.AddFile(manifest.FileInput{Path: "x", Content: cdc.File{Size: 5}, Tier: 1}); err == nil {
		t.Error("chunks that do not tile the file were added")
	}
	if err := b.AddFile(manifest.FileInput{Path: "x", Content: file, Tier: 3}); err == nil {
		t.Error("tier 3 was added")
	}
	_ = add(b, "dup")
	_ = add(b, "dup")
	if _, err := b.Build(); err == nil {
		t.Error("a duplicate path built")
	}
	b = manifest.NewBuilder(h)
	_ = add(b, "a")
	b.PlaceChunk(content[0].Hash, cdc.Hash{1}, 0, 10)
	if _, err := b.Build(); err == nil {
		t.Error("a placement in an undeclared pack built")
	}
	b = manifest.NewBuilder(h)
	_ = add(b, "a")
	b.SetStoredSize(cdc.Hash{9}, 10)
	if _, err := b.Build(); err == nil {
		t.Error("a stored size for an unknown chunk built")
	}
	refuses := func(what, rule string, setup func(b *manifest.Builder)) { // with this message: no other check
		b := manifest.NewBuilder(h)
		_ = add(b, "a")
		setup(b)
		if _, err := b.Build(); err == nil || !strings.Contains(err.Error(), rule) {
			t.Errorf("%s: %v, want an error containing %q", what, err, rule)
		}
	}
	refuses("a pack that holds no chunk built", "holds no chunk", func(b *manifest.Builder) { b.AddPack(cdc.Hash{1}, 100) })
	refuses("one chunk's stored size set twice built", "is placed twice", func(b *manifest.Builder) {
		b.SetStoredSize(content[0].Hash, 10)
		b.SetStoredSize(content[0].Hash, 11)
	})
	refuses("one chunk placed in a pack and given a loose stored size built", "is placed twice", func(b *manifest.Builder) {
		b.AddPack(cdc.Hash{1}, 100)
		b.PlaceChunk(content[0].Hash, cdc.Hash{1}, 0, 10)
		b.SetStoredSize(content[0].Hash, 11)
	})
	b = manifest.NewBuilder(h)
	_ = add(b, "a")
	b.AddPatch("missing", cdc.Hash{1}, cdc.Hash{2}, 3)
	if _, err := b.Build(); err == nil {
		t.Error("a patch of a missing file built")
	}
	b = manifest.NewBuilder(h)
	_ = add(b, "a")
	_ = add(b, "A/b")
	if _, err := b.Build(); err == nil {
		t.Error("a file that is also a directory (ignoring case) built")
	}
	b = manifest.NewBuilder(h)
	_ = add(b, "docs/readme")
	_ = add(b, "Docs/readme")
	if _, err := b.Build(); err == nil {
		t.Error("two paths that differ only in case built")
	}
	b = manifest.NewBuilder(h)
	bad := file
	bad.Chunks = append([]cdc.Chunk(nil), content...)
	bad.Chunks[0].Size--
	bad.Chunks[1].Offset--
	bad.Chunks[1].Size++
	bad.Chunks[1].Hash = content[0].Hash // same ID, another size
	_ = add(b, "a")
	if err := b.AddFile(manifest.FileInput{Path: "b", Content: bad, Tier: 1}); err != nil {
		t.Fatal(err)
	}
	if _, err := b.Build(); err == nil {
		t.Error("one chunk ID with two sizes built")
	}
}

func TestLookups(t *testing.T) {
	m := pipeline(t)
	for i, f := range m.Files {
		if m.FindFile(f.Path) != i || len(m.FileRefs(i)) != int(f.RefCount) {
			t.Fatalf("file %q", f.Path)
		}
		for _, r := range m.FileRefs(i) {
			if m.FindChunk(m.Chunks[r.Chunk].Hash) != int(r.Chunk) {
				t.Fatalf("chunk %d", r.Chunk)
			}
		}
	}
	if m.FindFile("nope") != -1 || m.FindChunk(cdc.Hash{}) != -1 {
		t.Fatal("found what is not there")
	}
}

// FuzzParse: Parse never panics; a manifest that parses is valid, its canonical body hashes to the
// header's bodyHash, and it re-marshals (codec none) to bytes that parse back equal. The seeds are the
// shared goldens (deep-paths.hman among them), a small deep-paths manifest and the hostile cases; the
// normal go test run executes them.
func FuzzParse(f *testing.F) {
	// deep-paths.hman's 72 MB body is above this target's 16 MiB cap (it stops at the header), so a
	// 64-file version of it seeds mutations of deep paths.
	for _, name := range []string{"pipeline.hman", "pipeline.go-zstd.hman", "pipeline.cpp-zstd.hman", "deep-paths.hman"} {
		if b, err := os.ReadFile(goldenPath(name)); err == nil {
			f.Add(b)
		}
	}
	if b, err := pathsManifest(64, true).Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: 3}); err == nil {
		f.Add(b)
	}
	var hf hostileFile
	if raw, err := os.ReadFile(goldenPath("hostile.json")); err == nil && json.Unmarshal(raw, &hf) == nil {
		if golden, err := os.ReadFile(goldenPath("pipeline.hman")); err == nil {
			for _, c := range hf.Cases {
				f.Add(applyHostile(golden, c))
			}
		}
		if z, err := os.ReadFile(goldenPath("pipeline.go-zstd.hman")); err == nil {
			for _, c := range hf.ZstdCases {
				f.Add(applyHostile(z, c))
			}
		}
	}
	f.Fuzz(func(t *testing.T, b []byte) {
		for _, in := range [][]byte{b, reseal(b)} {
			m, err := manifest.Parse(in, 16<<20)
			if err != nil {
				continue
			}
			info, err := manifest.ParseHeader(in, 16<<20)
			if err != nil || info.Header != m.Header {
				t.Fatalf("Parse succeeded, ParseHeader: %v", err)
			}
			if err := m.Validate(); err != nil {
				t.Fatalf("a parsed manifest is invalid: %v", err)
			}
			body, err := m.EncodeBody()
			if err != nil || uint64(len(body)) != info.BodySize || cdc.Sum(body) != info.BodyHash {
				t.Fatalf("the canonical body differs from the parsed one: %v", err)
			}
			out, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecNone})
			if err != nil {
				t.Fatal(err)
			}
			back, err := manifest.Parse(out, 0)
			if err != nil || !equalManifests(back, m) {
				t.Fatalf("re-marshaled manifest: %v", err)
			}
		}
	})
}

// reseal recomputes payloadSize, bodySize and bodyHash (a zstd payload is decoded, up to 16 MiB, to learn
// its body) and the header hash, so fuzzed bytes reach the field checks behind the hashes.
func reseal(in []byte) []byte {
	b := append([]byte(nil), in...)
	if len(b) < manifest.HeaderSize {
		return b
	}
	payload := b[manifest.HeaderSize:]
	binary.LittleEndian.PutUint64(b[56:], uint64(len(payload)))
	body := payload
	if b[12] == 1 {
		dec, err := zstd.NewReader(nil, zstd.WithDecoderConcurrency(1), zstd.WithDecoderMaxMemory(16<<20))
		if err != nil {
			return b
		}
		body, err = dec.DecodeAll(payload, nil)
		dec.Close()
		if err != nil {
			return b
		}
	}
	binary.LittleEndian.PutUint64(b[48:], uint64(len(body)))
	h := cdc.Sum(body)
	copy(b[64:96], h[:])
	h = cdc.Sum(b[:manifest.SignedBytes])
	copy(b[256:288], h[:])
	return b
}

// A trailing skippable frame may pad a zstd payload up to its bound, bodySize + bodySize/128 + 4096, in
// both languages; one byte more is hostile.json's zstd case.
func TestZstdPayloadAtItsBound(t *testing.T) {
	z := readGolden(t, "pipeline.go-zstd.hman")
	bodySize := binary.LittleEndian.Uint64(z[48:])
	bound := bodySize + bodySize/128 + 4096
	b := append(append([]byte(nil), z...), make([]byte, manifest.HeaderSize+int(bound)-len(z))...)
	binary.LittleEndian.PutUint32(b[len(z):], 0x184D2A50)
	binary.LittleEndian.PutUint32(b[len(z)+4:], uint32(len(b)-len(z)-8))
	binary.LittleEndian.PutUint64(b[56:], bound)
	h := cdc.Sum(b[:manifest.SignedBytes])
	copy(b[256:288], h[:])
	m, err := manifest.Parse(b, 0)
	if err != nil || !equalManifests(m, pipeline(t)) {
		t.Fatalf("a payload of exactly its bound: %v", err)
	}
}

// A skippable zstd frame before the payload's frames is allowed (RFC 8878), in both languages.
func TestZstdSkippableFrame(t *testing.T) {
	z := readGolden(t, "pipeline.cpp-zstd.hman")
	skip := []byte{0x50, 0x2A, 0x4D, 0x18, 3, 0, 0, 0, 'h', 'm', 'n'}
	b := append(append(append([]byte(nil), z[:manifest.HeaderSize]...), skip...), z[manifest.HeaderSize:]...)
	binary.LittleEndian.PutUint64(b[56:], uint64(len(b)-manifest.HeaderSize))
	h := cdc.Sum(b[:manifest.SignedBytes])
	copy(b[256:288], h[:])
	m, err := manifest.Parse(b, 0)
	if err != nil || !equalManifests(m, pipeline(t)) {
		t.Fatalf("a leading skippable frame: %v", err)
	}
}
