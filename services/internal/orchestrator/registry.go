// Package orchestrator is the Phase 0 orchestrator / world directory (05 §1.4, 04 §1): one
// leader per shard, anchored in PostgreSQL with term-fenced writes (§1.4.1); processes (cells and
// gateways) register over NATS and prove liveness with a 1 Hz heartbeat; every registration gets
// a per-name epoch and records its failure domain and server build, and minting processes get
// time-prefixed ID blocks (§1.4.5); zones are placed on cells (v0: one cell per zone, one region
// per zone) under region_lease generations allocated in PostgreSQL before the holder is told
// (§1.4.2), and heartbeats report the regions held; the world directory answers "which cell owns
// zone X"; and a local supervisor spawns and restarts configured executables.
package orchestrator

import (
	"cmp"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/netip"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/idgen"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// Process kinds.
const (
	KindCell    = "cell"
	KindGateway = "gateway"
)

// ProcessInfo is what a process declares when it registers (05 §1.4 RegisterProcess).
type ProcessInfo struct {
	Name     string   `json:"name"`              // logical name, e.g. "cell-a", "gw-1"
	Kind     string   `json:"kind"`              // cell | gateway
	Address  string   `json:"address,omitempty"` // gateway: public UDP ip:port; cell: trunk address
	Host     string   `json:"host,omitempty"`    // superseded by FD.Host, which it fills when that is empty
	PID      int      `json:"pid,omitempty"`
	Version  string   `json:"version,omitempty"`
	Zones    []string `json:"zones,omitempty"`    // cell: zones it serves; empty = any
	KeyID    uint32   `json:"keyId,omitempty"`    // gateway: shard netcode key generation it runs with
	Capacity int      `json:"capacity,omitempty"` // gateway: session slots
	// FD is the failure domain (05 §1.4.3): from node labels under Agones, from helios.toml [fd]
	// under helios-agent. Phase 0 stores it; Phase 2's detection and placement use it.
	FD FailureDomain `json:"fd,omitzero"`
	// ServerBuild is the server build the process runs (05 §1.4.6's poison-build brake).
	ServerBuild int64 `json:"serverBuild,string,omitempty"`
}

// FailureDomain locates a process: availability zone, rack and host (05 §1.4.3).
type FailureDomain struct {
	AZ   string `json:"az,omitempty"`
	Rack string `json:"rack,omitempty"`
	Host string `json:"host,omitempty"`
}

// Input bounds for registrations and heartbeats (hostile or broken clients must not grow the
// registry without limit).
const (
	MaxFDLabel      = 64  // az, rack
	MaxFDHost       = 255 // a DNS name
	MaxHeldRegions  = 256 // regions a heartbeat's report keeps, and regions one process is placed (a v1 zone has up to 64)
	maxZones        = 256 // zones one cell may declare
	maxZoneName     = 64  // bytes per declared zone name
	maxProcessField = 255 // address, version
)

// HeldRegion is a region a process holds and the lease generation it holds it under, as its
// heartbeats report them (05 §1.4). v0 regions are whole zones, so Region is the zone ID.
type HeldRegion struct {
	Region   int64 `json:"region,string"`
	LeaseGen int64 `json:"leaseGen,string"`
}

// Load is reported with every heartbeat.
type Load struct {
	Players   int     `json:"players"`
	FreeSlots int     `json:"freeSlots"`
	TickP99Ms float64 `json:"tickP99Ms"`
}

// Process is a live registration.
type Process struct {
	ID            int64       `json:"processId,string"`
	Epoch         int64       `json:"epoch,string"`
	Info          ProcessInfo `json:"info"`
	Load          Load        `json:"load"`
	RegisteredAt  time.Time   `json:"registeredAt"`
	LastHeartbeat time.Time   `json:"lastHeartbeat"`
	LeaseExpires  time.Time   `json:"leaseExpires"`
	// Held is what the process's last heartbeat said it holds, by region. Phase 0 records it;
	// the degraded-mode exit (05 §1.4.4) reconciles it against region_lease from Phase 2.
	Held []HeldRegion `json:"held"`
}

// Assignment is a zone placed on a process under its region's lease generation (v0: the zone's
// Whole region, so ZoneID also names the region).
type Assignment struct {
	ZoneID   int64  `json:"zoneId,string"`
	ZoneName string `json:"zoneName"`
	LeaseGen int64  `json:"leaseGen,string"`
}

// Route answers a world-directory query.
type Route struct {
	ZoneID    int64  `json:"zoneId,string"`
	ZoneName  string `json:"zoneName"`
	LeaseGen  int64  `json:"leaseGen,string"`
	ProcessID int64  `json:"processId,string"`
	Process   string `json:"process"`
	Epoch     int64  `json:"epoch,string"`
	Address   string `json:"address"`
}

// GatewayInstance is a live gateway the session service can put into connect tokens.
type GatewayInstance struct {
	ProcessID int64
	Name      string
	Address   netip.AddrPort
	KeyID     uint32
	FreeSlots int
}

// ErrLeaseLost tells a holder that the orchestrator no longer knows its registration (expired,
// superseded, unknown, or a new leader took over): it must stop acting as the owner of its zones
// and register again as a new epoch. This explicit answer is the only thing that fences a holder;
// being unable to reach the orchestrator never does (05 §1.4.2).
var ErrLeaseLost = rpc.Errorf(rpc.CodeFailedPrecondition, "lease_lost")

// errNotLeaderRPC answers callers while a mutation found the leadership term superseded.
var errNotLeaderRPC = rpc.Errorf(rpc.CodeUnavailable, "orchestrator leadership changed; retry")

// ModeNormal is the control-plane mode reported in heartbeat replies (degraded mode, 05 §1.4.4,
// arrives in Phase 2).
const ModeNormal = "normal"

// Events receives registry changes (NATS KV mirror and event publishing in production; nil in
// unit tests). Calls happen with the registry lock held and must not block.
type Events interface {
	ProcessUp(p Process)
	ProcessDown(p Process, reason string)
	ZoneChanged(z Zone, owner *Process)
}

// Config tunes the registry.
type Config struct {
	// LeaseTTL is the liveness TTL: a process silent for this long is expired and its zones
	// are placed elsewhere (05 §1.4.2's single-signal fallback, 12 s by default).
	LeaseTTL          time.Duration
	HeartbeatInterval time.Duration
	// GatewayFreshness excludes gateways from new connect tokens once they have missed this
	// much heartbeating, long before their lease expires (default 3 x HeartbeatInterval).
	GatewayFreshness time.Duration
	Zones            []Zone // configured zones (ID, Name)
	// IDShard is the shard index of ID blocks handed to minting processes.
	IDShard int
	// IDBlocks is how many blocks a cell receives when it registers (default 2, 05 §1.4.5).
	IDBlocks int
}

// Registry is the in-memory lease table backed by Store. Only the shard's leader serves it;
// every write it makes to the store carries the leader's term. Safe for concurrent use.
type Registry struct {
	mu     sync.Mutex
	cfg    Config
	store  Store
	clk    clock.Clock
	log    *slog.Logger
	events Events
	m      *metrics
	// OnNotLeader, if set, is called (in its own goroutine) with the term a store write was
	// fenced with when that term turned out to be superseded, so the service steps down at once.
	OnNotLeader func(term int64)

	fence Fence
	procs map[int64]*Process
	zones map[int64]*Zone
}

type metrics struct {
	processes     *prometheus.GaugeVec
	zonesAssigned prometheus.Gauge
	registrations *prometheus.CounterVec
	ended         *prometheus.CounterVec
	assignments   prometheus.Counter
	heldDropped   prometheus.Counter
}

func newMetrics(reg prometheus.Registerer) *metrics {
	m := &metrics{
		processes: prometheus.NewGaugeVec(prometheus.GaugeOpts{Name: "helios_orchestrator_processes",
			Help: "Live registered processes by kind."}, []string{"kind"}),
		zonesAssigned: prometheus.NewGauge(prometheus.GaugeOpts{Name: "helios_orchestrator_zones_assigned",
			Help: "Zones currently placed on a live cell."}),
		registrations: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "helios_orchestrator_registrations_total",
			Help: "Process registrations by kind."}, []string{"kind"}),
		ended: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "helios_orchestrator_processes_ended_total",
			Help: "Registrations ended by reason (lease_expired, superseded, deregistered)."}, []string{"reason"}),
		assignments: prometheus.NewCounter(prometheus.CounterOpts{Name: "helios_orchestrator_zone_assignments_total",
			Help: "Zone placements (each bumps the region_lease generation of the zone's region)."}),
		heldDropped: prometheus.NewCounter(prometheus.CounterOpts{Name: "helios_orchestrator_held_entries_dropped_total",
			Help: "Held-region entries a heartbeat reported that were not recorded (invalid, duplicate or over the cap)."}),
	}
	if reg != nil {
		reg.MustRegister(m.processes, m.zonesAssigned, m.registrations, m.ended, m.assignments, m.heldDropped)
	}
	return m
}

// NewRegistry builds a registry. Call Load (as the leader) before serving.
func NewRegistry(cfg Config, store Store, clk clock.Clock, log *slog.Logger, events Events, reg prometheus.Registerer) *Registry {
	if clk == nil {
		clk = clock.System{}
	}
	if log == nil {
		log = slog.Default()
	}
	if cfg.GatewayFreshness <= 0 {
		cfg.GatewayFreshness = 3 * cfg.HeartbeatInterval
	}
	if cfg.IDBlocks <= 0 {
		cfg.IDBlocks = 2
	}
	return &Registry{cfg: cfg, store: store, clk: clk, log: log, events: events, m: newMetrics(reg),
		procs: map[int64]*Process{}, zones: map[int64]*Zone{}}
}

// Load starts a leadership term: it syncs configured zones into the store and clears ownership
// left by the previous leader. Those leases died with it, so live processes re-register (their
// next heartbeat returns lease_lost). Every later store write is fenced with f.
func (r *Registry) Load(ctx context.Context, f Fence) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.fence = f
	r.procs = map[int64]*Process{}
	now := r.clk.Now()
	if err := r.store.EnsureZones(ctx, f, r.cfg.Zones, now); err != nil {
		return fmt.Errorf("orchestrator: ensure zones: %w", err)
	}
	if err := r.store.ResetOwners(ctx, f, now); err != nil {
		return fmt.Errorf("orchestrator: reset owners: %w", err)
	}
	zones, err := r.store.ListZones(ctx)
	if err != nil {
		return err
	}
	r.zones = map[int64]*Zone{}
	for i := range zones {
		z := zones[i]
		r.zones[z.ID] = &z
	}
	r.updateGaugesLocked()
	return nil
}

// Reset forgets every registration and zone (the leader stepped down; a successor owns the
// state now). Subsequent heartbeats from anyone return lease_lost.
func (r *Registry) Reset() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.fence = Fence{}
	r.procs = map[int64]*Process{}
	r.zones = map[int64]*Zone{}
	r.updateGaugesLocked()
}

// storeErr maps a store failure to the caller's error and triggers a step-down when the
// leadership term moved on. Callers hold r.mu.
func (r *Registry) storeErr(err error) error {
	if errors.Is(err, ErrNotLeader) {
		if r.OnNotLeader != nil {
			go r.OnNotLeader(r.fence.Term)
		}
		return errNotLeaderRPC
	}
	return rpc.Internal(err)
}

func validate(info *ProcessInfo) error {
	if info.Name == "" || len(info.Name) > 64 {
		return rpc.Errorf(rpc.CodeInvalidArgument, "name must be 1-64 characters")
	}
	if info.FD.Host == "" {
		info.FD.Host = info.Host
	}
	switch {
	case len(info.FD.AZ) > MaxFDLabel || len(info.FD.Rack) > MaxFDLabel || len(info.FD.Host) > MaxFDHost || len(info.Host) > MaxFDHost:
		return rpc.Errorf(rpc.CodeInvalidArgument, "fd: az and rack are at most %d bytes, host at most %d", MaxFDLabel, MaxFDHost)
	case info.ServerBuild < 0:
		return rpc.Errorf(rpc.CodeInvalidArgument, "serverBuild must not be negative")
	case len(info.Address) > maxProcessField || len(info.Version) > maxProcessField || len(info.Zones) > maxZones:
		return rpc.Errorf(rpc.CodeInvalidArgument, "address or version too long, or too many zones")
	case hasNUL(info.Name, info.Address, info.Host, info.Version, info.FD.AZ, info.FD.Rack, info.FD.Host):
		return rpc.Errorf(rpc.CodeInvalidArgument, "text fields must not contain NUL")
	}
	for _, z := range info.Zones {
		if z == "" || len(z) > maxZoneName || hasNUL(z) {
			return rpc.Errorf(rpc.CodeInvalidArgument, "zone names are 1-%d bytes without NUL", maxZoneName)
		}
	}
	switch info.Kind {
	case KindCell:
	case KindGateway:
		if _, err := netip.ParseAddrPort(info.Address); err != nil {
			return rpc.Errorf(rpc.CodeInvalidArgument, "gateway address must be ip:port: %v", err)
		}
		if info.KeyID == 0 {
			return rpc.Errorf(rpc.CodeInvalidArgument, "gateway must report its shard key id")
		}
	default:
		return rpc.Errorf(rpc.CodeInvalidArgument, "kind must be cell or gateway")
	}
	return nil
}

// hasNUL reports whether any s contains a NUL byte (PostgreSQL TEXT cannot store one).
func hasNUL(s ...string) bool {
	for _, x := range s {
		if strings.IndexByte(x, 0) >= 0 {
			return true
		}
	}
	return false
}

// RegisterResult is returned to a registering process.
type RegisterResult struct {
	ProcessID           int64        `json:"processId,string"`
	Epoch               int64        `json:"epoch,string"`
	LeaseTTLMs          int64        `json:"leaseTtlMs"`
	HeartbeatIntervalMs int64        `json:"heartbeatIntervalMs"`
	Assignments         []Assignment `json:"assignments"`
	// IDShard and IDBlocks: the first ID-block prefixes for minting processes (cells); IDs are
	// idgen.Compose(prefix, IDShard, offset). Gateways never mint and get none (05 §1.4.5).
	IDShard  int          `json:"idShard"`
	IDBlocks Int64Strings `json:"idBlocks"`
}

// Int64Strings is a []int64 that encodes each element as a JSON string (proto3 JSON's int64
// mapping, like every other 64-bit field in these messages).
type Int64Strings []int64

// MarshalJSON implements json.Marshaler.
func (v Int64Strings) MarshalJSON() ([]byte, error) {
	out := make([]string, len(v))
	for i, x := range v {
		out[i] = strconv.FormatInt(x, 10)
	}
	return json.Marshal(out)
}

// UnmarshalJSON implements json.Unmarshaler.
func (v *Int64Strings) UnmarshalJSON(b []byte) error {
	var in []string
	if err := json.Unmarshal(b, &in); err != nil {
		return err
	}
	out := make(Int64Strings, len(in))
	for i, x := range in {
		n, err := strconv.ParseInt(x, 10, 64)
		if err != nil {
			return err
		}
		out[i] = n
	}
	*v = out
	return nil
}

// Register records a new incarnation. A live registration with the same name is superseded
// (the supervisor restarted a crashed process before its lease lapsed); its zones move to the
// new incarnation under a new lease generation.
func (r *Registry) Register(ctx context.Context, info ProcessInfo) (*RegisterResult, error) {
	if err := validate(&info); err != nil {
		return nil, err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.fence.Term == 0 {
		return nil, errNotLeaderRPC
	}
	now := r.clk.Now()
	for _, p := range r.procs {
		if p.Info.Name == info.Name {
			r.endLocked(ctx, p, "superseded", now)
		}
	}
	var blocks []int64
	if info.Kind == KindCell {
		var err error
		if blocks, err = r.store.AllocateIdBlocks(ctx, r.cfg.IDShard, r.cfg.IDBlocks); err != nil {
			return nil, r.storeErr(err)
		}
	}
	rec, err := r.store.CreateProcess(ctx, r.fence, info, now)
	if err != nil {
		return nil, r.storeErr(err)
	}
	p := &Process{ID: rec.ID, Epoch: rec.Epoch, Info: info, RegisteredAt: now, LastHeartbeat: now,
		LeaseExpires: now.Add(r.cfg.LeaseTTL), Held: []HeldRegion{}}
	if info.Kind == KindGateway {
		p.Load.FreeSlots = info.Capacity
	}
	r.procs[p.ID] = p
	r.m.registrations.WithLabelValues(info.Kind).Inc()
	r.log.Info("process registered", "process", p.ID, "name", info.Name, "kind", info.Kind, "epoch", p.Epoch)
	if r.events != nil {
		r.events.ProcessUp(*p)
	}
	r.placeLocked(ctx, now)
	r.updateGaugesLocked()
	return &RegisterResult{ProcessID: p.ID, Epoch: p.Epoch, LeaseTTLMs: r.cfg.LeaseTTL.Milliseconds(),
		HeartbeatIntervalMs: r.cfg.HeartbeatInterval.Milliseconds(), Assignments: r.assignmentsLocked(p.ID),
		IDShard: r.cfg.IDShard, IDBlocks: blocks}, nil
}

// AllocateIdBlocks hands a registered minting process n more block prefixes (n ≤ 16).
func (r *Registry) AllocateIdBlocks(ctx context.Context, id, epoch int64, n int) ([]int64, error) {
	if n < 1 || n > idgen.MaxBlocksPerCall {
		return nil, rpc.Errorf(rpc.CodeInvalidArgument, "n must be within 1..%d", idgen.MaxBlocksPerCall)
	}
	r.mu.Lock()
	p, ok := r.procs[id]
	live := ok && p.Epoch == epoch && !r.clk.Now().After(p.LeaseExpires)
	kind := ""
	if ok {
		kind = p.Info.Kind
	}
	r.mu.Unlock()
	if !live {
		return nil, ErrLeaseLost
	}
	if kind != KindCell {
		return nil, rpc.Errorf(rpc.CodePermissionDenied, "only minting processes (cells) receive ID blocks")
	}
	blocks, err := r.store.AllocateIdBlocks(ctx, r.cfg.IDShard, n)
	if err != nil {
		return nil, rpc.Internal(err) // unfenced (05 §1.4.5), so never ErrNotLeader
	}
	return blocks, nil
}

// HeartbeatResult tells the holder its current assignments (each with its lease generation)
// and the shard's control-plane mode.
type HeartbeatResult struct {
	LeaseExpires time.Time    `json:"leaseExpires"`
	Assignments  []Assignment `json:"assignments"`
	Mode         string       `json:"mode"`
}

// Heartbeat renews a lease and records the regions the process says it holds. A heartbeat that
// arrives after the lease lapsed is refused: the zones may already have moved, so the holder is
// told lease_lost and registers again. The held report never blocks the renewal: Phase 0 only
// records it, so invalid or duplicate entries and those over MaxHeldRegions are dropped (and
// counted) rather than refused, and lease_lost always wins over a bad report.
func (r *Registry) Heartbeat(ctx context.Context, id, epoch int64, load Load, held ...HeldRegion) (*HeartbeatResult, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := r.clk.Now()
	p, ok := r.procs[id]
	if !ok || p.Epoch != epoch {
		return nil, ErrLeaseLost
	}
	if now.After(p.LeaseExpires) {
		r.endLocked(ctx, p, "lease_expired", now)
		r.placeLocked(ctx, now)
		r.updateGaugesLocked()
		return nil, ErrLeaseLost
	}
	p.LastHeartbeat = now
	p.LeaseExpires = now.Add(r.cfg.LeaseTTL)
	p.Load = load
	var dropped int
	p.Held, dropped = sanitizeHeld(held)
	if dropped > 0 {
		r.m.heldDropped.Add(float64(dropped))
		r.log.Debug("held report entries dropped", "process", p.ID, "dropped", dropped)
	}
	return &HeartbeatResult{LeaseExpires: p.LeaseExpires, Assignments: r.assignmentsLocked(id), Mode: ModeNormal}, nil
}

// sanitizeHeld keeps the valid, distinct entries of a held report (at most MaxHeldRegions, by
// region) and returns how many it dropped.
func sanitizeHeld(held []HeldRegion) ([]HeldRegion, int) {
	out := make([]HeldRegion, 0, min(len(held), MaxHeldRegions))
	seen := make(map[int64]bool, len(out))
	for _, h := range held {
		if h.Region <= 0 || h.LeaseGen < 0 || seen[h.Region] || len(out) == MaxHeldRegions {
			continue
		}
		seen[h.Region] = true
		out = append(out, h)
	}
	slices.SortFunc(out, func(a, b HeldRegion) int { return cmp.Compare(a.Region, b.Region) })
	return out, len(held) - len(out)
}

// Deregister ends a registration cleanly (graceful shutdown).
func (r *Registry) Deregister(ctx context.Context, id, epoch int64) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	p, ok := r.procs[id]
	if !ok || p.Epoch != epoch {
		return ErrLeaseLost
	}
	now := r.clk.Now()
	r.endLocked(ctx, p, "deregistered", now)
	r.placeLocked(ctx, now)
	r.updateGaugesLocked()
	return nil
}

// Sweep expires lapsed leases and places unassigned zones. The service calls it periodically;
// tests call it directly with a fake clock. It returns the number of expired registrations.
func (r *Registry) Sweep(ctx context.Context) int {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := r.clk.Now()
	expired := 0
	for _, id := range r.sortedIDsLocked() {
		p := r.procs[id]
		if now.After(p.LeaseExpires) {
			r.endLocked(ctx, p, "lease_expired", now)
			expired++
		}
	}
	r.placeLocked(ctx, now)
	r.updateGaugesLocked()
	return expired
}

func (r *Registry) sortedIDsLocked() []int64 {
	ids := make([]int64, 0, len(r.procs))
	for id := range r.procs {
		ids = append(ids, id)
	}
	slices.Sort(ids)
	return ids
}

// endLocked removes a registration and releases its zones.
func (r *Registry) endLocked(ctx context.Context, p *Process, reason string, now time.Time) {
	delete(r.procs, p.ID)
	for _, z := range r.zones {
		if z.Owner == p.ID {
			if err := r.store.ReleaseRegion(ctx, r.fence, WholeRegion(z.ID), p.ID, now); err != nil {
				r.log.Error("zone release failed", "zone", z.ID, "err", err)
				_ = r.storeErr(err)
			}
			z.Owner = 0
			if r.events != nil {
				r.events.ZoneChanged(*z, nil)
			}
		}
	}
	if err := r.store.EndProcess(ctx, r.fence, p.ID, reason, now); err != nil {
		r.log.Error("process end failed", "process", p.ID, "err", err)
		_ = r.storeErr(err)
	}
	r.m.ended.WithLabelValues(reason).Inc()
	r.log.Info("process ended", "process", p.ID, "name", p.Info.Name, "epoch", p.Epoch, "reason", reason)
	if r.events != nil {
		r.events.ProcessDown(*p, reason)
	}
}

// placeLocked assigns every unowned zone to a live cell: cells that declared the zone first,
// then cells that accept any zone; the least-loaded candidate wins (ties: lowest process ID). No
// cell gets more than MaxHeldRegions regions, so its whole held report is always recorded.
func (r *Registry) placeLocked(ctx context.Context, now time.Time) {
	zoneIDs := make([]int64, 0, len(r.zones))
	for id := range r.zones {
		zoneIDs = append(zoneIDs, id)
	}
	slices.Sort(zoneIDs)
	owned := map[int64]int{}
	for _, z := range r.zones {
		if z.Owner != 0 {
			owned[z.Owner]++
		}
	}
	for _, zid := range zoneIDs {
		z := r.zones[zid]
		if z.Owner != 0 {
			continue
		}
		var best *Process
		bestDeclared := false
		for _, id := range r.sortedIDsLocked() {
			p := r.procs[id]
			if p.Info.Kind != KindCell || now.After(p.LeaseExpires) || owned[p.ID] >= MaxHeldRegions {
				continue
			}
			declared := slices.Contains(p.Info.Zones, z.Name)
			if !declared && len(p.Info.Zones) > 0 {
				continue
			}
			if best == nil || (declared && !bestDeclared) || (declared == bestDeclared && owned[p.ID] < owned[best.ID]) {
				best, bestDeclared = p, declared
			}
		}
		if best == nil {
			continue
		}
		gen, err := r.store.AssignRegion(ctx, r.fence, WholeRegion(z.ID), best.ID, now)
		if err != nil {
			r.log.Error("zone assignment failed", "zone", z.ID, "process", best.ID, "err", err)
			_ = r.storeErr(err)
			continue
		}
		z.Owner, z.LeaseGen = best.ID, gen
		owned[best.ID]++
		r.m.assignments.Inc()
		r.log.Info("zone assigned", "zone", z.ID, "name", z.Name, "process", best.ID, "cell", best.Info.Name, "lease_gen", gen)
		if r.events != nil {
			r.events.ZoneChanged(*z, best)
		}
	}
}

func (r *Registry) assignmentsLocked(id int64) []Assignment {
	out := []Assignment{}
	for _, z := range r.zones {
		if z.Owner == id {
			out = append(out, Assignment{ZoneID: z.ID, ZoneName: z.Name, LeaseGen: z.LeaseGen})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ZoneID < out[j].ZoneID })
	return out
}

func (r *Registry) updateGaugesLocked() {
	counts := map[string]int{KindCell: 0, KindGateway: 0}
	for _, p := range r.procs {
		counts[p.Info.Kind]++
	}
	for k, n := range counts {
		r.m.processes.WithLabelValues(k).Set(float64(n))
	}
	assigned := 0
	for _, z := range r.zones {
		if z.Owner != 0 {
			assigned++
		}
	}
	r.m.zonesAssigned.Set(float64(assigned))
}

// ResolveZone answers the world-directory query by zone ID or name.
func (r *Registry) ResolveZone(zoneID int64, name string) (*Route, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	var z *Zone
	for _, x := range r.zones {
		if (zoneID != 0 && x.ID == zoneID) || (zoneID == 0 && name != "" && x.Name == name) {
			z = x
			break
		}
	}
	if z == nil {
		return nil, rpc.Errorf(rpc.CodeNotFound, "unknown zone")
	}
	p, ok := r.procs[z.Owner]
	if z.Owner == 0 || !ok {
		return nil, rpc.Errorf(rpc.CodeUnavailable, "zone %s has no live cell", z.Name)
	}
	return &Route{ZoneID: z.ID, ZoneName: z.Name, LeaseGen: z.LeaseGen, ProcessID: p.ID, Process: p.Info.Name,
		Epoch: p.Epoch, Address: p.Info.Address}, nil
}

// Processes returns a snapshot of live registrations ordered by ID.
func (r *Registry) Processes() []Process {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]Process, 0, len(r.procs))
	for _, id := range r.sortedIDsLocked() {
		out = append(out, *r.procs[id])
	}
	return out
}

// Zones returns a snapshot of all zones ordered by ID.
func (r *Registry) Zones() []Zone {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]Zone, 0, len(r.zones))
	for _, z := range r.zones {
		out = append(out, *z)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// Gateways returns gateway instances that heartbeated recently (session service input): a
// gateway that missed GatewayFreshness of heartbeats gets no new tokens even while its lease
// still holds, so clients are not sent to an instance that may be gone.
func (r *Registry) Gateways() []GatewayInstance {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := r.clk.Now()
	var out []GatewayInstance
	for _, id := range r.sortedIDsLocked() {
		p := r.procs[id]
		if p.Info.Kind != KindGateway || now.After(p.LeaseExpires) || now.Sub(p.LastHeartbeat) > r.cfg.GatewayFreshness {
			continue
		}
		addr, err := netip.ParseAddrPort(p.Info.Address)
		if err != nil {
			continue
		}
		out = append(out, GatewayInstance{ProcessID: p.ID, Name: p.Info.Name, Address: addr, KeyID: p.Info.KeyID, FreeSlots: p.Load.FreeSlots})
	}
	return out
}

// Ping checks the backing store.
func (r *Registry) Ping(ctx context.Context) error {
	if err := r.store.Ping(ctx); err != nil {
		return errors.Join(errors.New("orchestrator store"), err)
	}
	return nil
}
