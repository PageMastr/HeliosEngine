package orchestrator

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/nats-io/nats.go"
	"github.com/nats-io/nats.go/jetstream"
	"github.com/prometheus/client_golang/prometheus"

	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// RPC method names on rpc.<shard>.orch.<Method> (05 §2.2: the orchestrator's subject domain is
// "orch", e.g. rpc.eu1.orch.Heartbeat, and §6.5 grants cells rpc.<shard>.orch.>). Payloads are
// JSON until schemac emits Helios-binary codecs for nats.c.
const (
	MethodRegister         = "RegisterProcess"
	MethodHeartbeat        = "Heartbeat"
	MethodDeregister       = "Deregister"
	MethodResolveZone      = "ResolveZone"
	MethodListProcesses    = "ListProcesses"
	MethodAllocateIdBlocks = "AllocateIdBlocks"
	queueGroup             = "orchestrator"
	// ServicePath is the Connect-style HTTP prefix on the ops listener.
	ServicePath = "/helios.orchestrator.v1.Orchestrator/"
	// BucketDirectory is the KV read projection of region_lease (zone ownership, v0) for
	// gateways and cells (05 §1.4, §2.3). It is never the authority: PostgreSQL is.
	BucketDirectory = "DIRECTORY"
)

// Leadership timing (05 §1.4.1): the lease lasts 10 s, the leader renews every 2 s and acts
// only while less than 8 s have passed since its last successful renewal.
const (
	DefaultLeaderTTL     = 10 * time.Second
	DefaultRenewInterval = 2 * time.Second
)

// Subject returns the NATS subject of an orchestrator RPC for shard.
func Subject(shard, method string) string { return "rpc." + shard + ".orch." + method }

// HeartbeatRequest renews a lease and reports load and the regions held with their
// generations (05 §1.4 Heartbeat).
type HeartbeatRequest struct {
	ProcessID int64        `json:"processId,string"`
	Epoch     int64        `json:"epoch,string"`
	Load      Load         `json:"load"`
	Held      []HeldRegion `json:"held,omitempty"`
}

// DeregisterRequest ends a registration.
type DeregisterRequest struct {
	ProcessID int64 `json:"processId,string"`
	Epoch     int64 `json:"epoch,string"`
}

// ResolveZoneRequest asks which cell owns a zone (by ID or name).
type ResolveZoneRequest struct {
	ZoneID   int64  `json:"zoneId,string,omitempty"`
	ZoneName string `json:"zoneName,omitempty"`
}

// AllocateIdBlocksRequest asks for n (1..16) more ID-block prefixes for a registered cell.
type AllocateIdBlocksRequest struct {
	ProcessID int64 `json:"processId,string"`
	Epoch     int64 `json:"epoch,string"`
	N         int   `json:"n"`
}

// AllocateIdBlocksResponse carries the prefixes; IDs are idgen.Compose(prefix, IDShard, offset).
type AllocateIdBlocksResponse struct {
	IDShard  int          `json:"idShard"`
	Prefixes Int64Strings `json:"prefixes"`
}

// ZoneView is a zone in ListProcesses.
type ZoneView struct {
	ZoneID   int64  `json:"zoneId,string"`
	Name     string `json:"name"`
	Owner    int64  `json:"owner,string"`
	LeaseGen int64  `json:"leaseGen,string"`
}

// ListProcessesResponse is the shard map.
type ListProcessesResponse struct {
	Leader     bool          `json:"leader"`
	Term       int64         `json:"term,string"`
	Processes  []Process     `json:"processes"`
	Zones      []ZoneView    `json:"zones"`
	Supervised []ChildStatus `json:"supervised"`
}

// Empty is an empty message.
type Empty struct{}

// Deps configures the service.
type Deps struct {
	Config     platform.OrchestratorConfig
	Shard      string
	ShardIndex int // ID-block shard field
	Store      Store
	NATS       *nats.Conn
	Supervisor *Supervisor // optional
	// Holder identifies this replica in orch_leader. It must be unique per live instance and
	// stable across restarts of the same instance, so a restarted instance reclaims its own
	// lease at once (the backend uses host + data directory, which the data-dir lock makes
	// unique). Default: host:pid.
	Holder string
	// LeaderTTL and RenewInterval override the 10 s / 2 s leadership timing (tests).
	LeaderTTL     time.Duration
	RenewInterval time.Duration
	Clock         clock.Clock
	Log           *slog.Logger
	Metrics       prometheus.Registerer
}

// Service runs the registry over NATS while this replica holds the shard's leadership, mirrors
// zone ownership into KV, sweeps leases and owns the local supervisor. A replica that is not the
// leader stays on standby and takes over when the leader's lease expires.
type Service struct {
	reg     *Registry
	store   Store
	sup     *Supervisor
	nc      *nats.Conn
	shard   string
	holder  string
	ttl     time.Duration
	renew   time.Duration
	log     *slog.Logger
	events  *busEvents
	sweep   time.Duration
	leaderG prometheus.Gauge
	termG   prometheus.Gauge

	mu        sync.Mutex
	term      int64     // 0 while on standby
	serving   bool      // handlers subscribed for term
	lastRenew time.Time // monotonic reading of the last successful acquire/renew
	subs      []*nats.Subscription
	stopLead  context.CancelFunc // stops the leader-only sweeper
	leadWG    sync.WaitGroup

	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// New builds the service.
func New(d Deps) (*Service, error) {
	if d.Store == nil || d.NATS == nil || d.Shard == "" {
		return nil, errors.New("orchestrator: missing dependency")
	}
	if d.Log == nil {
		d.Log = slog.Default()
	}
	if d.Holder == "" {
		host, _ := os.Hostname()
		d.Holder = host + ":" + strconv.Itoa(os.Getpid())
	}
	if d.LeaderTTL <= 0 {
		d.LeaderTTL = DefaultLeaderTTL
	}
	if d.RenewInterval <= 0 || d.RenewInterval >= d.LeaderTTL/2 {
		d.RenewInterval = min(DefaultRenewInterval, d.LeaderTTL/5)
	}
	zones := make([]Zone, 0, len(d.Config.Zones))
	for _, z := range d.Config.Zones {
		zones = append(zones, Zone{ID: z.ID, Name: z.Name})
	}
	events := newBusEvents(d.NATS, d.Shard, d.Log, d.Metrics)
	reg := NewRegistry(Config{LeaseTTL: d.Config.LeaseTTL.D(), HeartbeatInterval: d.Config.HeartbeatInterval.D(), Zones: zones,
		IDShard: d.ShardIndex}, d.Store, d.Clock, d.Log, events, d.Metrics)
	sweep := d.Config.LeaseTTL.D() / 4
	if sweep > 250*time.Millisecond {
		sweep = 250 * time.Millisecond
	}
	s := &Service{reg: reg, store: d.Store, sup: d.Supervisor, nc: d.NATS, shard: d.Shard, holder: d.Holder,
		ttl: d.LeaderTTL, renew: d.RenewInterval, log: d.Log, events: events, sweep: sweep,
		leaderG: prometheus.NewGauge(prometheus.GaugeOpts{Name: "helios_orchestrator_leader",
			Help: "1 while this replica holds the shard's orchestrator leadership."}),
		termG: prometheus.NewGauge(prometheus.GaugeOpts{Name: "helios_orchestrator_leader_term",
			Help: "Leadership term held by this replica (0 on standby)."}),
	}
	if d.Metrics != nil {
		d.Metrics.MustRegister(s.leaderG, s.termG)
	}
	reg.OnNotLeader = func(term int64) { s.stepDown(term, "leadership term superseded") }
	return s, nil
}

// Name implements app.Service.
func (s *Service) Name() string { return platform.ServiceOrchestrator }

// Registry exposes the lease table (the session service reads live gateways from it).
func (s *Service) Registry() *Registry { return s.reg }

// Leader reports whether this replica currently serves as the shard's leader, and its term.
func (s *Service) Leader() (bool, int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.serving && s.actingLocked(), s.term
}

// acting is the handlers' check: the term is held and renewed recently enough.
func (s *Service) acting() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.actingLocked()
}

// actingLocked: leader, and within the act window (TTL minus one renew interval, 8 s by
// default) since the last successful renewal, measured on the monotonic clock.
func (s *Service) actingLocked() bool {
	return s.term != 0 && time.Since(s.lastRenew) < s.ttl-s.renew
}

// Health implements app.Service. A standby replica is healthy.
func (s *Service) Health(ctx context.Context) error {
	if !s.nc.IsConnected() {
		return errors.New("orchestrator: bus disconnected")
	}
	return s.reg.Ping(ctx)
}

// Start implements app.Service: it tries to take the leadership once (a single replica leads
// immediately), then keeps renewing it or, on standby, keeps trying.
func (s *Service) Start(ctx context.Context) error {
	if err := s.events.init(ctx); err != nil {
		return err
	}
	runCtx, cancel := context.WithCancel(context.Background())
	s.cancel = cancel
	s.wg.Add(1)
	go func() { defer s.wg.Done(); s.events.run(runCtx) }()
	if err := s.tryLead(ctx); err != nil {
		cancel()
		s.wg.Wait()
		return err
	}
	if ok, _ := s.Leader(); !ok {
		s.log.Info("orchestrator on standby: another replica leads this shard", "shard", s.shard)
	}
	s.wg.Add(1)
	go func() { defer s.wg.Done(); s.leadershipLoop(runCtx) }()
	if s.sup != nil {
		if err := s.sup.Start(ctx); err != nil {
			return errors.Join(err, s.Stop(ctx)) // the runner never stops a service that failed to start
		}
	}
	return nil
}

func (s *Service) leadershipLoop(ctx context.Context) {
	t := time.NewTicker(s.renew)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
		s.mu.Lock()
		term := s.term
		s.mu.Unlock()
		if term == 0 {
			if err := s.tryLead(ctx); err != nil && ctx.Err() == nil {
				s.log.Warn("orchestrator leadership attempt failed", "err", err)
			}
			continue
		}
		rctx, cancel := context.WithTimeout(ctx, s.renew)
		ok, err := s.store.RenewLeadership(rctx, s.shard, s.holder, term, s.ttl)
		cancel()
		switch {
		case err == nil && ok:
			s.mu.Lock()
			if s.term == term {
				s.lastRenew = time.Now()
			}
			s.mu.Unlock()
		case err == nil:
			s.stepDown(term, "leadership term taken over")
		default:
			s.mu.Lock()
			expired := s.term == term && !s.actingLocked()
			s.mu.Unlock()
			s.log.Warn("orchestrator leadership renewal failed", "term", term, "err", err)
			if expired {
				s.stepDown(term, "leadership could not be renewed in time")
			}
		}
	}
}

// tryLead acquires the leadership if it is free, then loads the registry under the new term and
// starts serving.
func (s *Service) tryLead(ctx context.Context) error {
	actx, cancel := context.WithTimeout(ctx, s.ttl/2)
	defer cancel()
	term, ok, err := s.store.AcquireLeadership(actx, s.shard, s.holder, s.ttl)
	if err != nil || !ok {
		return err
	}
	start := time.Now()
	f := Fence{Shard: s.shard, Term: term}
	if err := s.reg.Load(actx, f); err != nil {
		s.reg.Reset()
		_ = s.store.ReleaseLeadership(context.WithoutCancel(ctx), s.shard, s.holder, term)
		return err
	}
	s.mu.Lock()
	s.term, s.lastRenew = term, start
	leadCtx, stop := context.WithCancel(context.Background())
	s.stopLead = stop
	s.mu.Unlock()
	subs, err := s.subscribe()
	s.mu.Lock()
	current := s.term == term
	if current {
		s.subs = append(s.subs, subs...)
	}
	s.mu.Unlock()
	if !current {
		// Stepped down while subscribing (a fenced write failed): drop what was just added.
		for _, sub := range subs {
			_ = sub.Unsubscribe()
		}
		return err
	}
	if err != nil {
		s.stepDown(term, "subscribe failed")
		_ = s.store.ReleaseLeadership(context.WithoutCancel(ctx), s.shard, s.holder, term)
		return err
	}
	s.leadWG.Add(1)
	go func() { defer s.leadWG.Done(); s.sweeper(leadCtx) }()
	s.mu.Lock()
	if s.term == term {
		s.serving = true
	}
	s.mu.Unlock()
	s.leaderG.Set(1)
	s.termG.Set(float64(term))
	s.log.Info("orchestrator leadership acquired", "shard", s.shard, "term", term, "holder", s.holder)
	return nil
}

// stepDown stops serving as the leader of term (no-op when that term is not held any more).
func (s *Service) stepDown(term int64, reason string) {
	s.mu.Lock()
	if term == 0 || s.term != term {
		s.mu.Unlock()
		return
	}
	s.term, s.serving = 0, false
	subs := s.subs
	s.subs = nil
	stop := s.stopLead
	s.stopLead = nil
	s.mu.Unlock()
	for _, sub := range subs {
		_ = sub.Unsubscribe()
	}
	if stop != nil {
		stop()
	}
	s.leadWG.Wait()
	s.reg.Reset()
	s.leaderG.Set(0)
	s.termG.Set(0)
	level := slog.LevelWarn
	if reason == "shutdown" {
		level = slog.LevelInfo
	}
	s.log.Log(context.Background(), level, "orchestrator stepped down", "shard", s.shard, "term", term, "reason", reason)
}

func (s *Service) sweeper(ctx context.Context) {
	t := time.NewTicker(s.sweep)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if !s.acting() {
				continue
			}
			sctx, c := context.WithTimeout(ctx, 5*time.Second)
			s.reg.Sweep(sctx)
			c()
		}
	}
}

// leading wraps a handler so it refuses work once this replica can no longer prove it leads.
func leading[Req, Res any](s *Service, h rpc.NATSHandlerFunc[Req, Res]) rpc.NATSHandlerFunc[Req, Res] {
	return func(ctx context.Context, req *Req) (*Res, error) {
		if !s.acting() {
			return nil, errNotLeaderRPC
		}
		return h(ctx, req)
	}
}

// subscribe registers the leader's handlers and returns the subscriptions (also on error, so
// the caller can drop partial ones).
func (s *Service) subscribe() ([]*nats.Subscription, error) {
	var subs []*nats.Subscription
	add := func(sub *nats.Subscription, err error) error {
		if err != nil {
			return err
		}
		subs = append(subs, sub)
		return nil
	}
	err := errors.Join(
		add(rpc.NATSHandle(s.nc, Subject(s.shard, MethodRegister), queueGroup, s.log,
			leading(s, func(ctx context.Context, req *ProcessInfo) (*RegisterResult, error) { return s.reg.Register(ctx, *req) }))),
		add(rpc.NATSHandle(s.nc, Subject(s.shard, MethodHeartbeat), queueGroup, s.log,
			leading(s, func(ctx context.Context, req *HeartbeatRequest) (*HeartbeatResult, error) {
				return s.reg.Heartbeat(ctx, req.ProcessID, req.Epoch, req.Load, req.Held...)
			}))),
		add(rpc.NATSHandle(s.nc, Subject(s.shard, MethodDeregister), queueGroup, s.log,
			leading(s, func(ctx context.Context, req *DeregisterRequest) (*Empty, error) {
				return &Empty{}, s.reg.Deregister(ctx, req.ProcessID, req.Epoch)
			}))),
		add(rpc.NATSHandle(s.nc, Subject(s.shard, MethodResolveZone), queueGroup, s.log,
			leading(s, func(_ context.Context, req *ResolveZoneRequest) (*Route, error) {
				return s.reg.ResolveZone(req.ZoneID, req.ZoneName)
			}))),
		add(rpc.NATSHandle(s.nc, Subject(s.shard, MethodAllocateIdBlocks), queueGroup, s.log,
			leading(s, func(ctx context.Context, req *AllocateIdBlocksRequest) (*AllocateIdBlocksResponse, error) {
				blocks, err := s.reg.AllocateIdBlocks(ctx, req.ProcessID, req.Epoch, req.N)
				if err != nil {
					return nil, err
				}
				return &AllocateIdBlocksResponse{IDShard: s.reg.cfg.IDShard, Prefixes: blocks}, nil
			}))),
		add(rpc.NATSHandle(s.nc, Subject(s.shard, MethodListProcesses), queueGroup, s.log,
			func(context.Context, *Empty) (*ListProcessesResponse, error) { return s.list(), nil })),
	)
	if err == nil {
		err = s.nc.Flush()
	}
	if err != nil {
		return subs, fmt.Errorf("orchestrator: subscribe: %w", err)
	}
	return subs, nil
}

func (s *Service) list() *ListProcessesResponse {
	leader, term := s.Leader()
	out := &ListProcessesResponse{Leader: leader, Term: term, Processes: s.reg.Processes(), Zones: []ZoneView{},
		Supervised: []ChildStatus{}}
	for _, z := range s.reg.Zones() {
		out.Zones = append(out.Zones, ZoneView{ZoneID: z.ID, Name: z.Name, Owner: z.Owner, LeaseGen: z.LeaseGen})
	}
	if s.sup != nil {
		out.Supervised = s.sup.Status()
	}
	return out
}

// Stop implements app.Service: children first (they deregister), then the bus handlers, then
// the leadership is released so a successor need not wait for the lease to expire.
func (s *Service) Stop(ctx context.Context) error {
	var errs []error
	if s.sup != nil {
		errs = append(errs, s.sup.Stop(ctx))
	}
	if s.cancel != nil {
		s.cancel()
	}
	s.wg.Wait()
	s.mu.Lock()
	term := s.term
	s.mu.Unlock()
	if term != 0 {
		s.stepDown(term, "shutdown")
		if err := s.store.ReleaseLeadership(ctx, s.shard, s.holder, term); err != nil {
			errs = append(errs, fmt.Errorf("orchestrator: release leadership: %w", err))
		}
	}
	return errors.Join(errs...)
}

// Mount registers the directory API on the ops router.
func (s *Service) Mount(r chi.Router) {
	opts := rpc.UnaryOptions{Logger: s.log}
	r.Post(ServicePath+MethodResolveZone, rpc.Unary(func(_ context.Context, _ *http.Request, req *ResolveZoneRequest) (*Route, error) {
		return s.reg.ResolveZone(req.ZoneID, req.ZoneName)
	}, opts))
	r.Post(ServicePath+MethodListProcesses, rpc.Unary(func(context.Context, *http.Request, *Empty) (*ListProcessesResponse, error) {
		return s.list(), nil
	}, opts))
}

// --- NATS KV mirror and events ---------------------------------------------------------------

// busEvents mirrors zone ownership into the DIRECTORY KV bucket and publishes
// evt.<shard>.orch.* notifications. Registry callbacks only enqueue; a single goroutine
// does the I/O so a slow bus never stalls lease handling. There is deliberately no LEASES bucket:
// generations and leadership live in PostgreSQL (05 §2.3).
type busEvents struct {
	nc      *nats.Conn
	shard   string
	log     *slog.Logger
	queue   chan func(context.Context)
	dir     jetstream.KeyValue
	dropped prometheus.Counter
}

func newBusEvents(nc *nats.Conn, shard string, log *slog.Logger, reg prometheus.Registerer) *busEvents {
	b := &busEvents{nc: nc, shard: shard, log: log, queue: make(chan func(context.Context), 4096),
		dropped: prometheus.NewCounter(prometheus.CounterOpts{Name: "helios_orchestrator_mirror_dropped_total",
			Help: "KV mirror updates dropped because the queue was full."})}
	if reg != nil {
		reg.MustRegister(b.dropped)
	}
	return b
}

func (b *busEvents) init(ctx context.Context) error {
	js, err := jetstream.New(b.nc)
	if err != nil {
		return err
	}
	if b.dir, err = js.CreateOrUpdateKeyValue(ctx, jetstream.KeyValueConfig{Bucket: BucketDirectory, History: 5,
		Storage: jetstream.MemoryStorage, Description: "zone -> owning cell (read projection of the orchestrator's PostgreSQL state)"}); err != nil {
		return fmt.Errorf("orchestrator: kv %s: %w", BucketDirectory, err)
	}
	return nil
}

func (b *busEvents) run(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case f := <-b.queue:
			fctx, cancel := context.WithTimeout(ctx, 2*time.Second)
			f(fctx)
			cancel()
		}
	}
}

func (b *busEvents) enqueue(f func(context.Context)) {
	select {
	case b.queue <- f:
	default:
		b.dropped.Inc()
	}
}

func (b *busEvents) publish(event string, v any) {
	data, err := json.Marshal(v)
	if err == nil {
		err = b.nc.Publish("evt."+b.shard+".orch."+event, data)
	}
	if err != nil {
		b.log.Warn("orchestrator event publish failed", "event", event, "err", err)
	}
}

func zoneKey(id int64) string { return "zone." + strconv.FormatInt(id, 10) }

// ProcessUp implements Events.
func (b *busEvents) ProcessUp(p Process) {
	b.enqueue(func(context.Context) { b.publish("process.up", p) })
}

// ProcessDown implements Events.
func (b *busEvents) ProcessDown(p Process, reason string) {
	b.enqueue(func(context.Context) {
		b.publish("process.down", struct {
			Process
			Reason string `json:"reason"`
		}{p, reason})
	})
}

// ZoneChanged implements Events.
func (b *busEvents) ZoneChanged(z Zone, owner *Process) {
	route := Route{ZoneID: z.ID, ZoneName: z.Name, LeaseGen: z.LeaseGen}
	if owner != nil {
		route.ProcessID, route.Process, route.Epoch, route.Address = owner.ID, owner.Info.Name, owner.Epoch, owner.Info.Address
	}
	b.enqueue(func(ctx context.Context) {
		if b.dir != nil {
			data, _ := json.Marshal(route)
			if _, err := b.dir.Put(ctx, zoneKey(z.ID), data); err != nil {
				b.log.Warn("directory mirror update failed", "zone", z.ID, "err", err)
			}
		}
		b.publish("zone.changed", route)
	})
}
