package manifest_test

import (
	"encoding/binary"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
)

// Budgets (engine/patch/README.md "Performance", the same for Go and C++), one core:
//   - a 50 GB install's manifest (20,000 files, 700,000 chunks, a 52.6 MB body): read (BLAKE2b, decode,
//     validate) ≤ 400 ms and write (codec none) ≤ 400 ms;
//   - deep-paths.hman, the deepest paths the limits allow (64 MiB of paths 508 directories deep, a 72 MB
//     body): read ≤ 400 ms.
//
// As in pkg/cdc, go test runs packages in parallel, so a plain run asserts four times each budget (a
// gross-regression floor) and HELIOS_PERF=1 the budgets themselves on a quiet machine.
const budgetManifest = 400 * time.Millisecond

func budgetScale() time.Duration {
	if os.Getenv("HELIOS_PERF") == "1" {
		return 1
	}
	return 4
}

func bestOf(runs int, f func()) time.Duration {
	best := time.Duration(1<<63 - 1)
	for i := 0; i < runs; i++ {
		t0 := time.Now()
		f()
		best = min(best, time.Since(t0))
	}
	return best
}

// installManifest is the synthetic 50 GB install of engine/patch/tests/test_perf.cpp: 20,000 files of 35
// chunks each (60,000-89,999 bytes, about 74 KiB on average), with chunk IDs from SplitMix64(9).
func installManifest(tb testing.TB) *manifest.Manifest {
	tb.Helper()
	b := manifest.NewBuilder(manifest.Header{ProductID: "sample-game", Platform: "win64", BuildID: "perf"})
	rng := cdc.SplitMix64{State: 9}
	for f := 0; f < 20000; f++ {
		var content cdc.File
		for c := 0; c < 35; c++ {
			ch := cdc.Chunk{Offset: content.Size, Size: 60000 + uint32(rng.Next()%30000)}
			for k := 0; k < cdc.HashSize; k += 8 {
				binary.LittleEndian.PutUint64(ch.Hash[k:], rng.Next())
			}
			content.Size += uint64(ch.Size)
			content.Chunks = append(content.Chunks, ch)
		}
		in := manifest.FileInput{Path: fmt.Sprintf("content/group%03d/file%05d.hpak", f%500, f), Content: content,
			Tier: uint8(f % 3)}
		if err := b.AddFile(in); err != nil {
			tb.Fatal(err)
		}
	}
	m, err := b.Build()
	if err != nil {
		tb.Fatal(err)
	}
	return m
}

func TestPerfManifest(t *testing.T) {
	if testing.Short() || raceEnabled {
		t.Skip("timing test: not under -short or the race detector")
	}
	m := installManifest(t)
	var raw []byte
	write := bestOf(3, func() {
		var err error
		if raw, err = m.Marshal(manifest.WriteOptions{Codec: manifest.CodecNone}); err != nil {
			t.Fatal(err)
		}
	})
	var back *manifest.Manifest
	read := bestOf(3, func() {
		var err error
		if back, err = manifest.Parse(raw, 0); err != nil {
			t.Fatal(err)
		}
	})
	if !equalManifests(back, m) {
		t.Fatal("the manifest read back differs")
	}
	t.Logf("%d files, %d chunks, %.1f MB body: write %v, read %v", len(m.Files), len(m.Chunks),
		float64(len(raw))/1e6, write.Round(time.Millisecond), read.Round(time.Millisecond))
	if limit := budgetManifest * budgetScale(); write > limit || read > limit {
		t.Fatalf("above %v (%dx the %v budget)", limit, budgetScale(), budgetManifest)
	}
}

// The path-collision check is linear in path bytes plus a sort, so 64 MiB of paths 508 directories deep
// reads within the read budget and costs about what as many bytes of paths without directories cost.
// Looking up every '/'-prefix in a set (len²/4 per path) took 2.5 s here: the ratio fails it on any
// machine, the budget on a quiet one.
func TestPerfDeepPaths(t *testing.T) {
	if testing.Short() || raceEnabled {
		t.Skip("timing test: not under -short or the race detector")
	}
	read := func(file []byte) time.Duration {
		return bestOf(3, func() {
			if _, err := manifest.Parse(file, 0); err != nil {
				t.Fatal(err)
			}
		})
	}
	marshal := func(m *manifest.Manifest) []byte {
		z, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: 3})
		if err != nil {
			t.Fatal(err)
		}
		return z
	}
	deep := read(marshal(pathsManifest(deepPathFiles, true)))
	flat := read(marshal(pathsManifest(deepPathFiles, false)))
	vector := read(readGolden(t, "deep-paths.hman"))
	t.Logf("64 MiB of paths: 508 levels deep %v (deep-paths.hman %v), without directories %v",
		deep.Round(time.Millisecond), vector.Round(time.Millisecond), flat.Round(time.Millisecond))
	if limit := budgetManifest * budgetScale(); deep > limit || vector > limit {
		t.Fatalf("above %v (%dx the %v budget)", limit, budgetScale(), budgetManifest)
	}
	if deep > 2*flat {
		t.Fatalf("deep paths read %.1fx slower than flat ones of the same size (at most 2x)",
			float64(deep)/float64(flat))
	}
}

func BenchmarkParse(b *testing.B) {
	raw, err := installManifest(b).Marshal(manifest.WriteOptions{Codec: manifest.CodecNone})
	if err != nil {
		b.Fatal(err)
	}
	b.SetBytes(int64(len(raw)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := manifest.Parse(raw, 0); err != nil {
			b.Fatal(err)
		}
	}
}

func BenchmarkMarshal(b *testing.B) {
	m := installManifest(b)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		raw, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecNone})
		if err != nil {
			b.Fatal(err)
		}
		b.SetBytes(int64(len(raw)))
	}
}

func BenchmarkParseDeepPaths(b *testing.B) {
	file := readGolden(b, "deep-paths.hman")
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := manifest.Parse(file, 0); err != nil {
			b.Fatal(err)
		}
	}
}
