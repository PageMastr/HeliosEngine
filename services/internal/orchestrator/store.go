package orchestrator

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"sync"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/idgen"
)

// Zone is a placeable zone and the lease on its region. A v0 zone is its own single instance
// with one region ("Whole", 05 §1.4.2), whose ID is the zone ID (WholeRegion), so Owner and
// LeaseGen are that region's region_lease row.
type Zone struct {
	ID       int64
	Name     string
	Owner    int64 // holder of the zone's region (process ID), 0 = unassigned
	LeaseGen int64 // the region's lease generation: bumped on every assignment; holders fence with it
}

// WholeRegion returns the region ID of a v0 zone's single region (and its instance ID). Region
// IDs travel as the zone ID in assignments and held lists until multi-region zones (v1) add a
// region field to them.
func WholeRegion(zoneID int64) int64 { return zoneID }

// Region is one region_lease row (05 §1.4.2, §3.2): the authority on who holds a region under
// which generation.
type Region struct {
	ID         int64
	InstanceID int64 // v0: the zone ID
	Holder     int64 // process ID, 0 = unassigned
	LeaseGen   int64
}

// ProcessRecord is the persisted part of a registration.
type ProcessRecord struct {
	ID    int64
	Epoch int64
}

// Fence names the leadership term a mutation is made under (05 §1.4.1). Every mutating Store
// call checks it against the shard's orch_leader row inside the same transaction, so a deposed
// leader's writes touch nothing.
type Fence struct {
	Shard string
	Term  int64
}

// Store errors.
var (
	ErrUnknownRegion = errors.New("orchestrator: unknown region")
	// ErrNotLeader is returned by a mutation whose Fence term is no longer the shard's term.
	ErrNotLeader = errors.New("orchestrator: leadership term superseded")
)

// Store persists leadership, zones and their region leases, registrations and ID-block prefixes.
// Generations live in region_lease and are allocated here before any holder is told
// (05 §1.4.2). Safe for concurrent use.
type Store interface {
	// AcquireLeadership makes holder the shard's leader with a new term when the current lease
	// has expired, or when holder already owns the row (a restarted instance takes over its own
	// lease at once). ok is false when another holder's lease is still valid.
	AcquireLeadership(ctx context.Context, shard, holder string, ttl time.Duration) (term int64, ok bool, err error)
	// RenewLeadership extends the lease; false means the term was lost.
	RenewLeadership(ctx context.Context, shard, holder string, term int64, ttl time.Duration) (bool, error)
	// ReleaseLeadership expires the lease now so a successor need not wait (graceful stop).
	ReleaseLeadership(ctx context.Context, shard, holder string, term int64) error

	// EnsureZones inserts missing zones and one region_lease row for each zone's Whole region
	// (existing rows keep their generation).
	EnsureZones(ctx context.Context, f Fence, zones []Zone, now time.Time) error
	// ListZones returns the zones with their region's holder and generation, by zone ID.
	ListZones(ctx context.Context) ([]Zone, error)
	// ListRegions returns every region_lease row, by region ID.
	ListRegions(ctx context.Context) ([]Region, error)
	// ResetOwners clears every region's holder (generations stay) and ends every open
	// registration (new leader: the in-memory leases are gone, so live processes must
	// re-register).
	ResetOwners(ctx context.Context, f Fence, now time.Time) error
	// CreateProcess records a registration and returns its ID and per-name epoch.
	CreateProcess(ctx context.Context, f Fence, p ProcessInfo, now time.Time) (ProcessRecord, error)
	EndProcess(ctx context.Context, f Fence, id int64, reason string, now time.Time) error
	// AssignRegion makes processID the region's holder and returns its new lease generation
	// (ErrUnknownRegion if there is no such region_lease row).
	AssignRegion(ctx context.Context, f Fence, regionID, processID int64, now time.Time) (int64, error)
	// ReleaseRegion clears the holder if it is still processID; the generation stays.
	ReleaseRegion(ctx context.Context, f Fence, regionID, processID int64, now time.Time) error

	// AllocateIdBlocks advances the shard's id_alloc row and returns n new block prefixes
	// (05 §1.4.5). It needs no fence: the row itself guarantees prefixes never repeat.
	AllocateIdBlocks(ctx context.Context, shardIndex, n int) ([]int64, error)

	Ping(ctx context.Context) error
}

// MemStore is an in-memory Store for tests. It follows the PostgreSQL store's semantics (the
// storetest suite runs against both); its clock drives lease expiry and block prefixes.
type MemStore struct {
	mu      sync.Mutex
	clk     clock.Clock
	zones   map[int64]*Zone // ID and Name only; the lease is in regions
	regions map[int64]*Region
	epochs  map[string]int64
	nextID  int64
	ended   map[int64]string
	leaders map[string]*memLeader
	idLast  map[int]int64
}

type memLeader struct {
	term    int64
	holder  string
	expires time.Time
}

// NewMemStore returns an empty store on the system clock.
func NewMemStore() *MemStore { return NewMemStoreWithClock(nil) }

// NewMemStoreWithClock returns an empty store on clk.
func NewMemStoreWithClock(clk clock.Clock) *MemStore {
	if clk == nil {
		clk = clock.System{}
	}
	return &MemStore{clk: clk, zones: map[int64]*Zone{}, regions: map[int64]*Region{}, epochs: map[string]int64{},
		ended: map[int64]string{}, leaders: map[string]*memLeader{}, idLast: map[int]int64{}}
}

// AcquireLeadership implements Store.
func (m *MemStore) AcquireLeadership(_ context.Context, shard, holder string, ttl time.Duration) (int64, bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	now := m.clk.Now()
	l, ok := m.leaders[shard]
	if !ok {
		m.leaders[shard] = &memLeader{term: 1, holder: holder, expires: now.Add(ttl)}
		return 1, true, nil
	}
	if l.expires.After(now) && l.holder != holder {
		return 0, false, nil
	}
	l.term++
	l.holder, l.expires = holder, now.Add(ttl)
	return l.term, true, nil
}

// RenewLeadership implements Store.
func (m *MemStore) RenewLeadership(_ context.Context, shard, holder string, term int64, ttl time.Duration) (bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	l, ok := m.leaders[shard]
	if !ok || l.term != term || l.holder != holder {
		return false, nil
	}
	l.expires = m.clk.Now().Add(ttl)
	return true, nil
}

// ReleaseLeadership implements Store.
func (m *MemStore) ReleaseLeadership(_ context.Context, shard, holder string, term int64) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if l, ok := m.leaders[shard]; ok && l.term == term && l.holder == holder {
		l.expires = m.clk.Now().Add(-time.Millisecond)
	}
	return nil
}

func (m *MemStore) checkLocked(f Fence) error {
	if l, ok := m.leaders[f.Shard]; !ok || l.term != f.Term {
		return ErrNotLeader
	}
	return nil
}

// EnsureZones implements Store.
func (m *MemStore) EnsureZones(_ context.Context, f Fence, zones []Zone, _ time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if err := m.checkLocked(f); err != nil {
		return err
	}
	for _, z := range zones {
		if _, ok := m.zones[z.ID]; !ok {
			m.zones[z.ID] = &Zone{ID: z.ID, Name: z.Name}
		} else {
			m.zones[z.ID].Name = z.Name
		}
		if r := WholeRegion(z.ID); m.regions[r] == nil {
			m.regions[r] = &Region{ID: r, InstanceID: z.ID}
		}
	}
	return nil
}

// ListZones implements Store.
func (m *MemStore) ListZones(context.Context) ([]Zone, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]Zone, 0, len(m.zones))
	for _, z := range m.zones {
		c := Zone{ID: z.ID, Name: z.Name}
		if r := m.regions[WholeRegion(z.ID)]; r != nil {
			c.Owner, c.LeaseGen = r.Holder, r.LeaseGen
		}
		out = append(out, c)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

// ListRegions implements Store.
func (m *MemStore) ListRegions(context.Context) ([]Region, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]Region, 0, len(m.regions))
	for _, r := range m.regions {
		out = append(out, *r)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

// ResetOwners implements Store.
func (m *MemStore) ResetOwners(_ context.Context, f Fence, _ time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if err := m.checkLocked(f); err != nil {
		return err
	}
	for _, r := range m.regions {
		r.Holder = 0
	}
	return nil
}

// CreateProcess implements Store.
func (m *MemStore) CreateProcess(_ context.Context, f Fence, p ProcessInfo, _ time.Time) (ProcessRecord, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if err := m.checkLocked(f); err != nil {
		return ProcessRecord{}, err
	}
	m.nextID++
	m.epochs[p.Name]++
	return ProcessRecord{ID: m.nextID, Epoch: m.epochs[p.Name]}, nil
}

// EndProcess implements Store.
func (m *MemStore) EndProcess(_ context.Context, f Fence, id int64, reason string, _ time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if err := m.checkLocked(f); err != nil {
		return err
	}
	m.ended[id] = reason
	return nil
}

// EndReason returns the recorded end reason of a process (tests).
func (m *MemStore) EndReason(id int64) string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.ended[id]
}

// AssignRegion implements Store.
func (m *MemStore) AssignRegion(_ context.Context, f Fence, regionID, processID int64, _ time.Time) (int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if err := m.checkLocked(f); err != nil {
		return 0, err
	}
	r, ok := m.regions[regionID]
	if !ok {
		return 0, ErrUnknownRegion
	}
	r.Holder = processID
	r.LeaseGen++
	return r.LeaseGen, nil
}

// ReleaseRegion implements Store.
func (m *MemStore) ReleaseRegion(_ context.Context, f Fence, regionID, processID int64, _ time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if err := m.checkLocked(f); err != nil {
		return err
	}
	if r, ok := m.regions[regionID]; ok && r.Holder == processID {
		r.Holder = 0
	}
	return nil
}

// AllocateIdBlocks implements Store.
func (m *MemStore) AllocateIdBlocks(_ context.Context, shardIndex, n int) ([]int64, error) {
	if err := checkBlockRequest(shardIndex, n); err != nil {
		return nil, err
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	last, ok := m.idLast[shardIndex]
	if !ok {
		last = -1
	}
	newLast, out := idgen.Allocate(last, n, idgen.PrefixAt(m.clk.Now()))
	if newLast > idgen.MaxPrefix {
		return nil, idgen.ErrPrefixOverflow
	}
	m.idLast[shardIndex] = newLast
	return out, nil
}

func checkBlockRequest(shardIndex, n int) error {
	if shardIndex < 0 || shardIndex > idgen.MaxShard {
		return fmt.Errorf("orchestrator: shard index %d out of range", shardIndex)
	}
	if n < 1 || n > idgen.MaxBlocksPerCall {
		return fmt.Errorf("orchestrator: block count %d outside 1..%d", n, idgen.MaxBlocksPerCall)
	}
	return nil
}

// Ping implements Store.
func (m *MemStore) Ping(context.Context) error { return nil }
