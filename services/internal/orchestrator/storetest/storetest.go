// Package storetest is the conformance suite for orchestrator.Store implementations. Unit tests
// run it against MemStore; the integration build runs it against PostgreSQL.
package storetest

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
	"github.com/PageMastr/scifi-test/services/pkg/idgen"
)

// Factory returns a fresh, empty store.
type Factory func(t *testing.T) orchestrator.Store

const shard = "st"

// lead takes the shard's leadership (the store is fresh, so it is free) and returns the fence.
func lead(t *testing.T, s orchestrator.Store) orchestrator.Fence {
	t.Helper()
	term, ok, err := s.AcquireLeadership(context.Background(), shard, "holder-a", time.Minute)
	if err != nil || !ok {
		t.Fatalf("acquire: %v %v", ok, err)
	}
	return orchestrator.Fence{Shard: shard, Term: term}
}

// Run executes the suite.
func Run(t *testing.T, newStore Factory) {
	ctx := context.Background()
	now := time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC)

	t.Run("leadership", func(t *testing.T) {
		s := newStore(t)
		t1, ok, err := s.AcquireLeadership(ctx, shard, "a", 200*time.Millisecond)
		if err != nil || !ok || t1 != 1 {
			t.Fatalf("first acquire: %d %v %v", t1, ok, err)
		}
		if _, ok, _ := s.AcquireLeadership(ctx, shard, "b", time.Second); ok {
			t.Fatal("b took a live lease")
		}
		if ok, err := s.RenewLeadership(ctx, shard, "a", t1, 200*time.Millisecond); err != nil || !ok {
			t.Fatalf("renew: %v %v", ok, err)
		}
		if ok, _ := s.RenewLeadership(ctx, shard, "b", t1, time.Second); ok {
			t.Fatal("b renewed a's term")
		}
		// The same holder (a restarted instance) reclaims its own lease at once, under a new term.
		t2, ok, _ := s.AcquireLeadership(ctx, shard, "a", 200*time.Millisecond)
		if !ok || t2 != t1+1 {
			t.Fatalf("reclaim: %d %v", t2, ok)
		}
		if ok, _ := s.RenewLeadership(ctx, shard, "a", t1, time.Second); ok {
			t.Fatal("an old term renewed")
		}
		// Expiry hands the lease to b.
		time.Sleep(600 * time.Millisecond)
		t3, ok, _ := s.AcquireLeadership(ctx, shard, "b", time.Minute)
		if !ok || t3 != t2+1 {
			t.Fatalf("takeover after expiry: %d %v", t3, ok)
		}
		if ok, _ := s.RenewLeadership(ctx, shard, "a", t2, time.Second); ok {
			t.Fatal("deposed leader renewed")
		}
		// Release lets a successor in without waiting; releasing someone else's term does nothing.
		if err := s.ReleaseLeadership(ctx, shard, "a", t2); err != nil {
			t.Fatal(err)
		}
		if _, ok, _ := s.AcquireLeadership(ctx, shard, "c", time.Minute); ok {
			t.Fatal("a released b's lease")
		}
		if err := s.ReleaseLeadership(ctx, shard, "b", t3); err != nil {
			t.Fatal(err)
		}
		if t4, ok, _ := s.AcquireLeadership(ctx, shard, "c", time.Minute); !ok || t4 != t3+1 {
			t.Fatalf("after release: %d %v", t4, ok)
		}
		// Shards are independent.
		if _, ok, _ := s.AcquireLeadership(ctx, "other", "a", time.Minute); !ok {
			t.Fatal("other shard")
		}
	})

	t.Run("writes are fenced by term", func(t *testing.T) {
		s := newStore(t)
		f := lead(t, s)
		if err := s.EnsureZones(ctx, f, []orchestrator.Zone{{ID: 1, Name: "a"}}, now); err != nil {
			t.Fatal(err)
		}
		stale := orchestrator.Fence{Shard: shard, Term: f.Term + 1}
		none := orchestrator.Fence{Shard: "no-leader", Term: 1}
		for name, op := range map[string]func(orchestrator.Fence) error{
			"EnsureZones": func(f orchestrator.Fence) error {
				return s.EnsureZones(ctx, f, []orchestrator.Zone{{ID: 2, Name: "b"}}, now)
			},
			"ResetOwners": func(f orchestrator.Fence) error { return s.ResetOwners(ctx, f, now) },
			"CreateProcess": func(f orchestrator.Fence) error {
				_, err := s.CreateProcess(ctx, f, orchestrator.ProcessInfo{Name: "x", Kind: orchestrator.KindCell}, now)
				return err
			},
			"EndProcess": func(f orchestrator.Fence) error { return s.EndProcess(ctx, f, 1, "x", now) },
			"AssignZone": func(f orchestrator.Fence) error {
				_, err := s.AssignZone(ctx, f, 1, 7, now)
				return err
			},
			"ReleaseZone": func(f orchestrator.Fence) error { return s.ReleaseZone(ctx, f, 1, 7, now) },
		} {
			for _, bad := range []orchestrator.Fence{stale, none} {
				if err := op(bad); !errors.Is(err, orchestrator.ErrNotLeader) {
					t.Errorf("%s with fence %+v: %v", name, bad, err)
				}
			}
		}
		zs, _ := s.ListZones(ctx)
		if len(zs) != 1 || zs[0].Owner != 0 || zs[0].LeaseGen != 0 {
			t.Fatalf("fenced writes changed state: %+v", zs)
		}
	})

	t.Run("zones and generations", func(t *testing.T) {
		s := newStore(t)
		f := lead(t, s)
		if err := s.EnsureZones(ctx, f, []orchestrator.Zone{{ID: 2, Name: "b"}, {ID: 1, Name: "a"}}, now); err != nil {
			t.Fatal(err)
		}
		if err := s.EnsureZones(ctx, f, []orchestrator.Zone{{ID: 1, Name: "a"}}, now); err != nil {
			t.Fatal("EnsureZones must be idempotent:", err)
		}
		zs, err := s.ListZones(ctx)
		if err != nil || len(zs) != 2 || zs[0].ID != 1 || zs[1].Name != "b" || zs[0].Owner != 0 || zs[0].LeaseGen != 0 {
			t.Fatalf("list: %+v %v", zs, err)
		}
		g1, err := s.AssignZone(ctx, f, 1, 100, now)
		if err != nil || g1 != 1 {
			t.Fatalf("assign: %d %v", g1, err)
		}
		// Release by a non-owner is a no-op.
		if err := s.ReleaseZone(ctx, f, 1, 999, now); err != nil {
			t.Fatal(err)
		}
		zs, _ = s.ListZones(ctx)
		if zs[0].Owner != 100 {
			t.Fatal("non-owner released the zone")
		}
		if err := s.ReleaseZone(ctx, f, 1, 100, now); err != nil {
			t.Fatal(err)
		}
		g2, _ := s.AssignZone(ctx, f, 1, 101, now)
		if g2 != 2 {
			t.Fatalf("generation must grow on every assignment: %d", g2)
		}
		if _, err := s.AssignZone(ctx, f, 77, 1, now); !errors.Is(err, orchestrator.ErrUnknownZone) {
			t.Fatalf("unknown zone: %v", err)
		}
		// A new leader clears owners but keeps generations.
		if err := s.ResetOwners(ctx, f, now); err != nil {
			t.Fatal(err)
		}
		zs, _ = s.ListZones(ctx)
		if zs[0].Owner != 0 || zs[0].LeaseGen != 2 {
			t.Fatalf("reset: %+v", zs[0])
		}
		if g3, _ := s.AssignZone(ctx, f, 1, 102, now); g3 != 3 {
			t.Fatalf("after reset: %d", g3)
		}
	})

	t.Run("process epochs", func(t *testing.T) {
		s := newStore(t)
		f := lead(t, s)
		a1, err := s.CreateProcess(ctx, f, orchestrator.ProcessInfo{Name: "cell-a", Kind: orchestrator.KindCell}, now)
		if err != nil || a1.Epoch != 1 {
			t.Fatalf("first: %+v %v", a1, err)
		}
		b1, _ := s.CreateProcess(ctx, f, orchestrator.ProcessInfo{Name: "cell-b", Kind: orchestrator.KindCell}, now)
		a2, _ := s.CreateProcess(ctx, f, orchestrator.ProcessInfo{Name: "cell-a", Kind: orchestrator.KindCell}, now)
		if b1.Epoch != 1 || a2.Epoch != 2 || a2.ID == a1.ID || b1.ID == a1.ID {
			t.Fatalf("epochs: %+v %+v %+v", a1, b1, a2)
		}
		if err := s.EndProcess(ctx, f, a1.ID, "superseded", now); err != nil {
			t.Fatal(err)
		}
		// Concurrent registrations of one name get distinct epochs.
		var wg sync.WaitGroup
		epochs := make(chan int64, 10)
		for i := 0; i < 10; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				r, err := s.CreateProcess(ctx, f, orchestrator.ProcessInfo{Name: "cell-c", Kind: orchestrator.KindCell}, now)
				if err != nil {
					t.Error(err)
					return
				}
				epochs <- r.Epoch
			}()
		}
		wg.Wait()
		close(epochs)
		seen := map[int64]bool{}
		for e := range epochs {
			if seen[e] {
				t.Fatalf("epoch %d issued twice", e)
			}
			seen[e] = true
		}
		if len(seen) != 10 {
			t.Fatalf("%d epochs", len(seen))
		}
		if err := s.Ping(ctx); err != nil {
			t.Fatal(err)
		}
	})

	t.Run("id blocks never repeat", func(t *testing.T) {
		s := newStore(t)
		wall := idgen.PrefixAt(time.Now())
		first, err := s.AllocateIdBlocks(ctx, 3, 2)
		if err != nil || len(first) != 2 || first[1] != first[0]+1 {
			t.Fatalf("first: %v %v", first, err)
		}
		// The prefix follows the (store's) wall clock: within a minute of ours.
		if d := first[1] - wall; d < -60_000 || d > 60_000 {
			t.Fatalf("prefix %d is %d ms away from the wall clock", first[1], d)
		}
		other, _ := s.AllocateIdBlocks(ctx, 4, 1)
		if len(other) != 1 {
			t.Fatal("other shard")
		}
		var mu sync.Mutex
		seen := map[int64]bool{first[0]: true, first[1]: true}
		var wg sync.WaitGroup
		for g := 0; g < 8; g++ {
			wg.Add(1)
			go func(g int) {
				defer wg.Done()
				for i := 0; i < 10; i++ {
					ps, err := s.AllocateIdBlocks(ctx, 3, 1+(g+i)%idgen.MaxBlocksPerCall)
					if err != nil {
						t.Error(err)
						return
					}
					mu.Lock()
					for j, p := range ps {
						if seen[p] || (j > 0 && p != ps[j-1]+1) {
							t.Errorf("prefix %d repeated or blocks not contiguous: %v", p, ps)
						}
						seen[p] = true
					}
					mu.Unlock()
				}
			}(g)
		}
		wg.Wait()
		for _, bad := range [][2]int{{3, 0}, {3, 17}, {32, 1}, {-1, 1}} {
			if _, err := s.AllocateIdBlocks(ctx, bad[0], bad[1]); err == nil {
				t.Errorf("AllocateIdBlocks(%d, %d) accepted", bad[0], bad[1])
			}
		}
	})
}
