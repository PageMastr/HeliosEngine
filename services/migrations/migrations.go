// Package migrations embeds the goose SQL migrations, one directory per service schema
// (05 §3.3), and applies them under PostgreSQL advisory locks so concurrent starters (several
// replicas, or a dev backend restarted quickly) never race.
//
// Schemas are named svc_<service> (05 §1.4, §3; CONF-06). Databases created before WP-0.15r
// have the schemas "identity" and "orchestrator"; Up renames them in place (see adoptLegacy), so
// the migration files that such databases already applied are never edited, and a fresh database
// runs exactly the same files in the same order. Later changes follow expand/contract (05 §3.3):
// a migration adds, a Go step backfills where keys are needed, and a later migration contracts.
package migrations

import (
	"context"
	"database/sql"
	"embed"
	"errors"
	"fmt"
	"io/fs"
	"log/slog"
	"strconv"
	"strings"

	"github.com/pressly/goose/v3"
	"github.com/pressly/goose/v3/lock"

	"github.com/PageMastr/scifi-test/services/internal/identity"
)

//go:embed identity/*.sql orchestrator/*.sql
var FS embed.FS

// Schema is one service's PostgreSQL schema and its migration directory in FS.
type Schema struct {
	Name string // svc_<service>
	Dir  string // directory in FS
	// Legacy is the schema's name before WP-0.15r, and LegacyVersion the last migration written
	// against that name. Every later file uses Name.
	Legacy        string
	LegacyVersion int64
}

// Schemas lists the service schemas in apply order. Each has its own goose version table
// (<schema>.goose_db_version) so services can later migrate independently.
var Schemas = []Schema{
	{Name: "svc_identity", Dir: "identity", Legacy: "identity", LegacyVersion: 1},
	{Name: "svc_orch", Dir: "orchestrator", Legacy: "orchestrator", LegacyVersion: 2},
}

// Go migrations (no .sql file). Their versions fill the gaps between the SQL files.
const (
	// identityEncryptEmails encrypts the e-mail addresses that pre-WP-0.15r databases stored in
	// plain text, between svc_identity's expand (00002) and contract (00004) steps.
	identityEncryptEmails = 3
)

// Options configure Up.
type Options struct {
	// PIIKeys supplies the keys that encrypt pre-WP-0.15r plain-text e-mail addresses. Up calls
	// it only when such rows exist, so a fresh or already migrated database needs none; without
	// it such a database stops at svc_identity version 2 with ErrNeedPIIKeys.
	PIIKeys func() (*identity.PIIKeys, error)
}

// Result summarises one schema's run.
type Result struct {
	Schema  string
	Applied int
	Version int64
}

// Up creates each schema if needed and applies its pending migrations. Safe to run from several
// processes at once.
func Up(ctx context.Context, db *sql.DB, log *slog.Logger, opts Options) ([]Result, error) {
	if log == nil {
		log = slog.Default()
	}
	var out []Result
	for _, s := range Schemas {
		if err := adoptLegacy(ctx, db, s, log); err != nil {
			return out, err
		}
		if _, err := db.ExecContext(ctx, "CREATE SCHEMA IF NOT EXISTS "+s.Name); err != nil {
			return out, fmt.Errorf("migrations: create schema %s: %w", s.Name, err)
		}
		p, err := newProvider(db, s, opts, log)
		if err != nil {
			return out, err
		}
		results, err := p.Up(ctx)
		if err != nil {
			return out, fmt.Errorf("migrations: %s: %w", s.Name, err)
		}
		v, err := p.GetDBVersion(ctx)
		if err != nil {
			return out, fmt.Errorf("migrations: %s version: %w", s.Name, err)
		}
		out = append(out, Result{Schema: s.Name, Applied: len(results), Version: v})
		log.Info("migrations applied", "schema", s.Name, "applied", len(results), "version", v)
	}
	return out, nil
}

// adoptLock serializes schema adoption across processes; goose's own lock covers only goose runs.
const adoptLock = `SELECT pg_advisory_xact_lock(hashtextextended('helios.migrations.adopt', 0))`

// adoptLegacy brings a database to s.Name. A pre-WP-0.15r database has s.Legacy at some version
// up to LegacyVersion; a fresh one has neither schema. Both take the same path: the legacy files
// are applied under the old name, then the schema is renamed, which moves every table, index,
// sequence, function and the goose version table with it (ALTER SCHEMA is atomic). The rename
// cannot be undone with goose down; restore from a backup instead.
func adoptLegacy(ctx context.Context, db *sql.DB, s Schema, log *slog.Logger) error {
	if s.Legacy == "" {
		return nil
	}
	// The legacy schema is only ever created while s.Name is absent, and the rename happens under
	// the same lock, so a concurrent starter can never leave a stray legacy schema behind.
	adopted, err := inAdoptTx(ctx, db, func(tx *sql.Tx) (bool, error) {
		if ok, err := schemaExists(ctx, tx, s.Name); ok || err != nil {
			return ok, err
		}
		// On a shared database a schema of that name may belong to something else: adopt it only
		// if helios-backend created it (it has goose's version table) or it is still empty.
		var ours, empty bool
		if err := tx.QueryRowContext(ctx, `SELECT
				EXISTS (SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
				        WHERE n.nspname = $1 AND c.relname = 'goose_db_version'),
				NOT EXISTS (SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1)`,
			s.Legacy).Scan(&ours, &empty); err != nil {
			return false, err
		}
		if !ours && !empty {
			return false, fmt.Errorf("%w: %s holds objects helios-backend did not create", ErrForeignSchema, s.Legacy)
		}
		_, err := tx.ExecContext(ctx, "CREATE SCHEMA IF NOT EXISTS "+s.Legacy)
		return false, err
	})
	if err != nil || adopted {
		return err
	}
	p, err := legacyProvider(db, s, log)
	if err != nil {
		return err
	}
	if _, err := p.Up(ctx); err != nil {
		// A concurrent starter may have renamed the schema under us.
		if ok, _ := schemaExists(ctx, db, s.Name); ok {
			return nil
		}
		return fmt.Errorf("migrations: %s (legacy name %s): %w", s.Name, s.Legacy, err)
	}
	renamed := false
	_, err = inAdoptTx(ctx, db, func(tx *sql.Tx) (bool, error) {
		if ok, err := schemaExists(ctx, tx, s.Name); ok || err != nil {
			return ok, err
		}
		if _, err := tx.ExecContext(ctx, "ALTER SCHEMA "+s.Legacy+" RENAME TO "+s.Name); err != nil {
			return false, err
		}
		renamed = true
		return true, nil
	})
	if err != nil {
		return fmt.Errorf("migrations: rename schema %s to %s: %w", s.Legacy, s.Name, err)
	}
	if renamed {
		log.Info("schema renamed (WP-0.15r, 05 §3)", "from", s.Legacy, "to", s.Name)
	}
	return nil
}

func inAdoptTx(ctx context.Context, db *sql.DB, fn func(*sql.Tx) (bool, error)) (bool, error) {
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	if _, err := tx.ExecContext(ctx, adoptLock); err != nil {
		return false, err
	}
	done, err := fn(tx)
	if err != nil {
		return false, err
	}
	return done, tx.Commit()
}

type querier interface {
	QueryRowContext(ctx context.Context, query string, args ...any) *sql.Row
}

func schemaExists(ctx context.Context, q querier, name string) (bool, error) {
	var ok bool
	err := q.QueryRowContext(ctx, `SELECT EXISTS (SELECT 1 FROM pg_namespace WHERE nspname = $1)`, name).Scan(&ok)
	return ok, err
}

// versions returns the versions of the SQL files in s.Dir.
func versions(s Schema) ([]int64, error) {
	entries, err := fs.ReadDir(FS, s.Dir)
	if err != nil {
		return nil, err
	}
	var out []int64
	for _, e := range entries {
		head, _, ok := strings.Cut(e.Name(), "_")
		v, err := strconv.ParseInt(head, 10, 64)
		if !ok || err != nil {
			return nil, fmt.Errorf("migrations: %s/%s: no version prefix", s.Dir, e.Name())
		}
		out = append(out, v)
	}
	return out, nil
}

func baseOptions(schema string, log *slog.Logger, extra ...goose.ProviderOption) ([]goose.ProviderOption, error) {
	locker, err := lock.NewPostgresSessionLocker()
	if err != nil {
		return nil, err
	}
	return append([]goose.ProviderOption{
		goose.WithTableName(schema + ".goose_db_version"),
		goose.WithSessionLocker(locker),
		goose.WithDisableGlobalRegistry(true),
		goose.WithSlog(log.With("component", "migrations", "schema", schema)),
	}, extra...), nil
}

// legacyProvider applies the files up to LegacyVersion under the legacy schema name.
func legacyProvider(db *sql.DB, s Schema, log *slog.Logger) (*goose.Provider, error) {
	sub, err := fs.Sub(FS, s.Dir)
	if err != nil {
		return nil, err
	}
	all, err := versions(s)
	if err != nil {
		return nil, err
	}
	var later []int64
	for _, v := range all {
		if v > s.LegacyVersion {
			later = append(later, v)
		}
	}
	opts, err := baseOptions(s.Legacy, log, goose.WithExcludeVersions(later))
	if err != nil {
		return nil, err
	}
	return goose.NewProvider(goose.DialectPostgres, db, sub, opts...)
}

func newProvider(db *sql.DB, s Schema, o Options, log *slog.Logger) (*goose.Provider, error) {
	sub, err := fs.Sub(FS, s.Dir)
	if err != nil {
		return nil, err
	}
	var extra []goose.ProviderOption
	if gm := goMigrations(s, o, log); len(gm) > 0 {
		extra = append(extra, goose.WithGoMigrations(gm...))
	}
	opts, err := baseOptions(s.Name, log, extra...)
	if err != nil {
		return nil, err
	}
	return goose.NewProvider(goose.DialectPostgres, db, sub, opts...)
}

func goMigrations(s Schema, o Options, log *slog.Logger) []*goose.Migration {
	if s.Name != "svc_identity" {
		return nil
	}
	up := &goose.GoFunc{RunTx: func(ctx context.Context, tx *sql.Tx) error { return encryptLegacyEmails(ctx, tx, o, log) }}
	return []*goose.Migration{goose.NewGoMigration(identityEncryptEmails, up, nil)}
}

// ErrForeignSchema is returned when a schema with a legacy name exists but was not created by
// helios-backend; Up then renames nothing and applies nothing to it.
var ErrForeignSchema = errors.New("migrations: refusing to adopt a schema")

// ErrNeedPIIKeys is returned when plain-text e-mail rows exist but Options.PIIKeys is nil.
var ErrNeedPIIKeys = errors.New("migrations: plain-text e-mail rows need the identity keys (subject KEK and blind-index pepper)")

// encryptLegacyEmails is svc_identity migration 3: every account without email_ct gets a subject
// key, its address sealed under it and the address's blind index, exactly as Register writes
// them; 00004 then drops the plain-text columns. It runs in goose's transaction, so a failure
// leaves the database at version 2 with nothing half-encrypted. Addresses are never logged.
func encryptLegacyEmails(ctx context.Context, tx *sql.Tx, o Options, log *slog.Logger) error {
	rows, err := tx.QueryContext(ctx, `SELECT account_id, email FROM svc_identity.account
		WHERE email_ct IS NULL ORDER BY account_id FOR UPDATE`)
	if err != nil {
		return err
	}
	type legacy struct {
		id    int64
		email string
	}
	var todo []legacy
	for rows.Next() {
		var l legacy
		if err := rows.Scan(&l.id, &l.email); err != nil {
			rows.Close()
			return err
		}
		todo = append(todo, l)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	if len(todo) == 0 {
		return nil
	}
	if o.PIIKeys == nil {
		return fmt.Errorf("%w (%d accounts)", ErrNeedPIIKeys, len(todo))
	}
	keys, err := o.PIIKeys()
	if err != nil {
		return err
	}
	for _, l := range todo {
		ct, bidx, key, err := keys.EncryptEmail(l.id, l.email)
		if err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, `UPDATE svc_identity.account SET email_ct = $2, email_bidx = $3 WHERE account_id = $1`,
			l.id, ct, bidx); err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO svc_identity.subject_key (account_id, wrapped_dek, kek_version)
			VALUES ($1, $2, $3)`, key.AccountID, key.WrappedDEK, key.KEKVersion); err != nil {
			return err
		}
	}
	log.Info("encrypted plain-text e-mail addresses", "accounts", len(todo))
	return nil
}
