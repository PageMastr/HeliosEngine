package orchestrator

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/testutil"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

type recorder struct {
	mu    sync.Mutex
	up    []int64
	down  map[int64]string
	zones []Zone
}

func (r *recorder) ProcessUp(p Process) { r.mu.Lock(); r.up = append(r.up, p.ID); r.mu.Unlock() }
func (r *recorder) ProcessDown(p Process, reason string) {
	r.mu.Lock()
	r.down[p.ID] = reason
	r.mu.Unlock()
}
func (r *recorder) ZoneChanged(z Zone, _ *Process) {
	r.mu.Lock()
	r.zones = append(r.zones, z)
	r.mu.Unlock()
}

type regFixture struct {
	reg   *Registry
	store *MemStore
	clk   *clock.Fake
	ev    *recorder
	prom  *prometheus.Registry
}

// lead takes the shard's leadership for holder in store and returns the fence.
func lead(t *testing.T, store Store, holder string) Fence {
	t.Helper()
	term, ok, err := store.AcquireLeadership(context.Background(), "t1", holder, 10*time.Second)
	if err != nil || !ok {
		t.Fatalf("acquire leadership: %v %v", ok, err)
	}
	return Fence{Shard: "t1", Term: term}
}

func newReg(t *testing.T, zones ...Zone) *regFixture {
	t.Helper()
	if len(zones) == 0 {
		zones = []Zone{{ID: 1001, Name: "alpha"}}
	}
	f := &regFixture{clk: clock.NewFake(time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC)),
		ev: &recorder{down: map[int64]string{}}, prom: prometheus.NewRegistry()}
	f.store = NewMemStoreWithClock(f.clk)
	f.reg = NewRegistry(Config{LeaseTTL: 3 * time.Second, HeartbeatInterval: time.Second, Zones: zones, IDShard: 5},
		f.store, f.clk, quiet, f.ev, f.prom)
	if err := f.reg.Load(context.Background(), lead(t, f.store, "replica-a")); err != nil {
		t.Fatal(err)
	}
	return f
}

func cell(name string, zones ...string) ProcessInfo {
	return ProcessInfo{Name: name, Kind: KindCell, Address: "127.0.0.1:9000", Zones: zones}
}

func gateway(name, addr string, key uint32) ProcessInfo {
	return ProcessInfo{Name: name, Kind: KindGateway, Address: addr, KeyID: key, Capacity: 256}
}

func TestLeaseLifecycle(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	a, err := f.reg.Register(ctx, cell("cell-a"))
	if err != nil {
		t.Fatal(err)
	}
	if a.Epoch != 1 || a.LeaseTTLMs != 3000 || a.HeartbeatIntervalMs != 1000 ||
		len(a.Assignments) != 1 || a.Assignments[0].ZoneID != 1001 || a.Assignments[0].LeaseGen != 1 {
		t.Fatalf("register: %+v", a)
	}
	route, err := f.reg.ResolveZone(1001, "")
	if err != nil || route.ProcessID != a.ProcessID || route.Epoch != 1 || route.LeaseGen != 1 {
		t.Fatalf("resolve: %+v %v", route, err)
	}
	if byName, _ := f.reg.ResolveZone(0, "alpha"); byName == nil || byName.ProcessID != a.ProcessID {
		t.Fatal("resolve by name")
	}

	// Heartbeats keep the lease alive past the TTL.
	for i := 0; i < 5; i++ {
		f.clk.Advance(time.Second)
		if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{Players: i}); err != nil {
			t.Fatalf("heartbeat %d: %v", i, err)
		}
		if n := f.reg.Sweep(ctx); n != 0 {
			t.Fatal("live lease expired")
		}
	}
	// A wrong epoch is refused.
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch+1, Load{}); rpc.CodeOf(err) != rpc.CodeFailedPrecondition {
		t.Fatalf("wrong epoch: %v", err)
	}

	// Silence for longer than the TTL: the sweep expires it and the zone becomes unowned.
	f.clk.Advance(3100 * time.Millisecond)
	if n := f.reg.Sweep(ctx); n != 1 {
		t.Fatalf("expired %d", n)
	}
	if f.store.EndReason(a.ProcessID) != "lease_expired" || f.ev.down[a.ProcessID] != "lease_expired" {
		t.Fatal("end reason not recorded")
	}
	if _, err := f.reg.ResolveZone(1001, ""); rpc.CodeOf(err) != rpc.CodeUnavailable {
		t.Fatalf("zone should be unowned: %v", err)
	}
	// The zombie's late heartbeat tells it to fence.
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{}); err != ErrLeaseLost {
		t.Fatalf("late heartbeat: %v", err)
	}

	// A replacement incarnation gets a higher epoch and a new lease generation.
	b, _ := f.reg.Register(ctx, cell("cell-a"))
	if b.Epoch != 2 || b.Assignments[0].LeaseGen != 2 {
		t.Fatalf("replacement: %+v", b)
	}
	if got := testutil.ToFloat64(f.reg.m.zonesAssigned); got != 1 {
		t.Fatalf("zones gauge %v", got)
	}
	if got := testutil.ToFloat64(f.reg.m.ended.WithLabelValues("lease_expired")); got != 1 {
		t.Fatalf("ended counter %v", got)
	}
}

func TestHeartbeatAfterLapseBeforeSweep(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	a, _ := f.reg.Register(ctx, cell("cell-a"))
	f.clk.Advance(4 * time.Second) // lapsed, sweep has not run yet
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{}); err != ErrLeaseLost {
		t.Fatalf("lapsed heartbeat must fence: %v", err)
	}
	if len(f.reg.Processes()) != 0 {
		t.Fatal("lapsed registration kept")
	}
}

func TestSupersedeMovesZonesUnderNewGeneration(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	a1, _ := f.reg.Register(ctx, cell("cell-a"))
	a2, _ := f.reg.Register(ctx, cell("cell-a")) // crash + fast restart before the TTL
	if a2.Epoch != 2 || a2.Assignments[0].LeaseGen != 2 {
		t.Fatalf("supersede: %+v", a2)
	}
	if f.store.EndReason(a1.ProcessID) != "superseded" {
		t.Fatal("old incarnation not ended")
	}
	if _, err := f.reg.Heartbeat(ctx, a1.ProcessID, a1.Epoch, Load{}); err != ErrLeaseLost {
		t.Fatal("superseded incarnation still renews")
	}
}

func TestPlacementPolicy(t *testing.T) {
	f := newReg(t, Zone{ID: 1, Name: "alpha"}, Zone{ID: 2, Name: "beta"}, Zone{ID: 3, Name: "gamma"})
	ctx := context.Background()
	anyCell, _ := f.reg.Register(ctx, cell("any"))
	if len(anyCell.Assignments) != 3 {
		t.Fatalf("a lone any-zone cell takes every zone: %+v", anyCell.Assignments)
	}
	// A cell declaring only beta does not steal an owned zone...
	beta, _ := f.reg.Register(ctx, cell("beta-cell", "beta"))
	if len(beta.Assignments) != 0 {
		t.Fatal("owned zones are not stolen")
	}
	// ...but gets it when the owner goes away, ahead of other any-zone cells.
	other, _ := f.reg.Register(ctx, cell("other"))
	if err := f.reg.Deregister(ctx, anyCell.ProcessID, anyCell.Epoch); err != nil {
		t.Fatal(err)
	}
	if r, _ := f.reg.ResolveZone(2, ""); r == nil || r.ProcessID != beta.ProcessID {
		t.Fatalf("declared cell must win beta: %+v", r)
	}
	for _, z := range []int64{1, 3} {
		if r, _ := f.reg.ResolveZone(z, ""); r == nil || r.ProcessID != other.ProcessID {
			t.Fatalf("zone %d should move to the remaining any-zone cell", z)
		}
	}
	// A cell declaring zones never receives undeclared ones.
	h, _ := f.reg.Heartbeat(ctx, beta.ProcessID, beta.Epoch, Load{})
	if len(h.Assignments) != 1 || h.Assignments[0].ZoneName != "beta" {
		t.Fatalf("beta assignments %+v", h.Assignments)
	}
	if _, err := f.reg.ResolveZone(99, ""); rpc.CodeOf(err) != rpc.CodeNotFound {
		t.Fatalf("unknown zone: %v", err)
	}
	if err := f.reg.Deregister(ctx, 12345, 1); err != ErrLeaseLost {
		t.Fatal("deregister unknown")
	}
}

func TestLeastLoadedCellWins(t *testing.T) {
	f := newReg(t, Zone{ID: 1, Name: "a"}, Zone{ID: 2, Name: "b"})
	ctx := context.Background()
	c1, _ := f.reg.Register(ctx, cell("c1"))
	c2, _ := f.reg.Register(ctx, cell("c2"))
	_ = f.reg.Deregister(ctx, c1.ProcessID, c1.Epoch) // both zones move to c2
	c3, _ := f.reg.Register(ctx, cell("c3"))
	if len(c3.Assignments) != 0 {
		t.Fatal("no free zones for c3 yet")
	}
	h, _ := f.reg.Heartbeat(ctx, c2.ProcessID, c2.Epoch, Load{})
	if len(h.Assignments) != 2 {
		t.Fatalf("c2 should own both: %+v", h.Assignments)
	}
}

func TestGatewaysAndValidation(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	for _, bad := range []ProcessInfo{
		{Name: "", Kind: KindCell},
		{Name: "x", Kind: "router"},
		{Name: "gw", Kind: KindGateway, Address: "gateway.local:7777", KeyID: 1},
		{Name: "gw", Kind: KindGateway, Address: "127.0.0.1:7777"},
	} {
		if _, err := f.reg.Register(ctx, bad); rpc.CodeOf(err) != rpc.CodeInvalidArgument {
			t.Errorf("%+v accepted: %v", bad, err)
		}
	}
	g1, _ := f.reg.Register(ctx, gateway("gw-1", "127.0.0.1:7777", 1))
	g2, _ := f.reg.Register(ctx, gateway("gw-2", "[::1]:7778", 2))
	if _, err := f.reg.Heartbeat(ctx, g2.ProcessID, g2.Epoch, Load{FreeSlots: 10}); err != nil {
		t.Fatal(err)
	}
	gws := f.reg.Gateways()
	if len(gws) != 2 || gws[0].ProcessID != g1.ProcessID || gws[0].FreeSlots != 256 || gws[1].FreeSlots != 10 ||
		gws[1].KeyID != 2 || gws[1].Address.String() != "[::1]:7778" {
		t.Fatalf("gateways: %+v", gws)
	}
	if len(g1.Assignments) != 0 {
		t.Fatal("gateways never own zones")
	}
	f.clk.Advance(4 * time.Second)
	if len(f.reg.Gateways()) != 0 {
		t.Fatal("lapsed gateways must not receive tokens even before the sweep")
	}
}

func TestCellsReceiveIDBlocks(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	c1, err := f.reg.Register(ctx, cell("c1"))
	if err != nil {
		t.Fatal(err)
	}
	c2, _ := f.reg.Register(ctx, cell("c2"))
	if c1.IDShard != 5 || len(c1.IDBlocks) != 2 || len(c2.IDBlocks) != 2 ||
		c1.IDBlocks[0] >= c1.IDBlocks[1] || c1.IDBlocks[1] >= c2.IDBlocks[0] {
		t.Fatalf("blocks must be distinct and increasing: %v %v", c1.IDBlocks, c2.IDBlocks)
	}
	gw, _ := f.reg.Register(ctx, gateway("gw", "127.0.0.1:7777", 1))
	if len(gw.IDBlocks) != 0 {
		t.Fatal("gateways never mint (05 §1.4.5)")
	}
	more, err := f.reg.AllocateIdBlocks(ctx, c1.ProcessID, c1.Epoch, 3)
	if err != nil || len(more) != 3 || more[0] <= c2.IDBlocks[1] {
		t.Fatalf("refill: %v %v", more, err)
	}
	if _, err := f.reg.AllocateIdBlocks(ctx, gw.ProcessID, gw.Epoch, 1); rpc.CodeOf(err) != rpc.CodePermissionDenied {
		t.Fatalf("gateway refill: %v", err)
	}
	if _, err := f.reg.AllocateIdBlocks(ctx, c1.ProcessID, c1.Epoch+1, 1); err != ErrLeaseLost {
		t.Fatalf("stale epoch refill: %v", err)
	}
	for _, n := range []int{0, 17} {
		if _, err := f.reg.AllocateIdBlocks(ctx, c1.ProcessID, c1.Epoch, n); rpc.CodeOf(err) != rpc.CodeInvalidArgument {
			t.Fatalf("n=%d: %v", n, err)
		}
	}
	// JSON carries the prefixes as strings (proto3 int64 mapping).
	b, _ := json.Marshal(c1)
	var back RegisterResult
	if err := json.Unmarshal(b, &back); err != nil || !strings.Contains(string(b), `"idBlocks":["`) ||
		len(back.IDBlocks) != 2 || back.IDBlocks[1] != c1.IDBlocks[1] {
		t.Fatalf("json: %s %v", b, err)
	}
}

func TestStaleGatewaysGetNoTokensBeforeTheirLeaseExpires(t *testing.T) {
	f := newReg(t)
	f.reg.cfg.LeaseTTL = 12 * time.Second // the single-signal liveness TTL
	ctx := context.Background()
	g, _ := f.reg.Register(ctx, gateway("gw-1", "127.0.0.1:7777", 1))
	f.clk.Advance(2 * time.Second)
	if len(f.reg.Gateways()) != 1 {
		t.Fatal("fresh gateway missing")
	}
	f.clk.Advance(1500 * time.Millisecond) // 3.5 s without a heartbeat (freshness 3 x 1 s)
	if len(f.reg.Gateways()) != 0 || len(f.reg.Processes()) != 1 {
		t.Fatal("a silent gateway must drop out of token lists while its lease still holds")
	}
	if _, err := f.reg.Heartbeat(ctx, g.ProcessID, g.Epoch, Load{FreeSlots: 3}); err != nil {
		t.Fatal(err)
	}
	if gws := f.reg.Gateways(); len(gws) != 1 || gws[0].FreeSlots != 3 {
		t.Fatalf("gateway back after heartbeat: %+v", gws)
	}
}

func TestLoadResetsOwnersFromPreviousRun(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	a, _ := f.reg.Register(ctx, cell("cell-a"))
	var deposed atomic.Int64
	f.reg.OnNotLeader = func(term int64) { deposed.Store(term) }
	// A new orchestrator instance over the same store takes over once the old lease expires.
	if _, ok, _ := f.store.AcquireLeadership(ctx, "t1", "replica-b", 10*time.Second); ok {
		t.Fatal("a live leader lease must not be taken over")
	}
	f.clk.Advance(11 * time.Second)
	reg2 := NewRegistry(Config{LeaseTTL: 3 * time.Second, HeartbeatInterval: time.Second, Zones: []Zone{{ID: 1001, Name: "alpha"}}},
		f.store, f.clk, quiet, nil, nil)
	f2 := lead(t, f.store, "replica-b")
	if f2.Term != 2 {
		t.Fatalf("new term %d", f2.Term)
	}
	if err := reg2.Load(ctx, f2); err != nil {
		t.Fatal(err)
	}
	// The deposed leader's writes are fenced by term: it cannot register or place anything.
	if _, err := f.reg.Register(ctx, cell("zombie-leader-cell")); rpc.CodeOf(err) != rpc.CodeUnavailable {
		t.Fatalf("deposed leader registered a process: %v", err)
	}
	eventually(t, func() bool { return deposed.Load() == 1 }) // the term the rejected write carried
	if zs, _ := f.store.ListZones(ctx); zs[0].Owner != 0 || zs[0].LeaseGen != 1 {
		t.Fatalf("deposed leader changed zone state: %+v", zs[0])
	}
	if _, err := reg2.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{}); err != ErrLeaseLost {
		t.Fatal("old leases must not survive an orchestrator restart")
	}
	b, _ := reg2.Register(ctx, cell("cell-a"))
	if b.Epoch != 2 || b.Assignments[0].LeaseGen != 2 {
		t.Fatalf("re-registration after restart: %+v", b)
	}
}

func eventually(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatal("condition not reached")
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestResetForgetsEverything(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	a, _ := f.reg.Register(ctx, cell("cell-a"))
	f.reg.Reset()
	if len(f.reg.Processes()) != 0 || len(f.reg.Zones()) != 0 {
		t.Fatal("state kept after stepping down")
	}
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{}); err != ErrLeaseLost {
		t.Fatalf("heartbeat after reset: %v", err)
	}
	if _, err := f.reg.Register(ctx, cell("cell-b")); rpc.CodeOf(err) != rpc.CodeUnavailable {
		t.Fatalf("register on a standby registry: %v", err)
	}
}

func TestRegistrationRecordsFailureDomainAndServerBuild(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	info := cell("cell-a")
	info.FD = FailureDomain{AZ: "eu1-a", Rack: "r12", Host: "sim-07"}
	info.ServerBuild = 4711
	a, err := f.reg.Register(ctx, info)
	if err != nil {
		t.Fatal(err)
	}
	legacy := cell("cell-b")
	legacy.Host = "old-style-host" // processes that predate fd name only their host
	b, _ := f.reg.Register(ctx, legacy)
	ps := f.reg.Processes()
	if len(ps) != 2 || ps[0].ID != a.ProcessID || ps[0].Info.FD != info.FD || ps[0].Info.ServerBuild != 4711 ||
		ps[1].ID != b.ProcessID || ps[1].Info.FD != (FailureDomain{Host: "old-style-host"}) {
		t.Fatalf("registrations: %+v", ps)
	}
	// Both travel in the directory listing and in process events.
	raw, _ := json.Marshal(ps[0])
	if !strings.Contains(string(raw), `"fd":{"az":"eu1-a","rack":"r12","host":"sim-07"}`) || !strings.Contains(string(raw), `"serverBuild":"4711"`) {
		t.Fatalf("json: %s", raw)
	}
	long := strings.Repeat("x", MaxFDLabel+1)
	for _, bad := range []func(*ProcessInfo){
		func(p *ProcessInfo) { p.FD.AZ = long },
		func(p *ProcessInfo) { p.FD.Rack = long },
		func(p *ProcessInfo) { p.FD.Host = strings.Repeat("h", MaxFDHost+1) },
		func(p *ProcessInfo) { p.ServerBuild = -1 },
		func(p *ProcessInfo) { p.Version = strings.Repeat("v", 256) },
		func(p *ProcessInfo) { p.Name = "cell\x00c" },
		func(p *ProcessInfo) { p.Host, p.FD.Host = "h\x00", "h" }, // an explicit fd.host does not hide it
		func(p *ProcessInfo) { p.FD.Host = "h\x00" },
		func(p *ProcessInfo) { p.Address = "127.0.0.1\x00:1" },
		func(p *ProcessInfo) { p.FD.Rack = "r\x001" },
		func(p *ProcessInfo) { p.Version = "1.0\x00" },
		func(p *ProcessInfo) { p.Zones = []string{strings.Repeat("z", 65)} },
		func(p *ProcessInfo) { p.Zones = []string{""} },
		func(p *ProcessInfo) { p.Zones = []string{"a\x00b"} },
	} {
		p := cell("cell-c")
		bad(&p)
		if _, err := f.reg.Register(ctx, p); rpc.CodeOf(err) != rpc.CodeInvalidArgument {
			t.Errorf("%+v accepted: %v", p, err)
		}
	}
}

func TestHeartbeatRecordsHeldRegions(t *testing.T) {
	f := newReg(t, Zone{ID: 1, Name: "a"}, Zone{ID: 2, Name: "b"})
	ctx := context.Background()
	a, _ := f.reg.Register(ctx, cell("cell-a"))
	if p := f.reg.Processes()[0]; p.Held == nil || len(p.Held) != 0 {
		t.Fatalf("nothing reported yet: %+v", p.Held)
	}
	held := []HeldRegion{{Region: 2, LeaseGen: 1}, {Region: 1, LeaseGen: 1}}
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{}, held...); err != nil {
		t.Fatal(err)
	}
	if got := f.reg.Processes()[0].Held; len(got) != 2 || got[0] != held[1] || got[1] != held[0] {
		t.Fatalf("held regions, by region: %+v", got)
	}
	// Each heartbeat replaces the report.
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{}, HeldRegion{Region: 1, LeaseGen: 1}); err != nil {
		t.Fatal(err)
	}
	if got := f.reg.Processes()[0].Held; len(got) != 1 || got[0].Region != 1 {
		t.Fatalf("second report: %+v", got)
	}
	// A bad report never blocks the renewal (PR #8 review, blocking 3): invalid, duplicate and
	// over-cap entries are dropped and counted, the rest is recorded, and the lease renews.
	big := []HeldRegion{{Region: 0, LeaseGen: 1}, {Region: 5, LeaseGen: -1}, {Region: 1, LeaseGen: 1}, {Region: 1, LeaseGen: 2}}
	for i := 0; i < MaxHeldRegions+10; i++ {
		big = append(big, HeldRegion{Region: int64(1000 + i), LeaseGen: 1})
	}
	f.clk.Advance(2 * time.Second)
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{}, big...); err != nil {
		t.Fatalf("oversized held list must still renew: %v", err)
	}
	got := f.reg.Processes()[0].Held
	if len(got) != MaxHeldRegions || got[0] != (HeldRegion{Region: 1, LeaseGen: 2}) { // a duplicate keeps its highest generation
		t.Fatalf("recorded %d entries, first %+v", len(got), got[0])
	}
	if d := testutil.ToFloat64(f.reg.m.heldDropped); d != float64(len(big)-MaxHeldRegions) {
		t.Fatalf("dropped counter %v", d)
	}
	f.clk.Advance(1500 * time.Millisecond)
	if n := f.reg.Sweep(ctx); n != 0 {
		t.Fatal("the renewal with an oversized report did not renew the lease")
	}
	// A stale registration is told lease_lost whatever it reports, so it never lingers.
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch+1, Load{}, big...); err != ErrLeaseLost {
		t.Fatalf("stale epoch with an oversized report: %v", err)
	}
	f.clk.Advance(4 * time.Second)
	if _, err := f.reg.Heartbeat(ctx, a.ProcessID, a.Epoch, Load{}, big...); err != ErrLeaseLost {
		t.Fatalf("lapsed lease with an oversized report: %v", err)
	}
}

func TestPlacementCapsRegionsPerCell(t *testing.T) {
	zones := make([]Zone, MaxHeldRegions+5)
	for i := range zones {
		zones[i] = Zone{ID: int64(1 + i), Name: fmt.Sprintf("z%d", i)}
	}
	f := newReg(t, zones...)
	ctx := context.Background()
	a, _ := f.reg.Register(ctx, cell("cell-a"))
	if len(a.Assignments) != MaxHeldRegions {
		t.Fatalf("a cell got %d regions, cap %d", len(a.Assignments), MaxHeldRegions)
	}
	if n := testutil.ToFloat64(f.reg.m.zonesUnplaced); n != 5 {
		t.Fatalf("zones left unplaced by the cap: gauge %v, want 5", n)
	}
	b, _ := f.reg.Register(ctx, cell("cell-b"))
	if len(b.Assignments) != 5 {
		t.Fatalf("the rest goes to the next cell: %d", len(b.Assignments))
	}
	if n := testutil.ToFloat64(f.reg.m.zonesUnplaced); n != 0 {
		t.Fatalf("unplaced gauge %v after the second cell", n)
	}
}

func TestPlacementWritesRegionLeases(t *testing.T) {
	f := newReg(t)
	ctx := context.Background()
	rs, _ := f.store.ListRegions(ctx)
	if len(rs) != 1 || rs[0] != (Region{ID: WholeRegion(1001), InstanceID: 1001}) {
		t.Fatalf("a v0 zone has exactly one region_lease row: %+v", rs)
	}
	a, _ := f.reg.Register(ctx, cell("cell-a"))
	rs, _ = f.store.ListRegions(ctx)
	if rs[0].Holder != a.ProcessID || rs[0].LeaseGen != 1 || a.Assignments[0].LeaseGen != rs[0].LeaseGen {
		t.Fatalf("assignment: %+v %+v", rs, a.Assignments)
	}
	_ = f.reg.Deregister(ctx, a.ProcessID, a.Epoch)
	rs, _ = f.store.ListRegions(ctx)
	if rs[0].Holder != 0 || rs[0].LeaseGen != 1 {
		t.Fatalf("release keeps the generation: %+v", rs)
	}
	b, _ := f.reg.Register(ctx, cell("cell-b"))
	rs, _ = f.store.ListRegions(ctx)
	if rs[0].Holder != b.ProcessID || rs[0].LeaseGen != 2 || b.Assignments[0].LeaseGen != 2 {
		t.Fatalf("reassignment bumps region_lease.lease_gen: %+v", rs)
	}
}

func TestSanitizeHeldDropsInvalidAndDuplicateEntries(t *testing.T) {
	got, dropped := sanitizeHeld([]HeldRegion{{Region: 7, LeaseGen: 2}, {Region: 0, LeaseGen: 1},
		{Region: 5, LeaseGen: -1}, {Region: 3, LeaseGen: 0}, {Region: 7, LeaseGen: 9}})
	if want := []HeldRegion{{Region: 3, LeaseGen: 0}, {Region: 7, LeaseGen: 9}}; !slices.Equal(got, want) || dropped != 3 {
		t.Fatalf("got %v, dropped %d", got, dropped)
	}
	// Over the cap, the lowest regions stay, whatever the order of the report.
	var asc, desc []HeldRegion
	for i := int64(1); i <= MaxHeldRegions+4; i++ {
		asc = append(asc, HeldRegion{Region: i, LeaseGen: 1})
		desc = append([]HeldRegion{{Region: i, LeaseGen: 1}}, desc...)
	}
	a, da := sanitizeHeld(asc)
	d, dd := sanitizeHeld(desc)
	if !slices.Equal(a, d) || da != 4 || dd != 4 || a[0].Region != 1 || a[len(a)-1].Region != MaxHeldRegions {
		t.Fatalf("order-dependent: %d..%d (%d dropped) vs %d..%d (%d dropped)", a[0].Region, a[len(a)-1].Region, da,
			d[0].Region, d[len(d)-1].Region, dd)
	}
}
