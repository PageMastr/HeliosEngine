// Package idgen mints Helios IDs from time-prefixed blocks (05 §1.4.5, §3.2):
//
//	bit 63     0 (IDs are positive int64; 02 §4.1's runtime-spawn range)
//	bits 62–22 41-bit block prefix: milliseconds since Epoch (2026-01-01T00:00:00Z), good until 2095
//	bits 21–17 5-bit shard
//	bits 16–0  17-bit offset inside the block (BlockSize = 131,072 IDs)
//
// Prefixes come from AllocateIdBlocks, which advances one PostgreSQL row per shard
// (`last_ms = GREATEST(last_ms + n, now_ms)`), so prefixes strictly increase whichever process or
// replica asks, and no block is ever handed out twice. Minting is then a local increment: there are
// no node IDs to lease and nothing to coordinate. A Minter holds its current block plus spares,
// refills in the background once the current block is half used, retires blocks one hour after
// allocation (keeping IDs within an hour of wall-clock order for `tx_id` range partitions) and, if
// the control plane is unreachable, keeps minting from retired blocks rather than stall.
//
// The C++ cell minter implements the same layout; testdata/vectors/block_ids.json holds the golden
// vectors both sides check.
package idgen

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
)

// Epoch is the zero point of block prefixes.
var Epoch = time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)

// Field widths and limits.
const (
	PrefixBits = 41
	ShardBits  = 5
	OffsetBits = 17

	BlockSize = 1 << OffsetBits // IDs per block
	MaxShard  = 1<<ShardBits - 1
	MaxPrefix = 1<<PrefixBits - 1
	// MaxBlocksPerCall bounds one AllocateIdBlocks call (05 §1.4.5: n ≤ 16).
	MaxBlocksPerCall = 16

	shardShift  = OffsetBits
	prefixShift = OffsetBits + ShardBits
)

// Compose builds an ID from its fields (golden vectors, tooling). Out-of-range fields are
// truncated to their width.
func Compose(prefix int64, shard, offset int) int64 {
	return (prefix&MaxPrefix)<<prefixShift | int64(shard&MaxShard)<<shardShift | int64(offset&(BlockSize-1))
}

// Parts is a decoded ID.
type Parts struct {
	Prefix int64     // block prefix (ms since Epoch at allocation)
	Time   time.Time // Epoch + Prefix ms: when the block was allocated, within ≤ 1 h of minting
	Shard  int
	Offset int
}

// Decode splits an ID into its fields.
func Decode(id int64) Parts {
	prefix := id >> prefixShift & MaxPrefix
	return Parts{
		Prefix: prefix,
		Time:   Epoch.Add(time.Duration(prefix) * time.Millisecond),
		Shard:  int(id>>shardShift) & MaxShard,
		Offset: int(id) & (BlockSize - 1),
	}
}

// PrefixAt returns the prefix for wall-clock time t (ms since Epoch, clamped at 0).
func PrefixAt(t time.Time) int64 {
	ms := t.Sub(Epoch).Milliseconds()
	if ms < 0 {
		return 0
	}
	return ms
}

// Allocate is the allocation rule every Source implements (the SQL in the orchestrator store
// runs the same arithmetic): the row advances to max(last+n, now) and the n prefixes ending at
// the new value are handed out. They are all greater than last, so no prefix repeats even when
// the clock steps back.
func Allocate(last int64, n int, nowPrefix int64) (newLast int64, prefixes []int64) {
	newLast = max(last+int64(n), nowPrefix)
	prefixes = make([]int64, n)
	for i := range prefixes {
		prefixes[i] = newLast - int64(n) + 1 + int64(i)
	}
	return newLast, prefixes
}

// Source hands out block prefixes for one shard (AllocateIdBlocks). Implementations must be
// safe for concurrent use and return strictly increasing prefixes across calls.
type Source interface {
	AllocateIdBlocks(ctx context.Context, n int) ([]int64, error)
}

// SourceFunc adapts a function to Source.
type SourceFunc func(ctx context.Context, n int) ([]int64, error)

// AllocateIdBlocks implements Source.
func (f SourceFunc) AllocateIdBlocks(ctx context.Context, n int) ([]int64, error) { return f(ctx, n) }

// MemSource is an in-memory Source with the same rule as the PostgreSQL row (tests, tools).
// Safe for concurrent use.
type MemSource struct {
	mu   sync.Mutex
	clk  clock.Clock
	last int64
	// Calls counts AllocateIdBlocks calls (tests).
	Calls int
	// Fail, if set, makes AllocateIdBlocks return it (tests of the degraded path).
	Fail error
}

// NewMemSource returns a source whose "now" comes from clk (the system clock when nil).
func NewMemSource(clk clock.Clock) *MemSource {
	if clk == nil {
		clk = clock.System{}
	}
	return &MemSource{clk: clk, last: -1}
}

// AllocateIdBlocks implements Source.
func (s *MemSource) AllocateIdBlocks(_ context.Context, n int) ([]int64, error) {
	if n < 1 || n > MaxBlocksPerCall {
		return nil, fmt.Errorf("idgen: n=%d outside 1..%d", n, MaxBlocksPerCall)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.Calls++
	if s.Fail != nil {
		return nil, s.Fail
	}
	var out []int64
	s.last, out = Allocate(s.last, n, PrefixAt(s.clk.Now()))
	if s.last > MaxPrefix {
		return nil, ErrPrefixOverflow
	}
	return out, nil
}

// Errors.
var (
	ErrPrefixOverflow = errors.New("idgen: block prefix exceeds 41 bits")
	ErrNoBlock        = errors.New("idgen: no ID block available")
	ErrClosed         = errors.New("idgen: minter closed")
)

// Options tune a Minter.
type Options struct {
	// Hold is how many blocks the minter keeps (current + spares); 2 by default, 4 for
	// battle-profile processes (05 §1.4.5).
	Hold int
	// Retire is the block lifetime after allocation (1 h by default).
	Retire time.Duration
	// Timeout bounds one AllocateIdBlocks call (10 s by default).
	Timeout time.Duration
	Clock   clock.Clock
	Log     *slog.Logger
}

type block struct {
	prefix    int64
	next      int
	allocated time.Time
}

// Minter mints IDs for one shard from blocks. IDs from one Minter strictly increase. Safe for
// concurrent use.
type Minter struct {
	shard   int
	src     Source
	clk     clock.Clock
	log     *slog.Logger
	hold    int
	retire  time.Duration
	timeout time.Duration

	mu        sync.Mutex
	blocks    []block
	refilling chan struct{} // non-nil while a refill runs; closed when it ends
	lastErr   error
	retryAt   time.Time
	closed    bool
	wg        sync.WaitGroup
}

// NewMinter returns a minter for shard (0..31) drawing blocks from src. It holds no block until
// the first Next (or Prime) call.
func NewMinter(shard int, src Source, o Options) (*Minter, error) {
	if shard < 0 || shard > MaxShard {
		return nil, fmt.Errorf("idgen: shard %d out of range 0..%d", shard, MaxShard)
	}
	if src == nil {
		return nil, errors.New("idgen: nil block source")
	}
	if o.Hold < 1 {
		o.Hold = 2
	}
	if o.Hold > MaxBlocksPerCall {
		o.Hold = MaxBlocksPerCall
	}
	if o.Retire <= 0 {
		o.Retire = time.Hour
	}
	if o.Timeout <= 0 {
		o.Timeout = 10 * time.Second
	}
	if o.Clock == nil {
		o.Clock = clock.System{}
	}
	if o.Log == nil {
		o.Log = slog.Default()
	}
	return &Minter{shard: shard, src: src, clk: o.Clock, log: o.Log, hold: o.Hold, retire: o.Retire, timeout: o.Timeout}, nil
}

// Prime allocates the initial blocks synchronously, so a start-up failure surfaces at start
// rather than on the first request.
func (m *Minter) Prime(ctx context.Context) error {
	prefixes, err := m.src.AllocateIdBlocks(ctx, m.hold)
	if err != nil {
		return fmt.Errorf("idgen: allocate initial blocks: %w", err)
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.addLocked(prefixes, m.clk.Now())
}

// Next returns a new ID. It only waits for the block source when the minter holds no usable
// block at all (before the first allocation, or after exhausting every block while refills
// failed).
func (m *Minter) Next() (int64, error) {
	m.mu.Lock()
	for attempt := 0; ; attempt++ {
		if m.closed {
			m.mu.Unlock()
			return 0, ErrClosed
		}
		now := m.clk.Now()
		m.pruneLocked(now)
		if len(m.blocks) > 0 {
			b := &m.blocks[0]
			id := Compose(b.prefix, m.shard, b.next)
			b.next++
			m.maybeRefillLocked(now)
			m.mu.Unlock()
			return id, nil
		}
		if attempt >= 2 {
			err := m.lastErr
			m.mu.Unlock()
			if err == nil {
				return 0, ErrNoBlock
			}
			return 0, fmt.Errorf("%w: %w", ErrNoBlock, err)
		}
		done := m.startRefillLocked(m.hold, now, true)
		m.mu.Unlock()
		t := time.NewTimer(m.timeout)
		select {
		case <-done:
		case <-t.C:
		}
		t.Stop()
		m.mu.Lock()
	}
}

// Remaining reports how many IDs the held blocks still have (05 §8 IdMinter.Reserve).
func (m *Minter) Remaining() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	n := 0
	for _, b := range m.blocks {
		n += BlockSize - b.next
	}
	return n
}

// Close stops refills and waits for one in flight.
func (m *Minter) Close() {
	m.mu.Lock()
	m.closed = true
	m.mu.Unlock()
	m.wg.Wait()
}

// pruneLocked drops spent blocks, and retired blocks when a fresher one is held (a retired
// block is still used when nothing else is: minters never stall on a control-plane outage).
func (m *Minter) pruneLocked(now time.Time) {
	i := 0
	for i < len(m.blocks) && m.blocks[i].next >= BlockSize {
		i++
	}
	m.blocks = m.blocks[i:]
	firstFresh := -1
	for j, b := range m.blocks {
		if now.Sub(b.allocated) < m.retire {
			firstFresh = j
			break
		}
	}
	if firstFresh > 0 {
		m.blocks = m.blocks[firstFresh:]
	}
}

// maybeRefillLocked asks for more blocks when fewer than Hold fresh blocks remain, counting the
// current block as gone once it is half used; so a refill has half a block of runway.
func (m *Minter) maybeRefillLocked(now time.Time) {
	fresh := 0
	for j, b := range m.blocks {
		if now.Sub(b.allocated) >= m.retire || (j == 0 && b.next >= BlockSize/2) {
			continue
		}
		fresh++
	}
	if need := m.hold - fresh; need > 0 {
		m.startRefillLocked(need, now, false)
	}
}

// startRefillLocked starts one background AllocateIdBlocks call (at most one runs at a time)
// and returns a channel closed when it ends; nil when a failed refill is still backing off.
func (m *Minter) startRefillLocked(n int, now time.Time, force bool) <-chan struct{} {
	if m.refilling != nil {
		return m.refilling
	}
	if m.closed || (!force && now.Before(m.retryAt)) {
		return nil
	}
	n = min(max(n, 1), MaxBlocksPerCall)
	done := make(chan struct{})
	m.refilling = done
	m.wg.Add(1)
	go func() {
		defer m.wg.Done()
		ctx, cancel := context.WithTimeout(context.Background(), m.timeout)
		prefixes, err := m.src.AllocateIdBlocks(ctx, n)
		cancel()
		m.mu.Lock()
		now := m.clk.Now()
		if err == nil {
			err = m.addLocked(prefixes, now)
		}
		if err != nil {
			m.lastErr, m.retryAt = err, now.Add(time.Second)
			m.log.Warn("ID block refill failed; minting from held blocks", "err", err, "remaining_blocks", len(m.blocks))
		} else {
			m.lastErr = nil
		}
		m.refilling = nil
		close(done)
		m.mu.Unlock()
	}()
	return done
}

// addLocked appends freshly allocated prefixes after validating them: in range and above every
// prefix already held, so IDs keep increasing.
func (m *Minter) addLocked(prefixes []int64, now time.Time) error {
	if len(prefixes) == 0 {
		return errors.New("idgen: block source returned no prefixes")
	}
	last := int64(-1)
	if len(m.blocks) > 0 {
		last = m.blocks[len(m.blocks)-1].prefix
	}
	for _, p := range prefixes {
		if p < 0 || p > MaxPrefix {
			return fmt.Errorf("%w: %d", ErrPrefixOverflow, p)
		}
		if p <= last {
			return fmt.Errorf("idgen: block source returned non-increasing prefix %d after %d", p, last)
		}
		last = p
	}
	for _, p := range prefixes {
		m.blocks = append(m.blocks, block{prefix: p, allocated: now})
	}
	return nil
}
