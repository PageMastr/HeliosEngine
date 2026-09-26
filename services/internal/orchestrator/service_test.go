package orchestrator_test

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/nats-io/nats.go/jetstream"
	"github.com/prometheus/client_golang/prometheus"

	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
	"github.com/PageMastr/scifi-test/services/pkg/testkit"
)

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

func startService(t *testing.T, bus *testkit.NATS) *orchestrator.Service {
	t.Helper()
	return startReplica(t, bus, orchestrator.NewMemStore(), "replica-a")
}

// startReplica starts an orchestrator replica with fast leadership timing (lease 1 s, renew
// 200 ms) on a shared store, as several backends on one PostgreSQL would run.
func startReplica(t *testing.T, bus *testkit.NATS, store orchestrator.Store, holder string) *orchestrator.Service {
	t.Helper()
	cfg := platform.Default().Orchestrator
	cfg.LeaseTTL = platform.Duration(600 * time.Millisecond)
	cfg.HeartbeatInterval = platform.Duration(100 * time.Millisecond)
	cfg.Zones = []platform.ZoneConfig{{ID: 1001, Name: "alpha"}}
	svc, err := orchestrator.New(orchestrator.Deps{Config: cfg, Shard: "t1", ShardIndex: 2, Store: store, NATS: bus.Conn,
		Holder: holder, LeaderTTL: time.Second, RenewInterval: 200 * time.Millisecond, Log: quiet, Metrics: prometheus.NewRegistry()})
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	if err := svc.Start(ctx); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = svc.Stop(context.Background()) })
	if err := svc.Health(ctx); err != nil {
		t.Fatal(err)
	}
	return svc
}

func eventually(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestAgentLifecycleOverNATS(t *testing.T) {
	bus := testkit.StartNATS(t)
	svc := startService(t, bus)
	cellConn := bus.Connect(t, "cell")

	fd := orchestrator.FailureDomain{AZ: "t1-a", Rack: "r1", Host: "sim-1"}
	agent := orchestrator.NewAgent(cellConn, "t1", orchestrator.ProcessInfo{Name: "cell-a", Kind: orchestrator.KindCell,
		Address: "127.0.0.1:9100", FD: fd, ServerBuild: 42}, quiet)
	var fenced atomic.Int32
	agent.OnFence = func() { fenced.Add(1) }
	var lastAssign atomic.Value
	agent.OnAssignments = func(a []orchestrator.Assignment) { lastAssign.Store(a) }
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { _ = agent.Run(ctx); close(done) }()

	eventually(t, "registration", func() bool { return agent.Current() != nil })
	first := agent.Current()
	if first.Epoch != 1 || len(first.Assignments) != 1 || first.Assignments[0].ZoneName != "alpha" {
		t.Fatalf("registration %+v", first)
	}

	// World directory over NATS.
	client := bus.Connect(t, "client")
	rctx, rcancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer rcancel()
	route, err := rpc.NATSRequest[orchestrator.ResolveZoneRequest, orchestrator.Route](rctx, client,
		orchestrator.Subject("t1", orchestrator.MethodResolveZone), &orchestrator.ResolveZoneRequest{ZoneName: "alpha"})
	if err != nil || route.ProcessID != first.ProcessID || route.Address != "127.0.0.1:9100" || route.LeaseGen != 1 {
		t.Fatalf("resolve: %+v %v", route, err)
	}
	_, err = rpc.NATSRequest[orchestrator.ResolveZoneRequest, orchestrator.Route](rctx, client,
		orchestrator.Subject("t1", orchestrator.MethodResolveZone), &orchestrator.ResolveZoneRequest{ZoneID: 9})
	if rpc.CodeOf(err) != rpc.CodeNotFound {
		t.Fatalf("unknown zone over NATS: %v", err)
	}

	// Heartbeats keep it alive well past the TTL, and report the region held with its generation.
	time.Sleep(900 * time.Millisecond)
	if cur := agent.Current(); cur == nil || cur.ProcessID != first.ProcessID {
		t.Fatal("lease lapsed despite heartbeats")
	}
	ps := svc.Registry().Processes()
	if len(ps) != 1 || ps[0].Info.FD != fd || ps[0].Info.ServerBuild != 42 ||
		len(ps[0].Held) != 1 || ps[0].Held[0] != (orchestrator.HeldRegion{Region: 1001, LeaseGen: 1}) {
		t.Fatalf("registration and held regions as the leader sees them: %+v", ps)
	}

	// KV mirror for nats.c consumers.
	js, _ := jetstream.New(client)
	dir, err := js.KeyValue(rctx, orchestrator.BucketDirectory)
	if err != nil {
		t.Fatal(err)
	}
	eventually(t, "directory mirror", func() bool {
		e, err := dir.Get(rctx, "zone.1001")
		return err == nil && strings.Contains(string(e.Value()), `"process":"cell-a"`)
	})
	if _, err := js.KeyValue(rctx, "LEASES"); err == nil {
		t.Fatal("there is no LEASES bucket: leases and generations live in PostgreSQL (05 §2.3)")
	}

	// A cell mints from its registration blocks and refills through the agent.
	if first.IDShard != 2 || len(first.IDBlocks) != 2 {
		t.Fatalf("id blocks on register: %+v", first)
	}
	more, err := agent.AllocateIdBlocks(rctx, 2)
	if err != nil || len(more) != 2 || more[0] <= first.IDBlocks[1] {
		t.Fatalf("AllocateIdBlocks over NATS: %v %v", more, err)
	}

	// The orchestrator drops the lease behind the agent's back: the agent fences and comes back
	// as a new epoch with a new lease generation.
	if err := svc.Registry().Deregister(context.Background(), first.ProcessID, first.Epoch); err != nil {
		t.Fatal(err)
	}
	eventually(t, "re-registration", func() bool {
		cur := agent.Current()
		return cur != nil && cur.Epoch == 2
	})
	if fenced.Load() != 1 {
		t.Fatalf("fence callbacks: %d", fenced.Load())
	}
	eventually(t, "new generation", func() bool {
		a, _ := lastAssign.Load().([]orchestrator.Assignment)
		return len(a) == 1 && a[0].LeaseGen == 2
	})

	// HTTP view on the ops router.
	r := chi.NewRouter()
	svc.Mount(r)
	w := httptest.NewRecorder()
	r.ServeHTTP(w, httptest.NewRequest(http.MethodPost, orchestrator.ServicePath+orchestrator.MethodListProcesses, strings.NewReader(`{}`)))
	var list orchestrator.ListProcessesResponse
	if err := json.Unmarshal(w.Body.Bytes(), &list); err != nil || len(list.Processes) != 1 || len(list.Zones) != 1 ||
		list.Zones[0].Owner != agent.Current().ProcessID {
		t.Fatalf("list: %s %v", w.Body, err)
	}
	w = httptest.NewRecorder()
	r.ServeHTTP(w, httptest.NewRequest(http.MethodPost, orchestrator.ServicePath+orchestrator.MethodResolveZone,
		strings.NewReader(`{"zoneId":"1001"}`)))
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"process":"cell-a"`) {
		t.Fatalf("http resolve: %d %s", w.Code, w.Body)
	}

	// Graceful stop deregisters immediately (no TTL wait).
	cancel()
	<-done
	eventually(t, "zone released", func() bool {
		_, err := svc.Registry().ResolveZone(1001, "")
		return rpc.CodeOf(err) == rpc.CodeUnavailable
	})
}

func TestSilentProcessExpires(t *testing.T) {
	bus := testkit.StartNATS(t)
	svc := startService(t, bus)
	conn := bus.Connect(t, "zombie")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	reg, err := rpc.NATSRequest[orchestrator.ProcessInfo, orchestrator.RegisterResult](ctx, conn,
		orchestrator.Subject("t1", orchestrator.MethodRegister), &orchestrator.ProcessInfo{Name: "zombie", Kind: orchestrator.KindCell})
	if err != nil || len(reg.Assignments) != 1 {
		t.Fatalf("register: %+v %v", reg, err)
	}
	// No heartbeats: the sweeper expires the lease within TTL + sweep interval.
	eventually(t, "expiry", func() bool { return len(svc.Registry().Processes()) == 0 })
	_, err = rpc.NATSRequest[orchestrator.HeartbeatRequest, orchestrator.HeartbeatResult](ctx, conn,
		orchestrator.Subject("t1", orchestrator.MethodHeartbeat), &orchestrator.HeartbeatRequest{ProcessID: reg.ProcessID, Epoch: reg.Epoch})
	if rpc.CodeOf(err) != rpc.CodeFailedPrecondition || !strings.Contains(err.Error(), "lease_lost") {
		t.Fatalf("zombie heartbeat: %v", err)
	}
	_, err = rpc.NATSRequest[orchestrator.ProcessInfo, orchestrator.RegisterResult](ctx, conn,
		orchestrator.Subject("t1", orchestrator.MethodRegister), &orchestrator.ProcessInfo{Name: "", Kind: orchestrator.KindCell})
	if rpc.CodeOf(err) != rpc.CodeInvalidArgument {
		t.Fatalf("validation over NATS: %v", err)
	}
}

func TestAgentKeepsZonesWhileOrchestratorIsUnreachable(t *testing.T) {
	bus := testkit.StartNATS(t)
	store := orchestrator.NewMemStore()
	a := startReplica(t, bus, store, "replica-a")
	agent := orchestrator.NewAgent(bus.Connect(t, "cell"), "t1", orchestrator.ProcessInfo{Name: "cell-a",
		Kind: orchestrator.KindCell}, quiet)
	var fenced atomic.Int32
	agent.OnFence = func() { fenced.Add(1) }
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { _ = agent.Run(ctx); close(done) }()
	eventually(t, "registration", func() bool { return agent.Current() != nil })
	first := agent.Current()

	// The only orchestrator goes away (no responders). The cell keeps its registration and
	// zones for far longer than the liveness TTL: losing the control plane never self-fences.
	if err := a.Stop(context.Background()); err != nil {
		t.Fatal(err)
	}
	time.Sleep(1500 * time.Millisecond)
	if cur := agent.Current(); cur == nil || cur.ProcessID != first.ProcessID || fenced.Load() != 0 {
		t.Fatalf("agent fenced itself without being told: %+v fenced=%d", cur, fenced.Load())
	}

	// A new leader knows nothing of the old registration: the next heartbeat says lease_lost, the
	// cell fences and comes back as a new epoch.
	b := startReplica(t, bus, store, "replica-b")
	if ok, term := b.Leader(); !ok || term < 2 {
		t.Fatalf("replica-b should lead after a released lease: %v %d", ok, term)
	}
	eventually(t, "re-registration under the new leader", func() bool {
		cur := agent.Current()
		return cur != nil && cur.ProcessID != first.ProcessID
	})
	if fenced.Load() != 1 {
		t.Fatalf("fence callbacks: %d", fenced.Load())
	}
	cancel()
	<-done
}

// partitioned is a Store whose leadership renewals fail while cut is set: the replica using it
// has lost PostgreSQL (the other replica still reaches it).
type partitioned struct {
	orchestrator.Store
	cut atomic.Bool
}

func (p *partitioned) RenewLeadership(ctx context.Context, shard, holder string, term int64, ttl time.Duration) (bool, error) {
	if p.cut.Load() {
		return false, errors.New("connection refused")
	}
	return p.Store.RenewLeadership(ctx, shard, holder, term, ttl)
}

func (p *partitioned) AcquireLeadership(ctx context.Context, shard, holder string, ttl time.Duration) (int64, bool, error) {
	if p.cut.Load() {
		return 0, false, errors.New("connection refused")
	}
	return p.Store.AcquireLeadership(ctx, shard, holder, ttl)
}

func TestLeaderFailoverToStandby(t *testing.T) {
	bus := testkit.StartNATS(t)
	store := orchestrator.NewMemStore()
	aStore := &partitioned{Store: store}
	a := startReplica(t, bus, aStore, "replica-a")
	b := startReplica(t, bus, store, "replica-b")
	if ok, _ := a.Leader(); !ok {
		t.Fatal("first replica must lead")
	}
	if ok, _ := b.Leader(); ok {
		t.Fatal("two leaders")
	}
	conn := bus.Connect(t, "cell")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	reg, err := rpc.NATSRequest[orchestrator.ProcessInfo, orchestrator.RegisterResult](ctx, conn,
		orchestrator.Subject("t1", orchestrator.MethodRegister), &orchestrator.ProcessInfo{Name: "c", Kind: orchestrator.KindCell})
	if err != nil || len(a.Registry().Processes()) != 1 || len(b.Registry().Processes()) != 0 {
		t.Fatalf("only the leader serves: %v", err)
	}

	// The leader loses PostgreSQL: it cannot renew, stops acting once its act window (TTL minus a
	// renew interval) has passed, and the standby takes over when the lease expires.
	aStore.cut.Store(true)
	eventually(t, "old leader steps down", func() bool { ok, _ := a.Leader(); return !ok })
	eventually(t, "standby takes over", func() bool { ok, _ := b.Leader(); return ok })
	if _, term := b.Leader(); term != 2 {
		t.Fatalf("new term %d", term)
	}
	if len(a.Registry().Processes()) != 0 {
		t.Fatal("deposed leader kept its registry")
	}
	_, err = rpc.NATSRequest[orchestrator.HeartbeatRequest, orchestrator.HeartbeatResult](ctx, conn,
		orchestrator.Subject("t1", orchestrator.MethodHeartbeat), &orchestrator.HeartbeatRequest{ProcessID: reg.ProcessID, Epoch: reg.Epoch})
	if rpc.CodeOf(err) != rpc.CodeFailedPrecondition {
		t.Fatalf("heartbeat to the new leader: %v", err)
	}
	again, err := rpc.NATSRequest[orchestrator.ProcessInfo, orchestrator.RegisterResult](ctx, conn,
		orchestrator.Subject("t1", orchestrator.MethodRegister), &orchestrator.ProcessInfo{Name: "c", Kind: orchestrator.KindCell})
	if err != nil || again.Epoch != reg.Epoch+1 || again.Assignments[0].LeaseGen != reg.Assignments[0].LeaseGen+1 ||
		again.IDBlocks[0] <= reg.IDBlocks[1] {
		t.Fatalf("re-registration: %+v %v", again, err)
	}
}

func TestSubjectsFollowTheSpec(t *testing.T) {
	// 05 §2.2's example subject, and §6.5's cell permission rpc.<shard>.orch.>.
	if got := orchestrator.Subject("eu1", orchestrator.MethodHeartbeat); got != "rpc.eu1.orch.Heartbeat" {
		t.Fatalf("subject %q", got)
	}
}
