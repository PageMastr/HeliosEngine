package orchestrator

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"time"

	"github.com/nats-io/nats.go"

	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// Agent is the process side of the lease protocol: register, heartbeat at the interval the
// orchestrator returned, and on an explicit lease_lost fence and register again as a new epoch.
// The C++ cell and gateway implement the same state machine over nats.c; this Go version serves
// tests, Go tools and as the reference.
//
// Holder rule (05 §1.4.2): a process never fences itself because it cannot reach the
// orchestrator. It keeps its zones and keeps heartbeating; it stops acting as owner only when
// the orchestrator answers lease_lost (or a zone disappears from, or changes generation in, the
// assignments a heartbeat returns). Safety comes from generations checked by every receiver, not
// from holders racing a TTL.
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

	mu      sync.Mutex
	current *RegisterResult
}

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
		req := &HeartbeatRequest{ProcessID: reg.ProcessID, Epoch: reg.Epoch}
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
