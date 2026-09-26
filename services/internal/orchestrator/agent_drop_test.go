package orchestrator_test

import (
	"context"
	"fmt"
	"slices"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/testkit"
)

// Every drop rule of the Agent's holder rule, each on an answer from the leader (PR #8 review,
// blocking 4): a region leaving the assignments, lease_lost, and a graceful stop drop it; a
// stale reply with a lower generation drops nothing.
func TestAgentDropsRegionsOnlyOnLeaderAnswers(t *testing.T) {
	bus := testkit.StartNATS(t)
	cp := startScripted(t, bus, "t2", []orchestrator.Assignment{
		{ZoneID: 1001, ZoneName: "alpha", LeaseGen: 3}, {ZoneID: 1002, ZoneName: "beta", LeaseGen: 5}})
	agent := orchestrator.NewAgent(bus.Connect(t, "cell"), "t2", orchestrator.ProcessInfo{Name: "cell-a", Kind: orchestrator.KindCell}, quiet)
	var fenced atomic.Int32
	agent.OnFence = func() { fenced.Add(1) }
	var mu sync.Mutex
	var lost []lostRegion
	agent.OnRegionLost = func(r orchestrator.HeldRegion, reason string) {
		mu.Lock()
		lost = append(lost, lostRegion{r, reason})
		mu.Unlock()
	}
	lostSoFar := func() []lostRegion { mu.Lock(); defer mu.Unlock(); return slices.Clone(lost) }
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { _ = agent.Run(ctx); close(done) }()
	stopped := false
	t.Cleanup(func() {
		if !stopped {
			cancel()
			<-done
		}
	})
	eventually(t, "both regions held", func() bool { return len(agent.Held()) == 2 })

	// Unassigned: the leader stops listing 1002.
	cp.setAssignments([]orchestrator.Assignment{{ZoneID: 1001, ZoneName: "alpha", LeaseGen: 3}})
	eventually(t, "unassigned region dropped", func() bool { return len(lostSoFar()) == 1 })
	if l := lostSoFar(); l[0] != (lostRegion{orchestrator.HeldRegion{Region: 1002, LeaseGen: 5}, orchestrator.LostUnassigned}) {
		t.Fatalf("lost %v", l)
	}
	if got := agent.Held(); !slices.Equal(got, []orchestrator.HeldRegion{{Region: 1001, LeaseGen: 3}}) {
		t.Fatalf("held: %v", got)
	}

	// Stale: a reply with a lower generation never lowers the one held, nor drops it.
	cp.setAssignments([]orchestrator.Assignment{{ZoneID: 1001, ZoneName: "alpha", LeaseGen: 2}})
	before, _ := cp.stats()
	eventually(t, "stale replies answered", func() bool { n, _ := cp.stats(); return n >= before+3 })
	if got := agent.Held(); !slices.Equal(got, []orchestrator.HeldRegion{{Region: 1001, LeaseGen: 3}}) || len(lostSoFar()) != 1 {
		t.Fatalf("stale generation: %v %v", got, lostSoFar())
	}

	// lease_lost: every region goes and the agent fences, then registers again.
	cp.setAssignments([]orchestrator.Assignment{{ZoneID: 1001, ZoneName: "alpha", LeaseGen: 3}})
	cp.setMode(cpLeaseLost)
	eventually(t, "lease_lost fences", func() bool { return fenced.Load() >= 1 })
	cp.setMode(cpServe)
	if l := lostSoFar(); len(l) < 2 || l[1] != (lostRegion{orchestrator.HeldRegion{Region: 1001, LeaseGen: 3}, orchestrator.LostLeaseLost}) {
		t.Fatalf("lease_lost: %v", l)
	}
	eventually(t, "re-registered and holding again", func() bool { return len(agent.Held()) == 1 && agent.Current() != nil })

	// Deregistered: a graceful stop hands the regions back.
	n := len(lostSoFar())
	cancel()
	<-done
	stopped = true
	if l := lostSoFar(); len(l) != n+1 || l[n] != (lostRegion{orchestrator.HeldRegion{Region: 1001, LeaseGen: 3}, orchestrator.LostDeregistered}) ||
		len(agent.Held()) != 0 {
		t.Fatalf("deregistered: %v held %v", l, agent.Held())
	}
}

// PR #8 review, blocking 3: a cell with more regions than a held report may carry must not
// become a zombie (lease ended by the leader, still holding everything).
func TestHeldOverCapDoesNotZombie(t *testing.T) {
	bus := testkit.StartNATS(t)
	cfg := platform.Default().Orchestrator
	cfg.LeaseTTL = platform.Duration(600 * time.Millisecond)
	cfg.HeartbeatInterval = platform.Duration(100 * time.Millisecond)
	cfg.Zones = nil
	for i := 0; i < orchestrator.MaxHeldRegions+1; i++ {
		cfg.Zones = append(cfg.Zones, platform.ZoneConfig{ID: int64(1000 + i), Name: fmt.Sprintf("z%d", i)})
	}
	svc, err := orchestrator.New(orchestrator.Deps{Config: cfg, Shard: "t1", ShardIndex: 2, Store: orchestrator.NewMemStore(),
		NATS: bus.Conn, Holder: "a", LeaderTTL: time.Second, RenewInterval: 200 * time.Millisecond, Log: quiet,
		Metrics: prometheus.NewRegistry()})
	if err != nil {
		t.Fatal(err)
	}
	if err := svc.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = svc.Stop(context.Background()) })
	agent := orchestrator.NewAgent(bus.Connect(t, "cell"), "t1", orchestrator.ProcessInfo{Name: "cell-a", Kind: orchestrator.KindCell}, quiet)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { _ = agent.Run(ctx); close(done) }()
	t.Cleanup(func() { cancel(); <-done })
	eventually(t, "registration", func() bool { return agent.Current() != nil })
	first := agent.Current().ProcessID
	time.Sleep(3 * time.Second) // five liveness TTLs
	ps := svc.Registry().Processes()
	if len(ps) != 1 || agent.Current() == nil || agent.Current().ProcessID != first || ps[0].ID != first {
		t.Fatalf("zombie: the leader has %d registrations, the agent %+v", len(ps), agent.Current())
	}
	if n := len(agent.Held()); n != orchestrator.MaxHeldRegions || len(ps[0].Held) != n {
		t.Fatalf("held: agent %d, leader %d (cap %d)", n, len(ps[0].Held), orchestrator.MaxHeldRegions)
	}
}
