package orchestrator

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/PageMastr/scifi-test/services/pkg/idgen"
)

// PGStore is the PostgreSQL Store (schema svc_orch, 05 §1.4, §3.2).
type PGStore struct {
	pool *pgxpool.Pool
}

// NewPGStore wraps a pool whose database has the svc_orch migrations applied.
func NewPGStore(pool *pgxpool.Pool) *PGStore { return &PGStore{pool: pool} }

// AcquireLeadership implements Store. The lease clock is PostgreSQL's, so replicas with skewed
// clocks agree on expiry.
func (s *PGStore) AcquireLeadership(ctx context.Context, shard, holder string, ttl time.Duration) (int64, bool, error) {
	var term int64
	err := s.pool.QueryRow(ctx, `INSERT INTO svc_orch.orch_leader AS l (shard, term, holder, expires_at)
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
	tag, err := s.pool.Exec(ctx, `UPDATE svc_orch.orch_leader SET expires_at = clock_timestamp() + $4 * interval '1 millisecond'
		WHERE shard = $1 AND holder = $2 AND term = $3`, shard, holder, term, ttl.Milliseconds())
	if err != nil {
		return false, err
	}
	return tag.RowsAffected() == 1, nil
}

// ReleaseLeadership implements Store.
func (s *PGStore) ReleaseLeadership(ctx context.Context, shard, holder string, term int64) error {
	_, err := s.pool.Exec(ctx, `UPDATE svc_orch.orch_leader SET expires_at = clock_timestamp() - interval '1 millisecond'
		WHERE shard = $1 AND holder = $2 AND term = $3`, shard, holder, term)
	return err
}

// fenced runs fn in a transaction that first share-locks the shard's leader row and checks the
// term (05 §1.4.1 term-fenced writes). The share lock makes a concurrent takeover wait for this
// transaction, so no write lands after a new term starts.
func (s *PGStore) fenced(ctx context.Context, f Fence, fn func(pgx.Tx) error) error {
	return pgx.BeginTxFunc(ctx, s.pool, pgx.TxOptions{}, func(tx pgx.Tx) error {
		var term int64
		err := tx.QueryRow(ctx, `SELECT term FROM svc_orch.orch_leader WHERE shard = $1 FOR SHARE`, f.Shard).Scan(&term)
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
			if _, err := tx.Exec(ctx, `INSERT INTO svc_orch.zone (zone_id, name, updated_at) VALUES ($1, $2, $3)
				ON CONFLICT (zone_id) DO UPDATE SET name = EXCLUDED.name`, z.ID, z.Name, now); err != nil {
				return err
			}
			if _, err := tx.Exec(ctx, `INSERT INTO svc_orch.region_lease (region_id, instance_id, lease_gen)
				VALUES ($1, $2, 0) ON CONFLICT (region_id) DO NOTHING`, WholeRegion(z.ID), z.ID); err != nil {
				return err
			}
		}
		return nil
	})
}

// ListZones implements Store.
func (s *PGStore) ListZones(ctx context.Context) ([]Zone, error) {
	rows, err := s.pool.Query(ctx, `SELECT z.zone_id, z.name, COALESCE(r.holder_proc, 0), COALESCE(r.lease_gen, 0)
		FROM svc_orch.zone z LEFT JOIN svc_orch.region_lease r ON r.region_id = z.zone_id ORDER BY z.zone_id`)
	if err != nil {
		return nil, err
	}
	return pgx.CollectRows(rows, func(row pgx.CollectableRow) (Zone, error) {
		var z Zone
		err := row.Scan(&z.ID, &z.Name, &z.Owner, &z.LeaseGen)
		return z, err
	})
}

// ListRegions implements Store.
func (s *PGStore) ListRegions(ctx context.Context) ([]Region, error) {
	rows, err := s.pool.Query(ctx, `SELECT region_id, instance_id, COALESCE(holder_proc, 0), lease_gen
		FROM svc_orch.region_lease ORDER BY region_id`)
	if err != nil {
		return nil, err
	}
	return pgx.CollectRows(rows, func(row pgx.CollectableRow) (Region, error) {
		var r Region
		err := row.Scan(&r.ID, &r.InstanceID, &r.Holder, &r.LeaseGen)
		return r, err
	})
}

// ResetOwners implements Store.
func (s *PGStore) ResetOwners(ctx context.Context, f Fence, now time.Time) error {
	return s.fenced(ctx, f, func(tx pgx.Tx) error {
		if _, err := tx.Exec(ctx, `INSERT INTO svc_orch.placement_log (at, zone_id, region_id, process_id, lease_gen, action)
			SELECT $1, instance_id, region_id, holder_proc, lease_gen, 'release' FROM svc_orch.region_lease
			WHERE holder_proc IS NOT NULL`, now); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `UPDATE svc_orch.region_lease SET holder_proc = NULL, assigned_at = NULL
			WHERE holder_proc IS NOT NULL`); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, `UPDATE svc_orch.process SET ended_at = $1, end_reason = 'leader_change' WHERE ended_at IS NULL`, now)
		return err
	})
}

// CreateProcess implements Store. The per-name epoch is allocated under an advisory lock on
// the name so two registrations of one name can never share an epoch.
func (s *PGStore) CreateProcess(ctx context.Context, f Fence, p ProcessInfo, now time.Time) (ProcessRecord, error) {
	var rec ProcessRecord
	err := s.fenced(ctx, f, func(tx pgx.Tx) error {
		if _, err := tx.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended('svc_orch.process:' || $1, 0))`, p.Name); err != nil {
			return err
		}
		host := p.FD.Host
		if host == "" {
			host = p.Host
		}
		return tx.QueryRow(ctx, `INSERT INTO svc_orch.process
			(process_id, name, kind, epoch, address, host, pid, version, registered_at, az, rack, server_build)
			SELECT nextval('svc_orch.process_id_seq'), $1, $2,
			       COALESCE((SELECT MAX(epoch) FROM svc_orch.process WHERE name = $1), 0) + 1, $3, $4, $5, $6, $7, $8, $9, $10
			RETURNING process_id, epoch`,
			p.Name, p.Kind, p.Address, host, p.PID, p.Version, now, p.FD.AZ, p.FD.Rack, p.ServerBuild).Scan(&rec.ID, &rec.Epoch)
	})
	return rec, err
}

// EndProcess implements Store.
func (s *PGStore) EndProcess(ctx context.Context, f Fence, id int64, reason string, now time.Time) error {
	return s.fenced(ctx, f, func(tx pgx.Tx) error {
		_, err := tx.Exec(ctx, `UPDATE svc_orch.process SET ended_at = $2, end_reason = $3 WHERE process_id = $1 AND ended_at IS NULL`,
			id, now, reason)
		return err
	})
}

// AssignRegion implements Store. The generation is bumped in the term-fenced transaction that
// names the holder, before anyone is told (05 §1.4.2).
func (s *PGStore) AssignRegion(ctx context.Context, f Fence, regionID, processID int64, now time.Time) (int64, error) {
	var gen, instance int64
	err := s.fenced(ctx, f, func(tx pgx.Tx) error {
		err := tx.QueryRow(ctx, `UPDATE svc_orch.region_lease SET holder_proc = $2, lease_gen = lease_gen + 1, assigned_at = $3
			WHERE region_id = $1 RETURNING lease_gen, instance_id`, regionID, processID, now).Scan(&gen, &instance)
		if errors.Is(err, pgx.ErrNoRows) {
			return ErrUnknownRegion
		}
		if err != nil {
			return err
		}
		_, err = tx.Exec(ctx, `INSERT INTO svc_orch.placement_log (at, zone_id, region_id, process_id, lease_gen, action)
			VALUES ($1, $2, $3, $4, $5, 'assign')`, now, instance, regionID, processID, gen)
		return err
	})
	return gen, err
}

// ReleaseRegion implements Store.
func (s *PGStore) ReleaseRegion(ctx context.Context, f Fence, regionID, processID int64, now time.Time) error {
	return s.fenced(ctx, f, func(tx pgx.Tx) error {
		var gen, instance int64
		err := tx.QueryRow(ctx, `UPDATE svc_orch.region_lease SET holder_proc = NULL, assigned_at = NULL
			WHERE region_id = $1 AND holder_proc = $2 RETURNING lease_gen, instance_id`, regionID, processID).Scan(&gen, &instance)
		if errors.Is(err, pgx.ErrNoRows) {
			return nil
		}
		if err != nil {
			return err
		}
		_, err = tx.Exec(ctx, `INSERT INTO svc_orch.placement_log (at, zone_id, region_id, process_id, lease_gen, action)
			VALUES ($1, $2, $3, $4, $5, 'release')`, now, instance, regionID, processID, gen)
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
		INSERT INTO svc_orch.id_alloc AS a (shard, last_ms)
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
