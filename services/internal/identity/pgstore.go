package identity

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// PGStore is the PostgreSQL Store (schema svc_identity, 05 §1.1, §3.2).
type PGStore struct {
	pool *pgxpool.Pool
}

// NewPGStore wraps a pool whose database has the identity migrations applied.
func NewPGStore(pool *pgxpool.Pool) *PGStore { return &PGStore{pool: pool} }

const accountColumns = `account_id, email_ct, email_bidx, handle, handle_norm, discriminator, password_hash, is_bot,
	banned_until, COALESCE(ban_reason, ''), created_at, updated_at, last_login_at`

// Unique constraints CreateAccount maps to errors.
const (
	constraintTag   = "account_tag_unique"
	constraintEmail = "account_email_bidx_unique"
)

func scanAccount(row pgx.Row) (*Account, error) {
	var a Account
	err := row.Scan(&a.ID, &a.EmailCT, &a.EmailBidx, &a.Handle, &a.HandleNorm, &a.Discriminator, &a.PasswordHash, &a.IsBot,
		&a.BannedUntil, &a.BanReason, &a.CreatedAt, &a.UpdatedAt, &a.LastLoginAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &a, nil
}

// inTx runs fn in a READ COMMITTED transaction, retrying serialization failures and deadlocks
// (05 §8 Store.InTx contract).
func (s *PGStore) inTx(ctx context.Context, fn func(pgx.Tx) error) error {
	var err error
	for attempt := 0; attempt < 5; attempt++ {
		err = pgx.BeginTxFunc(ctx, s.pool, pgx.TxOptions{IsoLevel: pgx.ReadCommitted}, fn)
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && (pgErr.Code == "40001" || pgErr.Code == "40P01") {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(time.Duration(attempt+1) * 5 * time.Millisecond):
			}
			continue
		}
		return err
	}
	return err
}

func appendAuditTx(ctx context.Context, tx pgx.Tx, e *AuditEntry) error {
	if e == nil {
		return nil
	}
	var seq int64
	var head []byte
	if err := tx.QueryRow(ctx, `SELECT seq, hash FROM svc_identity.audit_head WHERE id = 1 FOR UPDATE`).Scan(&seq, &head); err != nil {
		return err
	}
	e.Seq = seq + 1
	copy(e.PrevHash[:], head)
	e.Hash = ChainHash(e.PrevHash, e)
	if _, err := tx.Exec(ctx, `INSERT INTO svc_identity.audit_log
		(seq, at, actor_account, subject_account, action, client_ip, detail, prev_hash, hash)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)`,
		e.Seq, e.At, e.Actor, e.Subject, e.Action, e.ClientIP, e.Detail, e.PrevHash[:], e.Hash[:]); err != nil {
		return err
	}
	_, err := tx.Exec(ctx, `UPDATE svc_identity.audit_head SET seq = $1, hash = $2 WHERE id = 1`, e.Seq, e.Hash[:])
	return err
}

// CreateAccount implements Store.
func (s *PGStore) CreateAccount(ctx context.Context, a *Account, key *SubjectKey, audit *AuditEntry) error {
	if key == nil || key.AccountID != a.ID {
		return errors.New("identity: an account needs its own subject key")
	}
	return s.inTx(ctx, func(tx pgx.Tx) error {
		_, err := tx.Exec(ctx, `INSERT INTO svc_identity.account
			(account_id, email_ct, email_bidx, handle, handle_norm, discriminator, password_hash, is_bot, created_at, updated_at)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)`,
			a.ID, a.EmailCT, a.EmailBidx, a.Handle, a.HandleNorm, a.Discriminator, a.PasswordHash, a.IsBot, a.CreatedAt, a.UpdatedAt)
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == "23505" {
			switch pgErr.ConstraintName {
			case constraintTag:
				return ErrTagTaken
			case constraintEmail:
				return ErrEmailTaken
			}
		}
		if err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `INSERT INTO svc_identity.subject_key (account_id, wrapped_dek, kek_version)
			VALUES ($1, $2, $3)`, key.AccountID, key.WrappedDEK, key.KEKVersion); err != nil {
			return err
		}
		return appendAuditTx(ctx, tx, audit)
	})
}

// AccountByID implements Store.
func (s *PGStore) AccountByID(ctx context.Context, id int64) (*Account, error) {
	return scanAccount(s.pool.QueryRow(ctx, `SELECT `+accountColumns+` FROM svc_identity.account WHERE account_id = $1`, id))
}

// AccountByEmailIndex implements Store.
func (s *PGStore) AccountByEmailIndex(ctx context.Context, bidx []byte) (*Account, error) {
	if len(bidx) == 0 {
		return nil, ErrNotFound
	}
	return scanAccount(s.pool.QueryRow(ctx, `SELECT `+accountColumns+` FROM svc_identity.account WHERE email_bidx = $1`, bidx))
}

// SubjectKey implements Store.
func (s *PGStore) SubjectKey(ctx context.Context, accountID int64) (*SubjectKey, error) {
	k := SubjectKey{AccountID: accountID}
	err := s.pool.QueryRow(ctx, `SELECT wrapped_dek, kek_version FROM svc_identity.subject_key WHERE account_id = $1`,
		accountID).Scan(&k.WrappedDEK, &k.KEKVersion)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &k, nil
}

// AccountByTag implements Store.
func (s *PGStore) AccountByTag(ctx context.Context, handleNorm string, d int16) (*Account, error) {
	return scanAccount(s.pool.QueryRow(ctx, `SELECT `+accountColumns+` FROM svc_identity.account
		WHERE handle_norm = $1 AND discriminator = $2`, handleNorm, d))
}

func expectOne(tag pgconn.CommandTag, err error) error {
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrNotFound
	}
	return nil
}

// SetPasswordHash implements Store.
func (s *PGStore) SetPasswordHash(ctx context.Context, id int64, hash string, now time.Time) error {
	return expectOne(s.pool.Exec(ctx, `UPDATE svc_identity.account SET password_hash = $2, updated_at = $3 WHERE account_id = $1`, id, hash, now))
}

// RecordLogin implements Store.
func (s *PGStore) RecordLogin(ctx context.Context, id int64, now time.Time, audit *AuditEntry) error {
	return s.inTx(ctx, func(tx pgx.Tx) error {
		if err := expectOne(tx.Exec(ctx, `UPDATE svc_identity.account SET last_login_at = $2 WHERE account_id = $1`, id, now)); err != nil {
			return err
		}
		return appendAuditTx(ctx, tx, audit)
	})
}

// SetBan implements Store.
func (s *PGStore) SetBan(ctx context.Context, id int64, until *time.Time, reason string, now time.Time, audit *AuditEntry) error {
	return s.inTx(ctx, func(tx pgx.Tx) error {
		var r *string
		if until != nil {
			r = &reason
		}
		if err := expectOne(tx.Exec(ctx, `UPDATE svc_identity.account SET banned_until = $2, ban_reason = $3, updated_at = $4
			WHERE account_id = $1`, id, until, r, now)); err != nil {
			return err
		}
		if until != nil {
			if _, err := tx.Exec(ctx, `UPDATE svc_identity.refresh_token SET revoked_at = $2
				WHERE account_id = $1 AND revoked_at IS NULL`, id, now); err != nil {
				return err
			}
		}
		return appendAuditTx(ctx, tx, audit)
	})
}

// InsertRefreshToken implements Store.
func (s *PGStore) InsertRefreshToken(ctx context.Context, t *RefreshToken) error {
	_, err := s.pool.Exec(ctx, `INSERT INTO svc_identity.refresh_token
		(token_hash, family_id, account_id, issued_at, expires_at, client_ip) VALUES ($1, $2, $3, $4, $5, $6)`,
		t.Hash, t.FamilyID, t.AccountID, t.IssuedAt, t.ExpiresAt, t.ClientIP)
	return err
}

// lockFamilyOf serializes every change to the family of the token with hash (rotation,
// revocation, launch-code branches) with a transaction-scoped advisory lock, taken before any
// row lock so concurrent callers cannot deadlock. Without it, a logout's UPDATE could miss a
// successor row that a concurrent rotation inserted after the UPDATE's snapshot was taken.
func lockFamilyOf(ctx context.Context, tx pgx.Tx, hash []byte) (int64, error) {
	var family int64
	err := tx.QueryRow(ctx, `SELECT family_id FROM svc_identity.refresh_token WHERE token_hash = $1`, hash).Scan(&family)
	if errors.Is(err, pgx.ErrNoRows) {
		return 0, ErrTokenInvalid
	}
	if err != nil {
		return 0, err
	}
	return family, lockFamily(ctx, tx, family)
}

func lockFamily(ctx context.Context, tx pgx.Tx, family int64) error {
	_, err := tx.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended('svc_identity.refresh_family:' || $1::bigint::text, 0))`, family)
	return err
}

// ActiveFamily implements Store.
func (s *PGStore) ActiveFamily(ctx context.Context, familyID int64, now time.Time) (int64, error) {
	var acct int64
	err := s.pool.QueryRow(ctx, `SELECT account_id FROM svc_identity.refresh_token
		WHERE family_id = $1 AND revoked_at IS NULL AND used_at IS NULL AND expires_at > $2 LIMIT 1`, familyID, now).Scan(&acct)
	if errors.Is(err, pgx.ErrNoRows) {
		return 0, ErrTokenInvalid
	}
	return acct, err
}

// ExtendFamily implements Store. The family lock orders it against revocation and rotation;
// the account row is share-locked so a concurrent ban (which updates that row, then revokes
// the account's tokens) either sees the new row or is seen here.
func (s *PGStore) ExtendFamily(ctx context.Context, t *RefreshToken, now time.Time) error {
	return s.inTx(ctx, func(tx pgx.Tx) error {
		if err := lockFamily(ctx, tx, t.FamilyID); err != nil {
			return err
		}
		var acct int64
		err := tx.QueryRow(ctx, `SELECT account_id FROM svc_identity.refresh_token
			WHERE family_id = $1 AND revoked_at IS NULL AND used_at IS NULL AND expires_at > $2 LIMIT 1`, t.FamilyID, now).Scan(&acct)
		if errors.Is(err, pgx.ErrNoRows) || (err == nil && acct != t.AccountID) {
			return ErrTokenInvalid
		}
		if err != nil {
			return err
		}
		var banned *time.Time
		if err := tx.QueryRow(ctx, `SELECT banned_until FROM svc_identity.account WHERE account_id = $1 FOR SHARE`, acct).Scan(&banned); err != nil {
			return err
		}
		if banned != nil && banned.After(now) {
			return ErrTokenInvalid
		}
		_, err = tx.Exec(ctx, `INSERT INTO svc_identity.refresh_token
			(token_hash, family_id, account_id, issued_at, expires_at, client_ip) VALUES ($1, $2, $3, $4, $5, $6)`,
			t.Hash, t.FamilyID, t.AccountID, t.IssuedAt, t.ExpiresAt, t.ClientIP)
		return err
	})
}

func lockToken(ctx context.Context, tx pgx.Tx, hash []byte) (*RefreshToken, error) {
	var t RefreshToken
	err := tx.QueryRow(ctx, `SELECT token_hash, family_id, account_id, issued_at, expires_at, used_at, revoked_at,
		COALESCE(client_ip, '') FROM svc_identity.refresh_token WHERE token_hash = $1 FOR UPDATE`, hash).
		Scan(&t.Hash, &t.FamilyID, &t.AccountID, &t.IssuedAt, &t.ExpiresAt, &t.UsedAt, &t.RevokedAt, &t.ClientIP)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrTokenInvalid
	}
	return &t, err
}

func revokeFamilyTx(ctx context.Context, tx pgx.Tx, family int64, now time.Time) error {
	_, err := tx.Exec(ctx, `UPDATE svc_identity.refresh_token SET revoked_at = $2 WHERE family_id = $1 AND revoked_at IS NULL`, family, now)
	return err
}

// RotateRefreshToken implements Store.
func (s *PGStore) RotateRefreshToken(ctx context.Context, oldHash []byte, next *RefreshToken, now time.Time, reuseAudit *AuditEntry) (*RefreshToken, error) {
	// Reuse must still commit (the family revocation and its audit row are the point of
	// detecting it), so it is reported after the transaction rather than as its error.
	var old *RefreshToken
	reused := false
	err := s.inTx(ctx, func(tx pgx.Tx) error {
		reused = false
		if _, err := lockFamilyOf(ctx, tx, oldHash); err != nil {
			return err
		}
		var err error
		old, err = lockToken(ctx, tx, oldHash)
		if err != nil {
			return err
		}
		if old.RevokedAt != nil {
			return ErrTokenInvalid
		}
		if old.UsedAt != nil {
			if err := revokeFamilyTx(ctx, tx, old.FamilyID, now); err != nil {
				return err
			}
			if reuseAudit != nil {
				reuseAudit.Subject = old.AccountID
			}
			reused = true
			return appendAuditTx(ctx, tx, reuseAudit)
		}
		if !old.ExpiresAt.After(now) {
			return ErrTokenInvalid
		}
		if _, err := tx.Exec(ctx, `UPDATE svc_identity.refresh_token SET used_at = $2 WHERE token_hash = $1`, oldHash, now); err != nil {
			return err
		}
		next.FamilyID, next.AccountID = old.FamilyID, old.AccountID
		_, err = tx.Exec(ctx, `INSERT INTO svc_identity.refresh_token
			(token_hash, family_id, account_id, issued_at, expires_at, client_ip) VALUES ($1, $2, $3, $4, $5, $6)`,
			next.Hash, next.FamilyID, next.AccountID, next.IssuedAt, next.ExpiresAt, next.ClientIP)
		return err
	})
	if err != nil {
		return nil, err
	}
	if reused {
		return nil, ErrTokenReused
	}
	return old, nil
}

// RevokeFamilyOf implements Store.
func (s *PGStore) RevokeFamilyOf(ctx context.Context, hash []byte, now time.Time, audit *AuditEntry) (*RefreshToken, error) {
	var t *RefreshToken
	err := s.inTx(ctx, func(tx pgx.Tx) error {
		if _, err := lockFamilyOf(ctx, tx, hash); err != nil {
			return err
		}
		var err error
		if t, err = lockToken(ctx, tx, hash); err != nil {
			return err
		}
		if err := revokeFamilyTx(ctx, tx, t.FamilyID, now); err != nil {
			return err
		}
		if audit != nil {
			audit.Subject = t.AccountID
		}
		return appendAuditTx(ctx, tx, audit)
	})
	return t, err
}

// AppendAudit implements Store.
func (s *PGStore) AppendAudit(ctx context.Context, e *AuditEntry) error {
	return s.inTx(ctx, func(tx pgx.Tx) error { return appendAuditTx(ctx, tx, e) })
}

// ListAudit implements Store.
func (s *PGStore) ListAudit(ctx context.Context, afterSeq int64, limit int) ([]AuditEntry, error) {
	if limit <= 0 {
		limit = 1000
	}
	rows, err := s.pool.Query(ctx, `SELECT seq, at, actor_account, subject_account, action, client_ip, detail, prev_hash, hash
		FROM svc_identity.audit_log WHERE seq > $1 ORDER BY seq LIMIT $2`, afterSeq, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AuditEntry
	for rows.Next() {
		var e AuditEntry
		var prev, h []byte
		if err := rows.Scan(&e.Seq, &e.At, &e.Actor, &e.Subject, &e.Action, &e.ClientIP, &e.Detail, &prev, &h); err != nil {
			return nil, err
		}
		copy(e.PrevHash[:], prev)
		copy(e.Hash[:], h)
		out = append(out, e)
	}
	return out, rows.Err()
}

// Ping implements Store.
func (s *PGStore) Ping(ctx context.Context) error { return s.pool.Ping(ctx) }
