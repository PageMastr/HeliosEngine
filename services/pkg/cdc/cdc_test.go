package cdc_test

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/crypto/blake2b"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/cdc/cdctest"
)

// The shared vectors: doctest (engine/patch/tests/test_fastcdc.cpp) checks the same file, so a divergence
// between the Go and C++ chunkers fails both suites.
var vectorsFile = filepath.Join("..", "..", "testdata", "vectors", "fastcdc.json")

var update = flag.Bool("update", false, "rewrite testdata/vectors/fastcdc.json from this implementation")

type vectorChunk struct {
	Offset uint64 `json:"offset"`
	Size   uint32 `json:"size"`
	Hash   string `json:"hash"`
}

type vectorCase struct {
	Name     string        `json:"name"`
	Input    cdctest.Input `json:"input"`
	Size     uint64        `json:"size"`
	FileHash string        `json:"fileHash"`
	Chunks   []vectorChunk `json:"chunks"`
}

type vectorFile struct {
	Comment    string       `json:"comment"`
	MinSize    int          `json:"minSize"`
	AvgSize    int          `json:"avgSize"`
	MaxSize    int          `json:"maxSize"`
	MaskS      string       `json:"maskS"`
	MaskL      string       `json:"maskL"`
	GearSeed   string       `json:"gearSeed"`
	GearDigest string       `json:"gearDigest"`
	Gear       []string     `json:"gear"`
	Cases      []vectorCase `json:"cases"`
}

// The cases -update writes. Edge files (empty, one byte, below and at the minimum, at and past the maximum,
// all zero) and seeded random inputs, a periodic input (identical chunks) and an insertion (resync).
var caseInputs = []struct {
	name string
	in   cdctest.Input
}{
	{"empty", cdctest.Input{Kind: "random", Seed: 1, Size: 0}},
	{"one-byte", cdctest.Input{Kind: "random", Seed: 1, Size: 1}},
	{"below-min", cdctest.Input{Kind: "random", Seed: 1, Size: 10000}},
	{"exactly-min", cdctest.Input{Kind: "random", Seed: 1, Size: cdc.MinSize}},
	{"min-plus-one", cdctest.Input{Kind: "random", Seed: 1, Size: cdc.MinSize + 1}},
	{"exactly-max", cdctest.Input{Kind: "random", Seed: 1, Size: cdc.MaxSize}},
	{"max-plus-one", cdctest.Input{Kind: "random", Seed: 1, Size: cdc.MaxSize + 1}},
	{"zero-exactly-max", cdctest.Input{Kind: "zero", Size: cdc.MaxSize}},
	{"zero-max-plus-one", cdctest.Input{Kind: "zero", Size: cdc.MaxSize + 1}},
	{"zero-1mib", cdctest.Input{Kind: "zero", Size: 1 << 20}},
	{"random-1mib", cdctest.Input{Kind: "random", Seed: 2, Size: 1 << 20}},
	{"random-8mib", cdctest.Input{Kind: "random", Seed: 3, Size: 8 << 20}},
	{"repeat-4093-2mib", cdctest.Input{Kind: "repeat", Seed: 5, Size: 2 << 20, Period: 4093}},
	{"random-4mib", cdctest.Input{Kind: "random", Seed: 4, Size: 4 << 20}},
	{"random-4mib-insert-17", cdctest.Input{Kind: "insert", Seed: 4, Size: 4 << 20, At: 1000000, InsertSize: 17, InsertSeed: 6}},
	// Crafted (found by searching the last bytes before the target): the mask switch and the start of
	// hashing. The earliest possible MaskS match is at byte 16386 (no gear entry, and no two, has the top 18
	// bits zero), the last byte MaskS covers is 65535 and the first MaskL covers is 65536.
	{"masks-match-at-16386", cdctest.Input{Kind: "random", Seed: 100, Size: 300000,
		Edits: []cdctest.Edit{{At: 16384, Hex: "0e446d"}}}}, // cut at 16387
	{"maskl-only-match-at-65535", cdctest.Input{Kind: "random", Seed: 100, Size: 300000,
		Edits: []cdctest.Edit{{At: 65534, Hex: "541f"}}}}, // no cut there: MaskS still applies
	{"maskl-only-match-at-65536", cdctest.Input{Kind: "random", Seed: 100, Size: 300000,
		Edits: []cdctest.Edit{{At: 65535, Hex: "13fc"}}}}, // cut at 65537
}

func gearDigest() string {
	g := cdc.Gear()
	var b [256 * 8]byte
	for i, v := range g {
		binary.LittleEndian.PutUint64(b[i*8:], v)
	}
	d := blake2b.Sum256(b[:])
	return hex.EncodeToString(d[:])
}

func toVector(chunks []cdc.Chunk) []vectorChunk {
	out := make([]vectorChunk, 0, len(chunks))
	for _, c := range chunks {
		out = append(out, vectorChunk{Offset: c.Offset, Size: c.Size, Hash: c.Hash.String()})
	}
	return out
}

// writeVectors writes the file with one chunk per line, so a diff shows exactly which boundary moved.
func writeVectors(t *testing.T, f vectorFile) {
	t.Helper()
	var b strings.Builder
	head, err := json.Marshal(struct {
		Comment    string   `json:"comment"`
		MinSize    int      `json:"minSize"`
		AvgSize    int      `json:"avgSize"`
		MaxSize    int      `json:"maxSize"`
		MaskS      string   `json:"maskS"`
		MaskL      string   `json:"maskL"`
		GearSeed   string   `json:"gearSeed"`
		GearDigest string   `json:"gearDigest"`
		Gear       []string `json:"gear"`
	}{f.Comment, f.MinSize, f.AvgSize, f.MaxSize, f.MaskS, f.MaskL, f.GearSeed, f.GearDigest, f.Gear})
	if err != nil {
		t.Fatal(err)
	}
	b.Write(head[:len(head)-1])
	b.WriteString(",\n\"cases\": [\n")
	for i, c := range f.Cases {
		in, _ := json.Marshal(c.Input)
		fmt.Fprintf(&b, "{\"name\": %q, \"input\": %s, \"size\": %d, \"fileHash\": %q, \"chunks\": [", c.Name, in, c.Size, c.FileHash)
		for j, ch := range c.Chunks {
			sep := ","
			if j == len(c.Chunks)-1 {
				sep = ""
			}
			fmt.Fprintf(&b, "\n  {\"offset\": %d, \"size\": %d, \"hash\": %q}%s", ch.Offset, ch.Size, ch.Hash, sep)
		}
		sep := ","
		if i == len(f.Cases)-1 {
			sep = ""
		}
		fmt.Fprintf(&b, "]}%s\n", sep)
	}
	b.WriteString("]}\n")
	if err := os.WriteFile(vectorsFile, []byte(b.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}

func loadVectors(t *testing.T) vectorFile {
	t.Helper()
	raw, err := os.ReadFile(vectorsFile)
	if err != nil {
		t.Fatal(err)
	}
	var f vectorFile
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	return f
}

func TestUpdateVectors(t *testing.T) {
	if !*update {
		t.Skip("run with -update to rewrite the shared vectors")
	}
	g := cdc.Gear()
	f := vectorFile{
		Comment: "FastCDC shared vectors (WP-0.16): Go pkg/cdc and C++ engine/patch must both reproduce every " +
			"chunk. Inputs per services/pkg/cdc/cdctest; algorithm in engine/patch/README.md. Regenerate with " +
			"go test ./pkg/cdc -run TestUpdateVectors -update.",
		MinSize: cdc.MinSize, AvgSize: cdc.AvgSize, MaxSize: cdc.MaxSize,
		MaskS:      fmt.Sprintf("%016x", cdc.MaskS),
		MaskL:      fmt.Sprintf("%016x", cdc.MaskL),
		GearSeed:   fmt.Sprintf("%016x", cdc.GearSeed),
		GearDigest: gearDigest(),
	}
	for _, v := range g {
		f.Gear = append(f.Gear, fmt.Sprintf("%016x", v))
	}
	for _, c := range caseInputs {
		data, err := c.in.Bytes()
		if err != nil {
			t.Fatal(err)
		}
		f.Cases = append(f.Cases, vectorCase{Name: c.name, Input: c.in, Size: uint64(len(data)),
			FileHash: cdc.Sum(data).String(), Chunks: toVector(cdc.Split(data))})
	}
	writeVectors(t, f)
}

// choppyReader returns reads of varying sizes (1 byte up to ~200 KB), so the Chunker's buffering is
// exercised at every alignment.
type choppyReader struct {
	data []byte
	s    cdc.SplitMix64
}

func (r *choppyReader) Read(p []byte) (int, error) {
	if len(r.data) == 0 {
		return 0, io.EOF
	}
	n := int(r.s.Next()%200000) + 1
	if r.s.Next()%4 == 0 {
		n = int(r.s.Next()%7) + 1
	}
	n = min(n, len(p), len(r.data))
	copy(p, r.data[:n])
	r.data = r.data[n:]
	return n, nil
}

func TestSharedVectors(t *testing.T) {
	f := loadVectors(t)
	if f.MinSize != cdc.MinSize || f.AvgSize != cdc.AvgSize || f.MaxSize != cdc.MaxSize ||
		f.MaskS != fmt.Sprintf("%016x", cdc.MaskS) || f.MaskL != fmt.Sprintf("%016x", cdc.MaskL) ||
		f.GearSeed != fmt.Sprintf("%016x", cdc.GearSeed) {
		t.Fatalf("vector parameters differ from the implementation's")
	}
	if f.GearDigest != gearDigest() || len(f.Gear) != 256 {
		t.Fatalf("gear table digest %s, vectors say %s", gearDigest(), f.GearDigest)
	}
	g := cdc.Gear()
	for i, s := range f.Gear {
		if s != fmt.Sprintf("%016x", g[i]) {
			t.Fatalf("gear[%d] = %016x, vectors say %s", i, g[i], s)
		}
	}
	if len(f.Cases) != len(caseInputs) {
		t.Fatalf("%d cases in the file, %d in the test", len(f.Cases), len(caseInputs))
	}
	for _, c := range f.Cases {
		t.Run(c.Name, func(t *testing.T) {
			data, err := c.Input.Bytes()
			if err != nil {
				t.Fatal(err)
			}
			if uint64(len(data)) != c.Size || cdc.Sum(data).String() != c.FileHash {
				t.Fatalf("generated input differs: %d bytes, hash %s", len(data), cdc.Sum(data))
			}
			got := toVector(cdc.Split(data))
			if len(got) != len(c.Chunks) {
				t.Fatalf("%d chunks, vectors say %d", len(got), len(c.Chunks))
			}
			for i := range got {
				if got[i] != c.Chunks[i] {
					t.Fatalf("chunk %d: got %+v, vectors say %+v", i, got[i], c.Chunks[i])
				}
			}
			// The streaming chunker agrees, whatever the read sizes.
			file, err := cdc.ChunkReader(&choppyReader{data: data, s: cdc.SplitMix64{State: uint64(len(data))}})
			if err != nil {
				t.Fatal(err)
			}
			if file.Size != c.Size || file.Hash.String() != c.FileHash {
				t.Fatalf("ChunkReader: size %d hash %s", file.Size, file.Hash)
			}
			stream := toVector(file.Chunks)
			if len(stream) != len(got) {
				t.Fatalf("ChunkReader: %d chunks, Split %d", len(stream), len(got))
			}
			for i := range got {
				if stream[i] != got[i] {
					t.Fatalf("ChunkReader chunk %d: %+v, Split %+v", i, stream[i], got[i])
				}
			}
		})
	}
}

// Every chunk but the last is in [MinSize+1, MaxSize]; the last is in [1, MaxSize]; chunks tile the input.
func TestChunkBounds(t *testing.T) {
	for seed := uint64(100); seed < 108; seed++ {
		data := cdctest.Random(seed, 3<<20+int(seed)*7919)
		var off int
		lens := cdc.Boundaries(data)
		for i, n := range lens {
			if n < 1 || n > cdc.MaxSize || (i < len(lens)-1 && n <= cdc.MinSize) {
				t.Fatalf("seed %d chunk %d has %d bytes", seed, i, n)
			}
			off += n
		}
		if off != len(data) {
			t.Fatalf("seed %d: chunks cover %d of %d bytes", seed, off, len(data))
		}
	}
	if cdc.Cut(nil) != 0 || len(cdc.Split(nil)) != 0 || len(cdc.Boundaries([]byte{})) != 0 {
		t.Fatal("empty input must have no chunks")
	}
}

// The mean chunk size on random data lands near the normal size (normalized chunking, level 2).
func TestMeanChunkSize(t *testing.T) {
	data := cdctest.Random(42, 64<<20)
	lens := cdc.Boundaries(data)
	mean := float64(len(data)) / float64(len(lens))
	if mean < 48*1024 || mean > 96*1024 {
		t.Fatalf("mean chunk size %.0f bytes over %d chunks, want 48–96 KiB", mean, len(lens))
	}
	t.Logf("mean chunk size %.0f bytes over %d chunks", mean, len(lens))
}

// An insertion changes only the chunks around it: content-defined boundaries resynchronize.
func TestInsertResyncs(t *testing.T) {
	base := cdctest.Random(7, 16<<20)
	edited, _ := cdctest.Input{Kind: "insert", Seed: 7, Size: 16 << 20, At: 5 << 20, InsertSize: 100, InsertSeed: 8}.Bytes()
	have := map[cdc.Hash]bool{}
	for _, c := range cdc.Split(base) {
		have[c.Hash] = true
	}
	var changed int
	for _, c := range cdc.Split(edited) {
		if !have[c.Hash] {
			changed++
		}
	}
	if changed < 1 || changed > 3 {
		t.Fatalf("%d chunks changed after a 100-byte insertion, want 1–3", changed)
	}
}

type failingReader struct{ n int }

func (r *failingReader) Read(p []byte) (int, error) {
	if r.n <= 0 {
		return 0, errors.New("disk on fire")
	}
	n := min(len(p), r.n)
	r.n -= n
	return n, nil
}

type stuckReader struct{}

func (stuckReader) Read([]byte) (int, error) { return 0, nil }

type liarReader struct{}

func (liarReader) Read(p []byte) (int, error) { return len(p) + 1, nil }

func TestChunkerErrors(t *testing.T) {
	_, err := cdc.ChunkReader(&failingReader{n: 3 * cdc.MaxSize})
	if err == nil || !strings.Contains(err.Error(), "disk on fire") {
		t.Fatalf("read error not reported: %v", err)
	}
	c := cdc.NewChunker(&failingReader{n: 10})
	for i := 0; i < 2; i++ {
		if _, _, err := c.Next(); err == nil || err == io.EOF {
			t.Fatalf("call %d: error not sticky: %v", i, err)
		}
	}
	if _, err := cdc.ChunkReader(stuckReader{}); !errors.Is(err, io.ErrNoProgress) {
		t.Fatalf("a reader that never progresses: %v", err)
	}
	if _, err := cdc.ChunkReader(liarReader{}); err == nil {
		t.Fatal("an invalid read count was accepted")
	}
	f, err := cdc.ChunkReader(bytes.NewReader(nil))
	if err != nil || f.Size != 0 || len(f.Chunks) != 0 || f.Hash != cdc.Sum(nil) {
		t.Fatalf("empty stream: %+v %v", f, err)
	}
}

// Chunker.Next's bytes are the chunk's bytes.
func TestChunkerBytes(t *testing.T) {
	data := cdctest.Random(9, 2<<20)
	c := cdc.NewChunker(&choppyReader{data: data, s: cdc.SplitMix64{State: 1}})
	for {
		ch, b, err := c.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(b, data[ch.Offset:ch.Offset+uint64(ch.Size)]) || cdc.Sum(b) != ch.Hash {
			t.Fatalf("chunk at %d: bytes differ", ch.Offset)
		}
	}
}

// FuzzChunker: the streaming chunker agrees with Split for any input and read pattern, and chunks obey
// the size bounds.
func FuzzChunker(f *testing.F) {
	f.Add([]byte{}, uint64(0))
	f.Add(cdctest.Random(1, cdc.MaxSize+1), uint64(1))
	f.Add(make([]byte, 3*cdc.MaxSize), uint64(2))
	f.Fuzz(func(t *testing.T, data []byte, seed uint64) {
		want := cdc.Split(data)
		file, err := cdc.ChunkReader(&choppyReader{data: data, s: cdc.SplitMix64{State: seed}})
		if err != nil || len(file.Chunks) != len(want) || file.Size != uint64(len(data)) {
			t.Fatalf("ChunkReader: %d chunks, Split %d, err %v", len(file.Chunks), len(want), err)
		}
		var off uint64
		for i, c := range want {
			if file.Chunks[i] != c || c.Offset != off || c.Size == 0 || c.Size > cdc.MaxSize ||
				(i < len(want)-1 && c.Size <= cdc.MinSize) {
				t.Fatalf("chunk %d: %+v / %+v", i, c, file.Chunks[i])
			}
			off += uint64(c.Size)
		}
	})
}
