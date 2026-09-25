package orchestrator

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/PageMastr/scifi-test/services/pkg/idgen"
)

// PGStore is the PostgreSQL Store (schema "orchestrator").
type PGStore struct {
	pool *pgxpool.Pool
}

// NewPGStore wraps a pool whose database has the orchestrator migrations applied.
func NewPGStore(pool *pgxpool.Pool) *PGStore { return &PGStore{pool: pool} }

// AcquireLeadership implements Store. The lease clock is PostgreSQL's, so replicas with skewed
// clocks agree on expiry.
func (s *PGStore) AcquireLeadership(ctx context.Context, shard, holder string, ttl time.Duration) (int64, bool, error) {
	var term int64
	err := s.pool.QueryRow(ctx, `INSERT INTO orchestrator.orch_leader AS l (shard, term, holder, expires_at)
		VALUES ($1, 1, $2, clock_timestamp() + $3 * interval '1 millisecond')
		ON CONFLICT (shard) DO UPDATE SET term = l.term + 1, holder = EXCLUDED.holder, expires_at = EXCLUDED.expires_at
		WHERE l.expires_at < clock_timestamp() OR l.holder = EXCLUDED.holder
		RETURNING term`, shard, holder, ttl.Milliseconds()).Scan(&term)
	if errors.Is(err, pgx.ErrNoRows) {
		return 0, false, nil
	}
	if err != nil {
		return 0, false, err
	}
	return term, true, nil
}

// RenewLeadership implements Store.
func (s *PGStore) RenewLeadership(ctx context.Context, shard, holder string, term int64, ttl time.Duration) (bool, error) {
	tag, err := s.pool.Exec(ctx, `UPDATE orchestrator.orch_leader SET expires_at = clock_timestamp() + $4 * interval '1 millisecond'
		WHERE shard = $1 AND holder = $2 AND term = $3`, shard, holder, term, ttl.Milliseconds())
	if err != nil {
		return false, err
	}
	return tag.RowsAffected() == 1, nil
}

// ReleaseLeadership implements Store.
func (s *PGStore) ReleaseLeadership(ctx context.Context, shard, holder string, term int64) error {
	_, err := s.pool.Exec(ctx, `UPDATE orchestrator.orch_leader SET expires_at = clock_timestamp() - interval '1 millisecond'
		WHERE shard = $1 AND holder = $2 AND term = $3`, shard, holder, term)
	return err
}

// fenced runs fn in a transaction that first share-locks the shard's leader row and checks the
// term (05 §1.4.1 term-fenced writes). The share lock makes a concurrent takeover wait for this
// transaction, so no write lands after a new term starts.
func (s *PGStore) fenced(ctx context.Context, f Fence, fn func(pgx.Tx) error) error {
	return pgx.BeginTxFunc(ctx, s.pool, pgx.TxOptions{}, func(tx pgx.Tx) error {
		var term int64
		err := tx.QueryRow(ctx, `SELECT term FROM orchestrator.orch_leader WHERE shard = $1 FOR SHARE`, f.Shard).Scan(&term)
		if errors.Is(err, pgx.ErrNoRows) || (err == nil && term != f.Term) {
			return ErrNotLeader
		}
		if err != nil {
			return err
		}
		return fn(tx)
	})
}

// EnsureZones implements Store.
func (s *PGStore) EnsureZones(ctx context.Context, f Fence, zones []Zone, now time.Time) error {
	return s.fenced(ctx, f, func(tx pgx.Tx) error {
		for _, z := range zones {
			if _, err := tx.Exec(ctx, `INSERT INTO orchestrator.zone (zone_id, name, updated_at) VALUES ($1, $2, $3)
				ON CONFLICT (zone_id) DO UPDATE SET name = EXCLUDED.name`, z.ID, z.Name, now); err != nil {
				return err
			}
		}
		return nil
	})
}

// ListZones implements Store.
func (s *PGStore) ListZones(ctx context.Context) ([]Zone, error) {
	rows, err := s.pool.Query(ctx, `SELECT zone_id, name, COALESCE(owner_process, 0), lease_gen FROM orchestrator.zone ORDER BY zone_id`)
	if err != nil {
		return nil, err
	}
	return pgx.CollectRows(rows, func(row pgx.CollectableRow) (Zone, error) {
		var z Zone
		err := row.Scan(&z.ID, &z.Name, &z.Owner, &z.LeaseGen)
		return z, err
	})
}

// ResetOwners implements Store.
func (s *PGStore) ResetOwners(ctx context.Context, f Fence, now time.Time) error {
	return s.fenced(ctx, f, func(tx pgx.Tx) error {
		if _, err := tx.Exec(ctx, `INSERT INTO orchestrator.placement_log (at, zone_id, process_id, lease_gen, action)
			SELECT $1, zone_id, owner_process, lease_gen, 'release' FROM orchestrator.zone WHERE owner_process IS NOT NULL`, now); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE orchestrator.zone SET owner_process = NULL, updated_at = $1 WHERE owner_process IS NOT NULL`, now); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, `UPDATE orchestrator.process SET ended_at = $1, end_reason = 'leader_change' WHERE ended_at IS NULL`, now)
		return err
	})
}

// CreateProcess implements Store. The per-name epoch is allocated under an advisory lock on
// the name so two registrations of one name can never share an epoch.
func (s *PGStore) CreateProcess(ctx context.Context, f Fence, p ProcessInfo, now time.Time) (ProcessRecord, error) {
	var rec ProcessRecord
	err := s.fenced(ctx, f, func(tx pgx.Tx) error {
		if _, err := tx.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended('orchestrator.process:' || $1, 0))`, p.Name); err != nil {
			return err
		}
		return tx.QueryRow(ctx, `INSERT INTO orchestrator.process
			(process_id, name, kind, epoch, address, host, pid, version, registered_at)
			SELECT nextval('orchestrator.process_id_seq'), $1, $2,
			       COALESCE((SELECT MAX(epoch) FROM orchestrator.process WHERE name = $1), 0) + 1, $3, $4, $5, $6, $7
			RETURNING process_id, epoch`,
			p.Name, p.Kind, p.Address, p.Host, p.PID, p.Version, now).Scan(&rec.ID, &rec.Epoch)
	})
	return rec, err
}

// EndProcess implements Store.
func (s *PGStore) EndProcess(ctx context.Context, f Fence, id int64, reason string, now time.Time) error {
	return s.fenced(ctx, f, func(tx pgx.Tx) error {
		_, err := tx.Exec(ctx, `UPDATE orchestrator.process SET ended_at = $2, end_reason = $3 WHERE process_id = $1 AND ended_at IS NULL`,
			id, now, reason)
		return err
	})
}

// AssignZone implements Store.
func (s *PGStore) AssignZone(ctx context.Context, f Fence, zoneID, processID int64, now time.Time) (int64, error) {
	var gen int64
	err := s.fenced(ctx, f, func(tx pgx.Tx) error {
		err := tx.QueryRow(ctx, `UPDATE orchestrator.zone SET owner_process = $2, lease_gen = lease_gen + 1, updated_at = $3
			WHERE zone_id = $1 RETURNING lease_gen`, zoneID, processID, now).Scan(&gen)
		if errors.Is(err, pgx.ErrNoRows) {
			return ErrUnknownZone
		}
		if err != nil {
			return err
		}
		_, err = tx.Exec(ctx, `INSERT INTO orchestrator.placement_log (at, zone_id, process_id, lease_gen, action)
			VALUES ($1, $2, $3, $4, 'assign')`, now, zoneID, processID, gen)
		return err
	})
	return gen, err
}

// ReleaseZone implements Store.
func (s *PGStore) ReleaseZone(ctx context.Context, f Fence, zoneID, processID int64, now time.Time) error {
	return s.fenced(ctx, f, func(tx pgx.Tx) error {
		var gen int64
		err := tx.QueryRow(ctx, `UPDATE orchestrator.zone SET owner_process = NULL, updated_at = $3
			WHERE zone_id = $1 AND owner_process = $2 RETURNING lease_gen`, zoneID, processID, now).Scan(&gen)
		if errors.Is(err, pgx.ErrNoRows) {
			return nil
		}
		if err != nil {
			return err
		}
		_, err = tx.Exec(ctx, `INSERT INTO orchestrator.placement_log (at, zone_id, process_id, lease_gen, action)
			VALUES ($1, $2, $3, $4, 'release')`, now, zoneID, processID, gen)
		return err
	})
}

// AllocateIdBlocks implements Store with 05 §1.4.5's single-row statement; "now" is
// PostgreSQL's clock in ms since idgen.Epoch.
func (s *PGStore) AllocateIdBlocks(ctx context.Context, shardIndex, n int) ([]int64, error) {
	if err := checkBlockRequest(shardIndex, n); err != nil {
		return nil, err
	}
	var last int64
	err := s.pool.QueryRow(ctx, `WITH now_ms AS (
			SELECT floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint - $3::bigint AS v)
		INSERT INTO orchestrator.id_alloc AS a (shard, last_ms)
		SELECT $1, GREATEST($2::bigint - 1, v) FROM now_ms
		ON CONFLICT (shard) DO UPDATE SET last_ms = GREATEST(a.last_ms + $2::bigint, (SELECT v FROM now_ms))
		RETURNING last_ms`, shardIndex, n, idgen.Epoch.UnixMilli()).Scan(&last)
	if err != nil {
		return nil, err
	}
	out := make([]int64, n)
	for i := range out {
		out[i] = last - int64(n) + 1 + int64(i)
	}
	return out, nil
}

// Ping implements Store.
func (s *PGStore) Ping(ctx context.Context) error { return s.pool.Ping(ctx) }
