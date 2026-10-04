package cdc_test

import (
	"os"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/cdc/cdctest"
)

// Budgets (engine/patch/README.md "Performance", the same for Go and C++): boundary detection
// ≥ 1,000 MB/s per core (R07 §7 cites FastCDC at "> 1 GB/s/core") and chunking with BLAKE2b-256 IDs
// ≥ 250 MB/s per core. go test runs packages in parallel, so a plain run asserts only a quarter of each
// budget (a gross-regression floor); HELIOS_PERF=1 asserts the budgets themselves on a quiet machine.
const (
	budgetCutMBps   = 1000.0
	budgetSplitMBps = 250.0
)

func bestOf(runs int, f func()) time.Duration {
	best := time.Duration(1<<63 - 1)
	for i := 0; i < runs; i++ {
		t0 := time.Now()
		f()
		best = min(best, time.Since(t0))
	}
	return best
}

func TestPerfChunking(t *testing.T) {
	if testing.Short() || raceEnabled {
		t.Skip("timing test: not under -short or the race detector")
	}
	data := cdctest.Random(1, 32<<20)
	mb := float64(len(data)) / 1e6
	cut := mb / bestOf(5, func() { _ = cdc.Boundaries(data) }).Seconds()
	split := mb / bestOf(3, func() { _ = cdc.Split(data) }).Seconds()
	t.Logf("boundaries %.0f MB/s, split + BLAKE2b %.0f MB/s", cut, split)
	scale := 0.25
	if os.Getenv("HELIOS_PERF") == "1" {
		scale = 1
	}
	if cut < budgetCutMBps*scale || split < budgetSplitMBps*scale {
		t.Fatalf("below %.0f%% of the budget (%.0f and %.0f MB/s)", scale*100, budgetCutMBps, budgetSplitMBps)
	}
}

func BenchmarkBoundaries(b *testing.B) {
	data := cdctest.Random(1, 16<<20)
	b.SetBytes(int64(len(data)))
	for i := 0; i < b.N; i++ {
		_ = cdc.Boundaries(data)
	}
}

func BenchmarkSplit(b *testing.B) {
	data := cdctest.Random(1, 16<<20)
	b.SetBytes(int64(len(data)))
	for i := 0; i < b.N; i++ {
		_ = cdc.Split(data)
	}
}
