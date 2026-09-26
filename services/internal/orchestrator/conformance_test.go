package orchestrator_test

import (
	"context"
	"slices"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/nats-io/nats.go"

	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
	"github.com/PageMastr/scifi-test/services/pkg/testkit"
)

// TestConformance holds the behavioural tests that 09 §5.10's CONF rules require of the Go
// backend; subtest names are the test IDs of tools/conformance/map.jsonc ("conformance/<name>").
func TestConformance(t *testing.T) {
	t.Run("holder_rule", testHolderRule)
}

type cpMode int

const (
	cpServe       cpMode = iota
	cpGone               // no subscriber: requests fail with no responders
	cpSilent             // requests arrive but are never answered: they time out
	cpUnavailable        // answered with "unavailable", as while leadership changes or PG is down
	cpLeaseLost          // answered with lease_lost: the leader no longer knows the registration
)

// scriptedOrchestrator answers RegisterProcess and Heartbeat the way the leader would, except
// that the test chooses the answer.
type scriptedOrchestrator struct {
	t     *testing.T
	nc    *nats.Conn
	shard string

	mu          sync.Mutex
	mode        cpMode
	assignments []orchestrator.Assignment
	subs        []*nats.Subscription
	beats       int                       // heartbeat requests received (answered or not)
	lastHeld    []orchestrator.HeldRegion // held list of the last heartbeat received
}

func startScripted(t *testing.T, bus *testkit.NATS, shard string, assignments []orchestrator.Assignment) *scriptedOrchestrator {
	s := &scriptedOrchestrator{t: t, nc: bus.Connect(t, "scripted-orchestrator"), shard: shard, assignments: assignments}
	s.subscribe()
	return s
}

func (s *scriptedOrchestrator) subscribe() {
	unavailable := rpc.Errorf(rpc.CodeUnavailable, "orchestrator leadership changed; retry")
	reg, err := rpc.NATSHandle(s.nc, orchestrator.Subject(s.shard, orchestrator.MethodRegister), "orchestrator", quiet,
		func(context.Context, *orchestrator.ProcessInfo) (*orchestrator.RegisterResult, error) {
			s.mu.Lock()
			defer s.mu.Unlock()
			return &orchestrator.RegisterResult{ProcessID: 77, Epoch: 1, LeaseTTLMs: 600, HeartbeatIntervalMs: 100,
				Assignments: slices.Clone(s.assignments)}, nil
		})
	if err != nil {
		s.t.Fatal(err)
	}
	hb, err := rpc.NATSHandle(s.nc, orchestrator.Subject(s.shard, orchestrator.MethodHeartbeat), "orchestrator", quiet,
		func(ctx context.Context, req *orchestrator.HeartbeatRequest) (*orchestrator.HeartbeatResult, error) {
			s.mu.Lock()
			s.beats++
			s.lastHeld = slices.Clone(req.Held)
			mode, as := s.mode, slices.Clone(s.assignments)
			s.mu.Unlock()
			switch mode {
			case cpSilent:
				<-ctx.Done() // the caller's deadline passes; the late reply is dropped
				return nil, ctx.Err()
			case cpUnavailable:
				return nil, unavailable
			case cpLeaseLost:
				return nil, rpc.Errorf(rpc.CodeFailedPrecondition, "lease_lost")
			}
			return &orchestrator.HeartbeatResult{LeaseExpires: time.Now().Add(600 * time.Millisecond), Assignments: as,
				Mode: orchestrator.ModeNormal}, nil
		})
	if err != nil {
		s.t.Fatal(err)
	}
	if err := s.nc.Flush(); err != nil {
		s.t.Fatal(err)
	}
	s.mu.Lock()
	s.subs = []*nats.Subscription{reg, hb}
	s.mu.Unlock()
}

func (s *scriptedOrchestrator) setMode(m cpMode) {
	s.mu.Lock()
	prev, subs := s.mode, s.subs
	s.mode = m
	if m == cpGone {
		s.subs = nil
	}
	s.mu.Unlock()
	switch {
	case m == cpGone:
		for _, sub := range subs {
			_ = sub.Unsubscribe()
		}
		_ = s.nc.Flush()
	case prev == cpGone:
		s.subscribe()
	}
}

func (s *scriptedOrchestrator) setAssignments(as []orchestrator.Assignment) {
	s.mu.Lock()
	s.assignments = as
	s.mu.Unlock()
}

func (s *scriptedOrchestrator) stats() (beats int, held []orchestrator.HeldRegion) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.beats, slices.Clone(s.lastHeld)
}

type lostRegion struct {
	region orchestrator.HeldRegion
	reason string
}

// testHolderRule is CONF-03's required test (09 §5.10.3; 05 §1.4.2): with the control plane
// unreachable for 60 s the Go Agent keeps its regions, and it drops one only after seeing a higher
// lease_gen for it. The outage covers every way the control plane goes missing: no responders,
// requests that time out, and "unavailable" answers. A shorter outage would let a holder that
// self-fences after, say, 30 s pass, so -short skips the test rather than shortening it; CI's Go
// jobs run without -short.
func testHolderRule(t *testing.T) {
	if testing.Short() {
		t.Skip("conformance/holder_rule needs the 60 s outage CONF-03 specifies; run without -short")
	}
	t.Parallel()
	const outage = 60 * time.Second
	bus := testkit.StartNATS(t)
	cp := startScripted(t, bus, "t1", []orchestrator.Assignment{
		{ZoneID: 1001, ZoneName: "alpha", LeaseGen: 3}, {ZoneID: 1002, ZoneName: "beta", LeaseGen: 5}})
	agent := orchestrator.NewAgent(bus.Connect(t, "cell"), "t1", orchestrator.ProcessInfo{Name: "cell-a", Kind: orchestrator.KindCell}, quiet)
	var fenced atomic.Int32
	agent.OnFence = func() { fenced.Add(1) }
	var mu sync.Mutex
	var lost []lostRegion
	agent.OnRegionLost = func(r orchestrator.HeldRegion, reason string) {
		mu.Lock()
		lost = append(lost, lostRegion{r, reason})
		mu.Unlock()
	}
	lostSoFar := func() []lostRegion {
		mu.Lock()
		defer mu.Unlock()
		return slices.Clone(lost)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { _ = agent.Run(ctx); close(done) }()
	t.Cleanup(func() { cancel(); <-done })

	held := []orchestrator.HeldRegion{{Region: 1001, LeaseGen: 3}, {Region: 1002, LeaseGen: 5}}
	eventually(t, "registration and heartbeats reporting both regions", func() bool {
		n, last := cp.stats()
		return n >= 2 && slices.Equal(last, held)
	})
	first := agent.Current()

	check := func(stage string) {
		t.Helper()
		if cur := agent.Current(); cur == nil || cur.ProcessID != first.ProcessID {
			t.Fatalf("%s: the agent gave up its registration: %+v", stage, cur)
		}
		if got := agent.Held(); !slices.Equal(got, held) || fenced.Load() != 0 || len(lostSoFar()) != 0 {
			t.Fatalf("%s: regions dropped without an answer from the leader: held %v, fenced %d, lost %v",
				stage, got, fenced.Load(), lostSoFar())
		}
	}
	names := map[cpMode]string{cpGone: "no responders", cpSilent: "timeouts", cpUnavailable: "unavailable"}
	start := time.Now()
	for _, mode := range []cpMode{cpGone, cpSilent, cpUnavailable} {
		before, _ := cp.stats()
		cp.setMode(mode)
		for end := time.Now().Add(outage / 3); time.Now().Before(end); time.Sleep(50 * time.Millisecond) {
			check(names[mode])
		}
		if after, _ := cp.stats(); mode != cpGone && after-before < 3 {
			t.Fatalf("%s: the agent stopped heartbeating (%d attempts)", names[mode], after-before)
		}
	}
	check("end of outage")
	if d := time.Since(start); d < outage {
		t.Fatalf("outage lasted %s, want %s", d, outage)
	}

	// The control plane answers again. Region 1001 was reassigned meanwhile, so it now carries a
	// higher generation: exactly that lease is dropped, and the region is held under the new one.
	cp.setAssignments([]orchestrator.Assignment{{ZoneID: 1001, ZoneName: "alpha", LeaseGen: 4}, {ZoneID: 1002, ZoneName: "beta", LeaseGen: 5}})
	cp.setMode(cpServe)
	eventually(t, "the higher generation seen", func() bool { return len(lostSoFar()) > 0 })
	if l := lostSoFar(); len(l) != 1 || l[0] != (lostRegion{orchestrator.HeldRegion{Region: 1001, LeaseGen: 3}, orchestrator.LostGenerationChanged}) {
		t.Fatalf("lost %v, want only region 1001 at generation 3", l)
	}
	held = []orchestrator.HeldRegion{{Region: 1001, LeaseGen: 4}, {Region: 1002, LeaseGen: 5}}
	eventually(t, "heartbeats reporting the new generation", func() bool {
		_, last := cp.stats()
		return slices.Equal(last, held)
	})
	if cur := agent.Current(); cur == nil || cur.ProcessID != first.ProcessID || fenced.Load() != 0 {
		t.Fatalf("a higher generation for one region must not fence the process: %+v fenced=%d", cur, fenced.Load())
	}
}
