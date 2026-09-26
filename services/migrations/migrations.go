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
	"bytes"
	"context"
	"database/sql"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"log/slog"
	"net/netip"
	"strconv"
	"strings"
	"time"

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
	// Markers are tables the legacy files create; a legacy-named schema without all of them is
	// not helios-backend's and is never adopted.
	Markers []string
}

// Schemas lists the service schemas in apply order. Each has its own goose version table
// (<schema>.goose_db_version) so services can later migrate independently.
var Schemas = []Schema{
	{Name: "svc_identity", Dir: "identity", Legacy: "identity", LegacyVersion: 1, Markers: []string{"account", "audit_head"}},
	{Name: "svc_orch", Dir: "orchestrator", Legacy: "orchestrator", LegacyVersion: 2, Markers: []string{"zone", "process"}},
}

// Go migrations (no .sql file). Their versions fill the gaps between the SQL files.
const (
	// identityEncryptPII encrypts the direct PII that pre-WP-0.15r databases stored in plain text
	// (e-mail addresses, client IPs) and re-chains the audit log without IPs, between
	// svc_identity's expand (00002) and contract (00004) steps.
	identityEncryptPII = 3
)

// Options configure Up.
type Options struct {
	// PIIKeys supplies the keys that encrypt pre-WP-0.15r plain-text PII. Up calls it only when
	// there is something to encrypt, so a fresh or already migrated database needs none; without
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
//
// The checks against foreign schemas (ErrForeignSchema) guard against accidents on a shared
// database, not against an attacker who can create schemas in it.
func adoptLegacy(ctx context.Context, db *sql.DB, s Schema, log *slog.Logger) error {
	if s.Legacy == "" {
		return nil
	}
	// The legacy schema is only ever created while s.Name is absent, and the rename happens under
	// the same lock, so a concurrent starter can never leave a stray legacy schema behind.
	adopted, err := inAdoptTx(ctx, db, func(tx *sql.Tx) (bool, error) {
		cur, err := inspect(ctx, tx, s.Name, nil)
		if err != nil {
			return false, err
		}
		old, err := inspect(ctx, tx, s.Legacy, s.Markers)
		if err != nil {
			return false, err
		}
		step, err := decideAdoption(s, cur, old)
		if err != nil {
			return false, err
		}
		switch step {
		case adoptedStray:
			// An older helios-backend started on this database after the upgrade would have
			// created the legacy schema again and written plain text into it.
			log.Error("stray legacy schema next to an adopted one: an older helios-backend ran on this upgraded database "+
				"and may have stored plain-text PII there; check it and drop it (the upgrade is one-way)",
				"legacy", s.Legacy, "schema", s.Name)
			return true, nil
		case adoptedAlready:
			return true, nil
		case replaceEmptyThenRunLegacy:
			// Pre-created, empty and with default privileges: adopt as fresh (the rename needs
			// the name free).
			if _, err := tx.ExecContext(ctx, "DROP SCHEMA "+s.Name); err != nil {
				return false, err
			}
			log.Warn("replaced a pre-created empty schema", "schema", s.Name)
		}
		_, err = tx.ExecContext(ctx, "CREATE SCHEMA IF NOT EXISTS "+s.Legacy)
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

// schemaState is what adoption needs to know about one schema.
type schemaState struct {
	exists, owned, empty, goose bool
	plainACL                    bool  // no GRANT on the schema and no default privileges in it
	gooseOnly                   bool  // it holds nothing but goose's version table
	markers                     bool  // every marker table exists
	maxVersion                  int64 // highest version in goose's table
}

// adoptStep is what adoptLegacy does next for one service schema.
type adoptStep int

const (
	runLegacy                 adoptStep = iota // apply the legacy files under s.Legacy, then rename
	replaceEmptyThenRunLegacy                  // drop a pre-created empty s.Name first
	adoptedAlready                             // s.Name is helios-backend's already
	adoptedStray                               // as adoptedAlready, but a goose-managed s.Legacy exists too
)

// decideAdoption is adoptLegacy's decision, given the state of s.Name (cur) and s.Legacy (old).
//
// inspect reads a schema in two statements, and other starters run goose on the legacy schema
// outside the adoption lock, so old can be torn: the first read may predate a starter's commit of
// its first legacy file and the second follow it (only goose's table, yet a version row past 0).
// A consistent read of an unfinished run shows only goose's table at version 0, so any state
// with nothing but goose's table at or below LegacyVersion is taken as ours and in progress: it
// holds no objects, so running the legacy files into it can harm nothing.
func decideAdoption(s Schema, cur, old schemaState) (adoptStep, error) {
	switch {
	case cur.goose:
		if old.goose {
			return adoptedStray, nil
		}
		return adoptedAlready, nil
	case cur.exists && cur.empty && cur.owned && cur.plainACL:
		return replaceEmptyThenRunLegacy, legacyAdoptable(s, old)
	case cur.exists && cur.empty && cur.owned:
		return 0, fmt.Errorf("%w: %s was pre-created with privileges that replacing it would drop; drop it first",
			ErrForeignSchema, s.Name)
	case cur.exists:
		return 0, fmt.Errorf("%w: %s exists but was not created by helios-backend (no goose version table)", ErrForeignSchema, s.Name)
	}
	return runLegacy, legacyAdoptable(s, old)
}

// legacyAdoptable reports whether the legacy files may run into s.Legacy (see decideAdoption).
func legacyAdoptable(s Schema, old schemaState) error {
	switch {
	case !old.exists:
	case !old.owned:
		return fmt.Errorf("%w: %s belongs to another role", ErrForeignSchema, s.Legacy)
	case old.empty:
	case old.goose && old.gooseOnly && old.maxVersion <= s.LegacyVersion:
		// Another starter's goose run has created its version table and not yet applied the
		// first file, or crashed right there, or this read is torn (see decideAdoption).
	case !old.goose || !old.markers:
		return fmt.Errorf("%w: %s holds objects helios-backend did not create", ErrForeignSchema, s.Legacy)
	case old.maxVersion > s.LegacyVersion:
		return fmt.Errorf("%w: %s is at version %d, newer than any legacy file (%d)", ErrForeignSchema, s.Legacy,
			old.maxVersion, s.LegacyVersion)
	}
	return nil
}

// inspect reads name's state. name is one of the constant schema names above, never input.
func inspect(ctx context.Context, tx *sql.Tx, name string, markers []string) (schemaState, error) {
	var st schemaState
	err := tx.QueryRowContext(ctx, `SELECT n.oid IS NOT NULL,
			COALESCE(pg_get_userbyid(n.nspowner) = current_user, false),
			NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.relnamespace = n.oid),
			EXISTS (SELECT 1 FROM pg_class c WHERE c.relnamespace = n.oid AND c.relname = 'goose_db_version'),
			n.nspacl IS NULL AND NOT EXISTS (SELECT 1 FROM pg_default_acl d WHERE d.defaclnamespace = n.oid),
			NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.relnamespace = n.oid
			   AND c.relname NOT IN ('goose_db_version', 'goose_db_version_pkey', 'goose_db_version_id_seq')),
			(SELECT count(*) FROM pg_class c WHERE c.relnamespace = n.oid AND c.relkind = 'r'
			   AND c.relname = ANY (string_to_array($2, ','))) = cardinality(string_to_array($2, ','))
		FROM (SELECT $1::name AS want) w LEFT JOIN pg_namespace n ON n.nspname = w.want`,
		name, strings.Join(markers, ",")).Scan(&st.exists, &st.owned, &st.empty, &st.goose, &st.plainACL, &st.gooseOnly,
		&st.markers)
	if err != nil || !st.goose {
		return st, err
	}
	err = tx.QueryRowContext(ctx, "SELECT COALESCE(max(version_id), 0) FROM "+name+".goose_db_version").Scan(&st.maxVersion)
	return st, err
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
	up := &goose.GoFunc{RunTx: func(ctx context.Context, tx *sql.Tx) error { return encryptLegacyPII(ctx, tx, o, log) }}
	down := &goose.GoFunc{RunTx: func(context.Context, *sql.Tx) error { return errIrreversible }}
	return []*goose.Migration{goose.NewGoMigration(identityEncryptPII, up, down)}
}

var errIrreversible = errors.New("migrations: svc_identity 3 is irreversible (the plain text is gone): restore a backup")

// ErrForeignSchema is returned when a schema with a service's name or legacy name exists but
// was not created by helios-backend; Up then renames nothing and applies nothing to it.
var ErrForeignSchema = errors.New("migrations: refusing to adopt a schema")

// ErrNeedPIIKeys is returned when plain-text PII rows exist but Options.PIIKeys is nil.
var ErrNeedPIIKeys = errors.New("migrations: plain-text PII needs the identity keys (subject KEK and blind-index pepper)")

// ErrAuditChainBroken is returned when the pre-WP-0.15r audit chain does not verify, row by row
// and against audit_head: migration 3 refuses to re-chain (and so launder) a chain that was
// already broken. The chain is unkeyed, so this catches corruption, naive edits and truncation,
// not a chain recomputed by someone who can write the table.
var ErrAuditChainBroken = errors.New("migrations: the audit chain does not verify; refusing to re-chain it")

// keySource loads the PII keys on first use and says what needed them if there are none.
type keySource struct {
	o    Options
	keys *identity.PIIKeys
}

func (k *keySource) get(what string, n int) (*identity.PIIKeys, error) {
	if k.keys != nil {
		return k.keys, nil
	}
	if k.o.PIIKeys == nil {
		return nil, fmt.Errorf("%w (%d %s)", ErrNeedPIIKeys, n, what)
	}
	keys, err := k.o.PIIKeys()
	if err != nil {
		return nil, err
	}
	k.keys = keys
	return keys, nil
}

// encryptLegacyPII is svc_identity migration 3. It writes what Register and the token paths now
// write for the rows the old schema stored in plain text, and nulls the plain-text copies:
//   - every account without email_ct gets a subject key, its address sealed under it and the
//     address's blind index;
//   - every ban reason (GM free text, 05 §1.17) is sealed into ban_reason_ct for its row;
//   - every refresh token's client IP is sealed into client_ip_ct for its row;
//   - the audit chain is verified in its old row format, row by row and against audit_head, its
//     IPs of the last 90 days move to the sealed login history (subject-less rows keep none),
//     unknown-login rows lose the unkeyed address digest they carried and ban rows their GM
//     reason, and the chain is recomputed in row format 2 with a closing "audit.rechain" row
//     that records the old head. Both heads are logged, so the old one also survives outside
//     the database.
//
// Values that are not IP addresses (dev seeds stored "seed") are dropped, not sealed.
//
// It runs in goose's transaction, so a failure leaves the database at version 2 with nothing
// half-done. It is sized for dev databases: it holds the rows in memory and makes a few round
// trips per row, about 6k audit rows/s, so ≤ 10k accounts and ≤ 100k audit rows take under 30 s
// (goose's lock makes concurrent starters wait up to 5 minutes). No address, IP or reason is
// ever logged.
func encryptLegacyPII(ctx context.Context, tx *sql.Tx, o Options, log *slog.Logger) error {
	ks := &keySource{o: o}
	emails, err := encryptLegacyEmails(ctx, tx, ks)
	if err != nil {
		return err
	}
	bans, err := encryptLegacyBanReasons(ctx, tx, ks)
	if err != nil {
		return err
	}
	ips, err := encryptLegacyTokenIPs(ctx, tx, ks)
	if err != nil {
		return err
	}
	rc, err := rechainAuditLog(ctx, tx, ks, time.Now().UTC())
	if err != nil {
		return err
	}
	if emails+bans+ips+rc.rows > 0 {
		log.Info("encrypted plain-text PII (WP-0.15r)", "accounts", emails, "ban_reasons", bans, "refresh_token_ips", ips,
			"audit_rows_rechained", rc.rows, "ips_to_login_history", rc.moved)
	}
	if rc.rows > 0 {
		log.Info("audit log re-chained in row format 2 (WP-0.15r)", "old_head", hex.EncodeToString(rc.oldHead[:]),
			"new_head", hex.EncodeToString(rc.newHead[:]), "marker_seq", rc.rows+1)
	}
	return nil
}

// isIP reports whether a legacy client_ip value is an IP address worth sealing.
func isIP(s string) bool {
	_, err := netip.ParseAddr(s)
	return err == nil
}

func encryptLegacyEmails(ctx context.Context, tx *sql.Tx, ks *keySource) (int, error) {
	rows, err := tx.QueryContext(ctx, `SELECT account_id, email FROM svc_identity.account
		WHERE email_ct IS NULL AND email IS NOT NULL ORDER BY account_id FOR UPDATE`)
	if err != nil {
		return 0, err
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
			return 0, err
		}
		todo = append(todo, l)
	}
	rows.Close()
	if err := rows.Err(); err != nil || len(todo) == 0 {
		return 0, err
	}
	keys, err := ks.get("accounts with a plain-text e-mail address", len(todo))
	if err != nil {
		return 0, err
	}
	for _, l := range todo {
		ct, bidx, key, err := keys.EncryptEmail(l.id, l.email)
		if err != nil {
			return 0, err
		}
		if _, err := tx.ExecContext(ctx, `UPDATE svc_identity.account
			SET email_ct = $2, email_bidx = $3, email = NULL, email_norm = NULL WHERE account_id = $1`, l.id, ct, bidx); err != nil {
			return 0, err
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO svc_identity.subject_key (account_id, wrapped_dek, kek_version)
			VALUES ($1, $2, $3)`, key.AccountID, key.WrappedDEK, key.KEKVersion); err != nil {
			return 0, err
		}
	}
	return len(todo), nil
}

func encryptLegacyBanReasons(ctx context.Context, tx *sql.Tx, ks *keySource) (int, error) {
	rows, err := tx.QueryContext(ctx, `SELECT account_id, ban_reason FROM svc_identity.account
		WHERE ban_reason IS NOT NULL ORDER BY account_id FOR UPDATE`)
	if err != nil {
		return 0, err
	}
	type legacy struct {
		id     int64
		reason string
	}
	var todo []legacy
	for rows.Next() {
		var l legacy
		if err := rows.Scan(&l.id, &l.reason); err != nil {
			rows.Close()
			return 0, err
		}
		todo = append(todo, l)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return 0, err
	}
	sealed := 0
	for _, l := range todo {
		var ct []byte
		sk, err := subjectKeyTx(ctx, tx, l.id)
		if err != nil {
			return 0, err
		}
		if sk != nil && l.reason != "" {
			keys, err := ks.get("accounts with a plain-text ban reason", len(todo))
			if err != nil {
				return 0, err
			}
			if ct, err = keys.Seal(sk, identity.BanReasonAAD(l.id), l.reason); err != nil {
				return 0, err
			}
			sealed++
		}
		if _, err := tx.ExecContext(ctx, `UPDATE svc_identity.account SET ban_reason_ct = $2, ban_reason = NULL
			WHERE account_id = $1`, l.id, ct); err != nil {
			return 0, err
		}
	}
	return sealed, nil
}

// subjectKeyTx reads an account's subject key inside the migration; nil if it has none.
func subjectKeyTx(ctx context.Context, tx *sql.Tx, accountID int64) (*identity.SubjectKey, error) {
	k := identity.SubjectKey{AccountID: accountID}
	err := tx.QueryRowContext(ctx, `SELECT wrapped_dek, kek_version FROM svc_identity.subject_key WHERE account_id = $1`,
		accountID).Scan(&k.WrappedDEK, &k.KEKVersion)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	return &k, err
}

func encryptLegacyTokenIPs(ctx context.Context, tx *sql.Tx, ks *keySource) (int, error) {
	rows, err := tx.QueryContext(ctx, `SELECT token_hash, account_id, client_ip FROM svc_identity.refresh_token
		WHERE client_ip IS NOT NULL ORDER BY token_hash FOR UPDATE`)
	if err != nil {
		return 0, err
	}
	type legacy struct {
		hash    []byte
		account int64
		ip      string
	}
	var todo []legacy
	for rows.Next() {
		var l legacy
		if err := rows.Scan(&l.hash, &l.account, &l.ip); err != nil {
			rows.Close()
			return 0, err
		}
		todo = append(todo, l)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return 0, err
	}
	sealed := 0
	for _, l := range todo {
		var ct []byte
		if isIP(l.ip) {
			sk, err := subjectKeyTx(ctx, tx, l.account)
			if err != nil {
				return 0, err
			}
			if sk != nil {
				keys, err := ks.get("refresh tokens with a plain-text client IP", len(todo))
				if err != nil {
					return 0, err
				}
				if ct, err = keys.Seal(sk, identity.RefreshIPAAD(l.hash), l.ip); err != nil {
					return 0, err
				}
				sealed++
			}
		}
		if _, err := tx.ExecContext(ctx, `UPDATE svc_identity.refresh_token SET client_ip_ct = $2, client_ip = NULL
			WHERE token_hash = $1`, l.hash, ct); err != nil {
			return 0, err
		}
	}
	return sealed, nil
}

// rechainResult is what rechainAuditLog did.
type rechainResult struct {
	rows, moved      int // rows re-chained; IPs moved to the login history
	oldHead, newHead [32]byte
}

// rechainAuditLog verifies the row-format-1 chain, moves its IPs and ban reasons out and
// recomputes it in row format 2.
func rechainAuditLog(ctx context.Context, tx *sql.Tx, ks *keySource, now time.Time) (rechainResult, error) {
	var res rechainResult
	rows, err := tx.QueryContext(ctx, `SELECT seq, at, actor_account, subject_account, action, COALESCE(client_ip, ''),
		detail, prev_hash, hash FROM svc_identity.audit_log ORDER BY seq FOR UPDATE`)
	if err != nil {
		return res, err
	}
	type legacy struct {
		e  identity.AuditEntry
		ip string
	}
	var todo []legacy
	for rows.Next() {
		var l legacy
		var prev, h []byte
		if err := rows.Scan(&l.e.Seq, &l.e.At, &l.e.Actor, &l.e.Subject, &l.e.Action, &l.ip, &l.e.Detail, &prev, &h); err != nil {
			rows.Close()
			return res, err
		}
		copy(l.e.PrevHash[:], prev)
		copy(l.e.Hash[:], h)
		todo = append(todo, l)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return res, err
	}
	var headSeq int64
	var headHash []byte
	if err := tx.QueryRowContext(ctx, `SELECT seq, hash FROM svc_identity.audit_head WHERE id = 1 FOR UPDATE`).
		Scan(&headSeq, &headHash); err != nil {
		return res, err
	}

	// Verify the old chain first, against its head: re-chaining must never launder a broken one.
	// The chain is unkeyed, so this catches corruption and edits, truncation included, but not
	// a chain recomputed by someone who can write the table; an external anchor (05 §1.17) will.
	var prev [32]byte // an empty log's head is the all-zero hash 00001 seeds
	for i, l := range todo {
		if l.e.Seq != int64(i+1) || l.e.PrevHash != prev || identity.LegacyChainHashV1(prev, &l.e, l.ip) != l.e.Hash {
			return res, fmt.Errorf("%w (at seq %d)", ErrAuditChainBroken, l.e.Seq)
		}
		prev = l.e.Hash
	}
	oldHead := prev
	if headSeq != int64(len(todo)) || !bytes.Equal(headHash, oldHead[:]) {
		return res, fmt.Errorf("%w (audit_head is at seq %d, the log at %d)", ErrAuditChainBroken, headSeq, len(todo))
	}
	if len(todo) == 0 {
		return res, nil
	}

	if _, err := tx.ExecContext(ctx, `ALTER TABLE svc_identity.audit_log DISABLE TRIGGER audit_log_append_only`); err != nil {
		return res, err
	}
	cutoff := now.Add(-identity.LoginHistoryRetention)
	moved := 0
	keysOf := map[int64]*identity.SubjectKey{}
	prev = [32]byte{}
	for _, l := range todo {
		e := l.e
		if isIP(l.ip) && e.Subject != 0 && !e.At.Before(cutoff) {
			sk, ok := keysOf[e.Subject]
			if !ok {
				if sk, err = subjectKeyTx(ctx, tx, e.Subject); err != nil {
					return res, err
				}
				keysOf[e.Subject] = sk
			}
			if sk != nil {
				keys, err := ks.get("audit rows with a plain-text client IP", len(todo))
				if err != nil {
					return res, err
				}
				// Negative event IDs never collide with the block IDs new rows get.
				ct, err := keys.Seal(sk, identity.LoginIPAAD(-e.Seq), l.ip)
				if err != nil {
					return res, err
				}
				if ct != nil {
					if _, err := tx.ExecContext(ctx, `INSERT INTO svc_identity.login_history
						(event_id, account_id, action, at, client_ip_ct) VALUES ($1, $2, $3, $4, $5)`,
						-e.Seq, e.Subject, e.Action, e.At, ct); err != nil {
						return res, err
					}
					moved++
				}
			}
		}
		// Unknown-login rows carried an unkeyed address digest, ban rows the GM's free-text
		// reason (now sealed in ban_reason_ct): chained rows hold only pseudonymous IDs (05 §1.17).
		if drop := map[string]string{identity.ActionLoginFailed: "login", identity.ActionBan: "reason"}[e.Action]; drop != "" {
			if e.Detail, err = dropDetailKey(e.Detail, drop); err != nil {
				return res, fmt.Errorf("migrations: audit seq %d detail: %w", e.Seq, err)
			}
		}
		e.PrevHash = prev
		e.Hash = identity.ChainHash(prev, &e)
		if _, err := tx.ExecContext(ctx, `UPDATE svc_identity.audit_log SET client_ip = NULL, detail = $2, prev_hash = $3,
			hash = $4 WHERE seq = $1`, e.Seq, e.Detail, e.PrevHash[:], e.Hash[:]); err != nil {
			return res, err
		}
		prev = e.Hash
	}
	marker := identity.NewAudit(now, 0, 0, identity.ActionAuditRechain, map[string]any{"from_format": 1, "to_format": 2,
		"rows": len(todo), "old_head": hex.EncodeToString(oldHead[:]), "reason": "client IPs and ban reasons left the chain (05 §1.17, §6.6; WP-0.15r)"})
	marker.Seq, marker.PrevHash = int64(len(todo))+1, prev
	marker.Hash = identity.ChainHash(prev, marker)
	if _, err := tx.ExecContext(ctx, `INSERT INTO svc_identity.audit_log
		(seq, at, actor_account, subject_account, action, detail, prev_hash, hash) VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`,
		marker.Seq, marker.At, marker.Actor, marker.Subject, marker.Action, marker.Detail, marker.PrevHash[:], marker.Hash[:]); err != nil {
		return res, err
	}
	if _, err := tx.ExecContext(ctx, `UPDATE svc_identity.audit_head SET seq = $1, hash = $2 WHERE id = 1`,
		marker.Seq, marker.Hash[:]); err != nil {
		return res, err
	}
	if _, err := tx.ExecContext(ctx, `ALTER TABLE svc_identity.audit_log ENABLE TRIGGER audit_log_append_only`); err != nil {
		return res, err
	}
	res.rows, res.moved, res.oldHead, res.newHead = len(todo), moved, oldHead, marker.Hash
	return res, nil
}

// dropDetailKey removes key from a canonical JSON detail and re-encodes it canonically (sorted
// keys, as NewAudit writes it). Numbers stay json.Number, so no 64-bit ID is rounded.
func dropDetailKey(detail, key string) (string, error) {
	var m map[string]any
	dec := json.NewDecoder(strings.NewReader(detail))
	dec.UseNumber()
	if err := dec.Decode(&m); err != nil {
		return "", err
	}
	if _, ok := m[key]; !ok {
		return detail, nil
	}
	delete(m, key)
	if len(m) == 0 {
		return "{}", nil
	}
	b, err := json.Marshal(m)
	return string(b), err
}
