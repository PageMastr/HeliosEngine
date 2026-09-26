package orchestrator

import (
	"cmp"
	"context"
	"errors"
	"log/slog"
	"slices"
	"sync"
	"time"

	"github.com/nats-io/nats.go"

	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// Agent is the process side of the lease protocol: register with its failure domain and server
// build, heartbeat at the interval the orchestrator returned while reporting the regions it
// holds with their generations, and on an explicit lease_lost fence and register again as a new
// epoch. The C++ cell and gateway implement the same state machine over nats.c; this Go version
// serves tests, Go tools and as the reference.
//
// Holder rule (05 §1.4.2): a process never fences itself because it cannot reach the
// orchestrator, however long that lasts: it has no timer that drops regions. It keeps its regions
// and keeps heartbeating; it drops a region only on an answer from the leader: lease_lost (every
// region), a higher lease_gen for the region in a heartbeat reply, or the region missing from the
// reply's assignments. Safety comes from generations checked by every receiver, not from holders
// racing a TTL. TestConformance/holder_rule (CONF-03) checks this.
type Agent struct {
	nc    *nats.Conn
	shard string
	info  ProcessInfo
	log   *slog.Logger
	// Load, if set, supplies the load reported with each heartbeat.
	Load func() Load
	// OnFence, if set, is called when the orchestrator reports the lease lost: the process must
	// stop acting as the owner of its zones before it registers again.
	OnFence func()
	// OnAssignments, if set, receives the current zone assignments (with lease generations)
	// after registering and after every successful heartbeat.
	OnAssignments func([]Assignment)
	// OnRegionLost, if set, is called for each region the agent stops holding, with the
	// generation it held and why (the Lost* reasons). A region whose generation changed is then
	// held again under the new generation.
	OnRegionLost func(r HeldRegion, reason string)

	mu      sync.Mutex
	current *RegisterResult
	held    map[int64]int64 // region -> lease generation
}

// Reasons passed to Agent.OnRegionLost.
const (
	LostLeaseLost         = "lease_lost"         // the leader no longer knows the registration
	LostGenerationChanged = "generation_changed" // a reply carried a higher lease_gen for the region
	LostUnassigned        = "unassigned"         // a reply no longer assigns the region
	LostDeregistered      = "deregistered"       // the agent stopped and handed its regions back
)

// NewAgent returns an agent for info on shard.
func NewAgent(nc *nats.Conn, shard string, info ProcessInfo, log *slog.Logger) *Agent {
	if log == nil {
		log = slog.Default()
	}
	return &Agent{nc: nc, shard: shard, info: info, log: log}
}

// Current returns the active registration (nil while unregistered).
func (a *Agent) Current() *RegisterResult {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.current
}

func (a *Agent) set(r *RegisterResult) {
	a.mu.Lock()
	a.current = r
	a.mu.Unlock()
}

// Held returns the regions the agent currently holds, by region, as its next heartbeat reports
// them.
func (a *Agent) Held() []HeldRegion {
	a.mu.Lock()
	defer a.mu.Unlock()
	out := make([]HeldRegion, 0, len(a.held))
	for r, g := range a.held {
		out = append(out, HeldRegion{Region: r, LeaseGen: g})
	}
	slices.SortFunc(out, func(x, y HeldRegion) int { return cmp.Compare(x.Region, y.Region) })
	return out
}

// hold replaces the held set with the leader's assignments and reports what was lost. Only an
// answer from the leader reaches here, never a failure to get one.
func (a *Agent) hold(assignments []Assignment) {
	next := make(map[int64]int64, len(assignments))
	for _, as := range assignments {
		next[WholeRegion(as.ZoneID)] = as.LeaseGen
	}
	a.mu.Lock()
	var lost []HeldRegion
	var reasons []string
	for r, g := range a.held {
		switch ng, ok := next[r]; {
		case !ok:
			lost, reasons = append(lost, HeldRegion{Region: r, LeaseGen: g}), append(reasons, LostUnassigned)
		case ng > g:
			lost, reasons = append(lost, HeldRegion{Region: r, LeaseGen: g}), append(reasons, LostGenerationChanged)
		case ng < g:
			next[r] = g // a stale reply never lowers a generation already seen
		}
	}
	a.held = next
	a.mu.Unlock()
	a.reportLost(lost, reasons)
}

// dropAll forgets every region.
func (a *Agent) dropAll(reason string) {
	a.mu.Lock()
	var lost []HeldRegion
	for r, g := range a.held {
		lost = append(lost, HeldRegion{Region: r, LeaseGen: g})
	}
	a.held = nil
	a.mu.Unlock()
	reasons := make([]string, len(lost))
	for i := range reasons {
		reasons[i] = reason
	}
	a.reportLost(lost, reasons)
}

func (a *Agent) reportLost(lost []HeldRegion, reasons []string) {
	for i, r := range lost {
		a.log.Info("region lost", "name", a.info.Name, "region", r.Region, "lease_gen", r.LeaseGen, "reason", reasons[i])
		if a.OnRegionLost != nil {
			a.OnRegionLost(r, reasons[i])
		}
	}
}

// ErrNotRegistered is returned by AllocateIdBlocks while the agent holds no registration.
var ErrNotRegistered = errors.New("orchestrator: agent is not registered")

// AllocateIdBlocks asks the orchestrator for n more ID-block prefixes, making the agent an
// idgen.Source for a cell's Minter (seed the minter with Current().IDBlocks first).
func (a *Agent) AllocateIdBlocks(ctx context.Context, n int) ([]int64, error) {
	reg := a.Current()
	if reg == nil {
		return nil, ErrNotRegistered
	}
	res, err := rpc.NATSRequest[AllocateIdBlocksRequest, AllocateIdBlocksResponse](ctx, a.nc,
		Subject(a.shard, MethodAllocateIdBlocks), &AllocateIdBlocksRequest{ProcessID: reg.ProcessID, Epoch: reg.Epoch, N: n})
	if err != nil {
		return nil, err
	}
	return res.Prefixes, nil
}

// Run keeps the registration until ctx ends, then deregisters.
func (a *Agent) Run(ctx context.Context) error {
	backoff := 100 * time.Millisecond
	for ctx.Err() == nil {
		reg, err := rpc.NATSRequest[ProcessInfo, RegisterResult](ctx, a.nc, Subject(a.shard, MethodRegister), &a.info)
		if err != nil {
			a.log.Warn("register failed; retrying", "name", a.info.Name, "err", err, "in", backoff)
			if !sleep(ctx, backoff) {
				break
			}
			backoff = min(backoff*2, 5*time.Second)
			continue
		}
		backoff = 100 * time.Millisecond
		a.set(reg)
		a.hold(reg.Assignments)
		if a.OnAssignments != nil {
			a.OnAssignments(reg.Assignments)
		}
		a.heartbeat(ctx, reg)
		if ctx.Err() != nil {
			// Graceful shutdown: hand the zones back now instead of waiting for the TTL.
			dctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			_, _ = rpc.NATSRequest[DeregisterRequest, Empty](dctx, a.nc, Subject(a.shard, MethodDeregister),
				&DeregisterRequest{ProcessID: reg.ProcessID, Epoch: reg.Epoch})
			cancel()
			a.dropAll(LostDeregistered)
		}
		a.set(nil)
	}
	return nil
}

// heartbeat renews until ctx ends (returns with the registration still current) or the
// orchestrator reports the lease lost (fences and returns so Run registers again). Failures to
// reach the orchestrator are logged and retried at the heartbeat interval; they never fence.
func (a *Agent) heartbeat(ctx context.Context, reg *RegisterResult) {
	interval := time.Duration(reg.HeartbeatIntervalMs) * time.Millisecond
	if interval <= 0 {
		interval = time.Second
	}
	failing := false
	for {
		if !sleep(ctx, interval) {
			return
		}
		req := &HeartbeatRequest{ProcessID: reg.ProcessID, Epoch: reg.Epoch, Held: a.Held()}
		if a.Load != nil {
			req.Load = a.Load()
		}
		hctx, cancel := context.WithTimeout(ctx, interval)
		res, err := rpc.NATSRequest[HeartbeatRequest, HeartbeatResult](hctx, a.nc, Subject(a.shard, MethodHeartbeat), req)
		cancel()
		if err == nil {
			if failing {
				a.log.Info("orchestrator reachable again", "name", a.info.Name, "process", reg.ProcessID)
				failing = false
			}
			a.hold(res.Assignments)
			if a.OnAssignments != nil {
				a.OnAssignments(res.Assignments)
			}
			continue
		}
		if ctx.Err() != nil {
			return
		}
		if rpc.CodeOf(err) == rpc.CodeFailedPrecondition {
			a.log.Warn("lease lost; fencing", "name", a.info.Name, "process", reg.ProcessID, "epoch", reg.Epoch)
			a.dropAll(LostLeaseLost)
			if a.OnFence != nil {
				a.OnFence()
			}
			a.set(nil)
			return
		}
		if !failing {
			a.log.Warn("heartbeat failed; keeping zones and retrying", "name", a.info.Name, "err", err)
			failing = true
		}
	}
}

func sleep(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
}
