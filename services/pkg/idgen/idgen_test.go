package idgen

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"os"
	"strconv"
	"sync"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
)

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

func mustInt(t *testing.T, s string) int64 {
	t.Helper()
	v, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func TestGoldenVectors(t *testing.T) {
	var file struct {
		EpochUnixMs string `json:"epoch_unix_ms"`
		BlockSize   int    `json:"block_size"`
		Vectors     []struct {
			Prefix string `json:"prefix"`
			Shard  int    `json:"shard"`
			Offset int    `json:"offset"`
			ID     string `json:"id"`
		} `json:"vectors"`
		Allocations []struct {
			Last     string   `json:"last"`
			N        int      `json:"n"`
			Now      string   `json:"now"`
			NewLast  string   `json:"new_last"`
			Prefixes []string `json:"prefixes"`
		} `json:"allocations"`
	}
	b, err := os.ReadFile("../../testdata/vectors/block_ids.json")
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(b, &file); err != nil {
		t.Fatal(err)
	}
	if strconv.FormatInt(Epoch.UnixMilli(), 10) != file.EpochUnixMs || file.BlockSize != BlockSize {
		t.Fatalf("epoch or block size drifted: %d %d", Epoch.UnixMilli(), BlockSize)
	}
	for _, v := range file.Vectors {
		prefix, want := mustInt(t, v.Prefix), mustInt(t, v.ID)
		if got := Compose(prefix, v.Shard, v.Offset); got != want {
			t.Errorf("Compose(%d,%d,%d) = %d, want %d", prefix, v.Shard, v.Offset, got, want)
		}
		p := Decode(want)
		if p.Prefix != prefix || p.Shard != v.Shard || p.Offset != v.Offset || p.Time.Sub(Epoch).Milliseconds() != prefix {
			t.Errorf("Decode(%d) = %+v", want, p)
		}
		if want < 0 {
			t.Errorf("id %d is negative", want)
		}
	}
	for _, a := range file.Allocations {
		newLast, got := Allocate(mustInt(t, a.Last), a.N, mustInt(t, a.Now))
		if newLast != mustInt(t, a.NewLast) || len(got) != len(a.Prefixes) {
			t.Fatalf("Allocate(%s, %d, %s) = %d %v", a.Last, a.N, a.Now, newLast, got)
		}
		for i := range got {
			if got[i] != mustInt(t, a.Prefixes[i]) {
				t.Fatalf("Allocate(%s, %d, %s) prefixes %v", a.Last, a.N, a.Now, got)
			}
		}
	}
}

func TestMemSourceNeverRepeatsPrefixes(t *testing.T) {
	clk := clock.NewFake(Epoch.Add(time.Hour))
	src := NewMemSource(clk)
	seen := map[int64]bool{}
	last := int64(-1)
	for i := 0; i < 50; i++ {
		if i == 20 {
			clk.Set(Epoch.Add(time.Minute)) // the clock steps back: prefixes still increase
		}
		ps, err := src.AllocateIdBlocks(context.Background(), 1+i%MaxBlocksPerCall)
		if err != nil {
			t.Fatal(err)
		}
		for _, p := range ps {
			if seen[p] || p <= last {
				t.Fatalf("prefix %d repeated or not increasing (last %d)", p, last)
			}
			seen[p], last = true, p
		}
	}
	if _, err := src.AllocateIdBlocks(context.Background(), 17); err == nil {
		t.Fatal("n > 16 accepted")
	}
}

func newMinter(t *testing.T, src Source, clk clock.Clock, hold int) *Minter {
	t.Helper()
	m, err := NewMinter(3, src, Options{Hold: hold, Clock: clk, Log: quiet})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(m.Close)
	return m
}

// waitIdle waits until no refill is in flight (refills run in the background).
func waitIdle(m *Minter) {
	for {
		m.mu.Lock()
		ch := m.refilling
		m.mu.Unlock()
		if ch == nil {
			return
		}
		<-ch
	}
}

func TestMinterLayoutAndRange(t *testing.T) {
	if _, err := NewMinter(32, NewMemSource(nil), Options{}); err == nil {
		t.Fatal("shard 32 accepted")
	}
	if _, err := NewMinter(-1, NewMemSource(nil), Options{}); err == nil {
		t.Fatal("negative shard accepted")
	}
	if _, err := NewMinter(0, nil, Options{}); err == nil {
		t.Fatal("nil source accepted")
	}
	clk := clock.NewFake(Epoch.Add(22809600000 * time.Millisecond))
	m := newMinter(t, NewMemSource(clk), clk, 2)
	if err := m.Prime(context.Background()); err != nil {
		t.Fatal(err)
	}
	id, err := m.Next()
	if err != nil {
		t.Fatal(err)
	}
	p := Decode(id)
	if p.Shard != 3 || p.Offset != 0 || p.Prefix != 22809600000-1 {
		t.Fatalf("first id %d decodes to %+v", id, p)
	}
	if m.Remaining() != 2*BlockSize-1 {
		t.Fatalf("remaining %d", m.Remaining())
	}
}

func TestMinterRefillsAtHalfAndStaysMonotonic(t *testing.T) {
	clk := clock.NewFake(Epoch.Add(24 * time.Hour))
	src := NewMemSource(clk)
	m := newMinter(t, src, clk, 2)
	prev := int64(-1)
	seen := make(map[int64]struct{}, 3*BlockSize)
	for i := 0; i < 3*BlockSize; i++ {
		id, err := m.Next()
		if err != nil {
			t.Fatalf("id %d: %v", i, err)
		}
		if id <= prev {
			t.Fatalf("id %d not increasing after %d", id, prev)
		}
		if _, dup := seen[id]; dup {
			t.Fatalf("duplicate %d", id)
		}
		seen[id], prev = struct{}{}, id
		if i == BlockSize/2 {
			// Half of the first block is used: a background refill tops the spares back up.
			waitIdle(m)
			if m.Remaining() <= BlockSize+BlockSize/2 {
				t.Fatalf("no refill at half use: remaining %d", m.Remaining())
			}
		}
		if i%BlockSize == BlockSize-1 {
			waitIdle(m) // keep the test deterministic: the refill always lands before exhaustion here
		}
	}
	if src.Calls > 4 {
		t.Fatalf("%d allocation calls for 3 blocks", src.Calls)
	}
}

func TestMinterRetiresOldBlocksButNeverStalls(t *testing.T) {
	clk := clock.NewFake(Epoch.Add(24 * time.Hour))
	src := NewMemSource(clk)
	m := newMinter(t, src, clk, 2)
	first, _ := m.Next()
	waitIdle(m)

	// An hour later the held blocks are retired: the next ID comes from a fresh block (after the
	// background refill) and its prefix is within the last hour.
	clk.Advance(61 * time.Minute)
	if _, err := m.Next(); err != nil { // triggers the refill; may still use the retired block
		t.Fatal(err)
	}
	waitIdle(m)
	id, _ := m.Next()
	if Decode(id).Time.Before(clk.Now().Add(-time.Minute)) || Decode(id).Prefix <= Decode(first).Prefix {
		t.Fatalf("retired block still used: %+v (now %v)", Decode(id), clk.Now())
	}

	// Control plane down: blocks retire, refills fail, minting continues on retired blocks.
	src.mu.Lock()
	src.Fail = errors.New("orchestrator unreachable")
	src.mu.Unlock()
	clk.Advance(2 * time.Hour)
	for i := 0; i < 1000; i++ {
		if _, err := m.Next(); err != nil {
			t.Fatalf("minter stalled during an outage: %v", err)
		}
	}
	waitIdle(m)
}

func TestMinterWithoutBlocksReportsSourceError(t *testing.T) {
	src := NewMemSource(nil)
	src.Fail = errors.New("db down")
	m := newMinter(t, src, nil, 2)
	if _, err := m.Next(); !errors.Is(err, ErrNoBlock) {
		t.Fatalf("want ErrNoBlock, got %v", err)
	}
	src.mu.Lock()
	src.Fail = nil
	src.mu.Unlock()
	// The failed refill backs off for a second, but Next forces a synchronous retry.
	if _, err := m.Next(); err != nil {
		t.Fatalf("recovery: %v", err)
	}
	m.Close()
	if _, err := m.Next(); !errors.Is(err, ErrClosed) {
		t.Fatalf("after close: %v", err)
	}
}

func TestMinterRejectsBadSources(t *testing.T) {
	back := SourceFunc(func(context.Context, int) ([]int64, error) { return []int64{5, 4}, nil })
	if err := newMinter(t, back, nil, 2).Prime(context.Background()); err == nil {
		t.Fatal("decreasing prefixes accepted")
	}
	huge := SourceFunc(func(context.Context, int) ([]int64, error) { return []int64{MaxPrefix + 1}, nil })
	if err := newMinter(t, huge, nil, 1).Prime(context.Background()); !errors.Is(err, ErrPrefixOverflow) {
		t.Fatalf("overflow: %v", err)
	}
}

func TestMinterConcurrentUniqueness(t *testing.T) {
	src := NewMemSource(nil)
	// Two minters on the same source (two processes on one shard) plus concurrent callers.
	a := newMinter(t, src, nil, 2)
	b := newMinter(t, src, nil, 4)
	const per = 20000
	var mu sync.Mutex
	seen := make(map[int64]struct{}, 8*per)
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ {
		m := a
		if g%2 == 1 {
			m = b
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			local := make([]int64, 0, per)
			for i := 0; i < per; i++ {
				id, err := m.Next()
				if err != nil {
					t.Error(err)
					return
				}
				local = append(local, id)
			}
			mu.Lock()
			defer mu.Unlock()
			for _, id := range local {
				if _, dup := seen[id]; dup {
					t.Errorf("duplicate id %d", id)
					return
				}
				seen[id] = struct{}{}
			}
		}()
	}
	wg.Wait()
	if len(seen) != 8*per {
		t.Fatalf("%d unique ids, want %d", len(seen), 8*per)
	}
}
