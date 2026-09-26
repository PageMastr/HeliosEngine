//go:build integration

package integration

import (
	"bytes"
	"context"
	"errors"
	"io"
	"io/fs"
	"log/slog"
	"slices"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/jackc/pgx/v5/stdlib"
	"github.com/pressly/goose/v3"

	"github.com/PageMastr/scifi-test/services/internal/backend"
	"github.com/PageMastr/scifi-test/services/internal/identity"
	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
	"github.com/PageMastr/scifi-test/services/migrations"
	"github.com/PageMastr/scifi-test/services/pkg/keyring"
)

// WP-0.15r (09 §5.10.4 (a)) against real PostgreSQL: schema names, encrypted e-mail, region
// leases, and the upgrade of a database the pre-WP-0.15r tree created.

func columns(t *testing.T, pool *pgxpool.Pool, schema, table string) []string {
	t.Helper()
	rows, err := pool.Query(context.Background(), `SELECT column_name FROM information_schema.columns
		WHERE table_schema = $1 AND table_name = $2 ORDER BY column_name`, schema, table)
	if err != nil {
		t.Fatal(err)
	}
	var out []string
	for rows.Next() {
		var c string
		if err := rows.Scan(&c); err != nil {
			t.Fatal(err)
		}
		out = append(out, c)
	}
	return out
}

// checkSchemas asserts the net schema 05 §1.4, §3 and §6.6 require: every service schema is
// svc_<service> and none has its old name (CONF-06); the only e-mail columns are svc_identity's
// ciphertext and blind index (CONF-07); leases live in region_lease, not zone.
func checkSchemas(t *testing.T, pool *pgxpool.Pool) {
	t.Helper()
	ctx := context.Background()
	rows, err := pool.Query(ctx, `SELECT nspname FROM pg_namespace WHERE nspname NOT LIKE 'pg\_%'
		AND nspname NOT IN ('information_schema', 'public') ORDER BY nspname`)
	if err != nil {
		t.Fatal(err)
	}
	var schemas []string
	for rows.Next() {
		var n string
		_ = rows.Scan(&n)
		schemas = append(schemas, n)
	}
	if !slices.Equal(schemas, []string{"svc_identity", "svc_orch"}) {
		t.Fatalf("schemas %v, want exactly svc_identity and svc_orch", schemas)
	}
	rows, err = pool.Query(ctx, `SELECT table_schema || '.' || table_name || '.' || column_name FROM information_schema.columns
		WHERE column_name ILIKE '%mail%' AND table_schema NOT IN ('pg_catalog', 'information_schema') ORDER BY 1`)
	if err != nil {
		t.Fatal(err)
	}
	var mail []string
	for rows.Next() {
		var c string
		_ = rows.Scan(&c)
		mail = append(mail, c)
	}
	if !slices.Equal(mail, []string{"svc_identity.account.email_bidx", "svc_identity.account.email_ct"}) {
		t.Fatalf("e-mail columns %v: only email_ct and email_bidx may exist (05 §6.6)", mail)
	}
	if got := columns(t, pool, "svc_orch", "region_lease"); !slices.Equal(got, []string{"assigned_at", "holder_proc", "instance_id", "lease_gen", "region_id"}) {
		t.Fatalf("region_lease columns %v", got)
	}
	if got := columns(t, pool, "svc_orch", "zone"); slices.Contains(got, "lease_gen") || slices.Contains(got, "owner_process") {
		t.Fatalf("zone still carries a lease: %v", got)
	}
	if got := columns(t, pool, "svc_identity", "subject_key"); !slices.Equal(got, []string{"account_id", "analytics_id", "kek_version", "shredded_at", "wrapped_dek"}) {
		t.Fatalf("subject_key columns %v", got)
	}
}

func TestSchemasFollowThePlan(t *testing.T) {
	need(t)
	checkSchemas(t, stack.PG.Pool)
	checkSchemas(t, freshDB(t))
	// Every configured zone has its Whole region's lease row.
	var n int
	if err := stack.PG.Pool.QueryRow(context.Background(), `SELECT count(*) FROM svc_orch.zone z
		JOIN svc_orch.region_lease r ON r.region_id = z.zone_id AND r.instance_id = z.zone_id`).Scan(&n); err != nil ||
		n != len(stack.Cfg.Orchestrator.Zones) || n == 0 {
		t.Fatalf("%d region_lease rows for %d zones: %v", n, len(stack.Cfg.Orchestrator.Zones), err)
	}
}

func TestEmailIsEncryptedInPostgres(t *testing.T) {
	need(t)
	ctx := context.Background()
	keys, err := backend.LoadPIIKeys(stack.Cfg, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	var reg identity.RegisterResponse
	if st := call(t, apiURL, identity.ServicePath+"Register", "", identity.RegisterRequest{
		Email: "Cipher.Pilot@Example.com", Handle: "Cipher", Password: "stellar cartography"}, &reg); st != 200 {
		t.Fatalf("register: %d", st)
	}
	// The row holds ciphertext and the keyed index; login (any case) finds it through the index.
	var ct, bidx []byte
	if err := stack.PG.Pool.QueryRow(ctx, `SELECT email_ct, email_bidx FROM svc_identity.account WHERE account_id = $1`,
		reg.AccountID).Scan(&ct, &bidx); err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(bytes.ToLower(ct), []byte("cipher")) || !bytes.Equal(bidx, keys.EmailIndex("cipher.pilot@example.com")) {
		t.Fatalf("stored e-mail: ct %x bidx %x", ct, bidx)
	}
	if pair := login(t, "CIPHER.pilot@example.COM", "stellar cartography"); pair.AccountID != reg.AccountID {
		t.Fatalf("login by blind index: %d", pair.AccountID)
	}
	store := identity.NewPGStore(stack.PG.Pool)
	acct, err := store.AccountByEmailIndex(ctx, keys.EmailIndex("Cipher.Pilot@Example.com"))
	if err != nil {
		t.Fatal(err)
	}
	sk, err := store.SubjectKey(ctx, reg.AccountID)
	if err != nil {
		t.Fatal(err)
	}
	if email, err := keys.DecryptEmail(acct, sk); err != nil || email != "Cipher.Pilot@Example.com" {
		t.Fatalf("decrypt: %q %v", email, err)
	}
	// Every account, the dev seeds included, has its wrapped DEK.
	var missing int
	if err := stack.PG.Pool.QueryRow(ctx, `SELECT count(*) FROM svc_identity.account a LEFT JOIN svc_identity.subject_key k
		USING (account_id) WHERE k.wrapped_dek IS NULL OR a.email_ct IS NULL OR a.email_bidx IS NULL`).Scan(&missing); err != nil || missing != 0 {
		t.Fatalf("%d accounts without encrypted e-mail or subject key: %v", missing, err)
	}
}

// legacyDB builds a database exactly as the pre-WP-0.15r tree's migrations.Up left it: schemas
// "identity" (version 1) and "orchestrator" (version 2), from the same, unchanged files.
func legacyDB(t *testing.T) *pgxpool.Pool {
	t.Helper()
	ctx := context.Background()
	pool := emptyDB(t)
	db := stdlib.OpenDBFromPool(pool)
	defer db.Close()
	for _, s := range migrations.Schemas {
		if _, err := db.ExecContext(ctx, "CREATE SCHEMA "+s.Legacy); err != nil {
			t.Fatal(err)
		}
		sub, _ := fs.Sub(migrations.FS, s.Dir)
		entries, _ := fs.ReadDir(sub, ".")
		var later []int64
		for _, e := range entries {
			if v, _ := strconv.ParseInt(e.Name()[:5], 10, 64); v > s.LegacyVersion {
				later = append(later, v)
			}
		}
		p, err := goose.NewProvider(goose.DialectPostgres, db, sub, goose.WithTableName(s.Legacy+".goose_db_version"),
			goose.WithDisableGlobalRegistry(true), goose.WithExcludeVersions(later))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := p.Up(ctx); err != nil {
			t.Fatal(err)
		}
	}
	return pool
}

func TestLegacyDatabaseUpgrade(t *testing.T) {
	need(t)
	ctx := context.Background()
	pool := legacyDB(t)
	now := time.Now().UTC().Truncate(time.Microsecond)
	// Rows the old code wrote: a plain-text e-mail, a refresh token, a placed zone at generation 7.
	for _, q := range []string{
		`INSERT INTO identity.account (account_id, email, email_norm, handle, handle_norm, discriminator, password_hash,
			created_at, updated_at) VALUES (4242, 'Legacy.Pilot@Example.com', 'legacy.pilot@example.com', 'Legacy', 'legacy', 7,
			'$argon2id$x', $1, $1)`,
		`INSERT INTO identity.refresh_token (token_hash, family_id, account_id, issued_at, expires_at)
			VALUES ('\x01', 9, 4242, $1, $1)`,
		`INSERT INTO orchestrator.zone (zone_id, name, owner_process, lease_gen, updated_at) VALUES (1001, 'alpha', 55, 7, $1)`,
		`INSERT INTO orchestrator.placement_log (at, zone_id, process_id, lease_gen, action) VALUES ($1, 1001, 55, 7, 'assign')`,
	} {
		if _, err := pool.Exec(ctx, q, now); err != nil {
			t.Fatal(err)
		}
	}
	db := stdlib.OpenDBFromPool(pool)
	defer db.Close()
	quiet := slog.New(slog.NewTextHandler(io.Discard, nil))

	// Without the PII keys the upgrade stops before anything is dropped: the schema is renamed
	// and expanded, the plain-text rows are still there, and nothing is half-encrypted.
	if _, err := migrations.Up(ctx, db, quiet, migrations.Options{}); !errors.Is(err, migrations.ErrNeedPIIKeys) {
		t.Fatalf("upgrade without keys: %v", err)
	}
	if got := columns(t, pool, "svc_identity", "account"); !slices.Contains(got, "email") || !slices.Contains(got, "email_ct") {
		t.Fatalf("stopped state: %v", got)
	}

	kek, _ := keyring.New("subject-kek", nil, now)
	pepper, _ := keyring.New("email-bidx-pepper", nil, now)
	keys, err := identity.NewPIIKeys(kek, pepper)
	if err != nil {
		t.Fatal(err)
	}
	res, err := migrations.Up(ctx, db, quiet, migrations.Options{PIIKeys: func() (*identity.PIIKeys, error) { return keys, nil }})
	if err != nil {
		t.Fatal(err)
	}
	if len(res) != 2 || res[0].Version != 4 || res[1].Version != 4 {
		t.Fatalf("versions after upgrade: %+v", res)
	}
	checkSchemas(t, pool)

	// The account kept everything but its plain text: it is found by blind index and decrypts to
	// the address as entered, its refresh token survived, and the audit guard still holds.
	store := identity.NewPGStore(pool)
	acct, err := store.AccountByEmailIndex(ctx, keys.EmailIndex(" LEGACY.pilot@example.com"))
	if err != nil || acct.ID != 4242 || acct.Discriminator != 7 {
		t.Fatalf("migrated account: %+v %v", acct, err)
	}
	sk, _ := store.SubjectKey(ctx, 4242)
	if email, err := keys.DecryptEmail(acct, sk); err != nil || email != "Legacy.Pilot@Example.com" {
		t.Fatalf("migrated e-mail: %q %v", email, err)
	}
	if fam, err := store.ActiveFamily(ctx, 9, now.Add(-time.Minute)); err != nil || fam != 4242 {
		t.Fatalf("refresh token after the rename: %d %v", fam, err)
	}
	if _, err := pool.Exec(ctx, "DELETE FROM svc_identity.audit_log"); err == nil || !strings.Contains(err.Error(), "svc_identity.audit_log is append-only") {
		t.Fatalf("audit guard after the rename: %v", err)
	}

	// The zone's lease moved to region_lease with its generation, and the next assignment
	// continues from it, term-fenced as before.
	var holder, gen, logged int64
	if err := pool.QueryRow(ctx, `SELECT holder_proc, lease_gen FROM svc_orch.region_lease WHERE region_id = 1001 AND instance_id = 1001`).
		Scan(&holder, &gen); err != nil || holder != 55 || gen != 7 {
		t.Fatalf("region_lease from zone: %d %d %v", holder, gen, err)
	}
	if err := pool.QueryRow(ctx, `SELECT region_id FROM svc_orch.placement_log`).Scan(&logged); err != nil || logged != 1001 {
		t.Fatalf("placement_log.region_id: %d %v", logged, err)
	}
	orch := orchestrator.NewPGStore(pool)
	term, ok, err := orch.AcquireLeadership(ctx, "legacy", "it", time.Minute)
	if err != nil || !ok {
		t.Fatalf("leadership: %v %v", ok, err)
	}
	f := orchestrator.Fence{Shard: "legacy", Term: term}
	if g, err := orch.AssignRegion(ctx, f, orchestrator.WholeRegion(1001), 56, now); err != nil || g != 8 {
		t.Fatalf("next generation after the upgrade: %d %v", g, err)
	}
	if rec, err := orch.CreateProcess(ctx, f, orchestrator.ProcessInfo{Name: "c", Kind: orchestrator.KindCell}, now); err != nil || rec.Epoch != 1 {
		t.Fatalf("process_id_seq after the rename: %+v %v", rec, err)
	}

	// Idempotent from here on.
	res, err = migrations.Up(ctx, db, quiet, migrations.Options{})
	if err != nil || res[0].Applied != 0 || res[1].Applied != 0 {
		t.Fatalf("re-run: %+v %v", res, err)
	}
}

// A schema that merely has a legacy name (a shared database) is never adopted or renamed.
func TestForeignLegacyNamedSchemaIsLeftAlone(t *testing.T) {
	need(t)
	ctx := context.Background()
	pool := emptyDB(t)
	if _, err := pool.Exec(ctx, `CREATE SCHEMA identity; CREATE TABLE identity.users (id INT)`); err != nil {
		t.Fatal(err)
	}
	db := stdlib.OpenDBFromPool(pool)
	defer db.Close()
	if _, err := migrations.Up(ctx, db, slog.New(slog.NewTextHandler(io.Discard, nil)), migrations.Options{}); !errors.Is(err, migrations.ErrForeignSchema) {
		t.Fatalf("foreign schema: %v", err)
	}
	if got := columns(t, pool, "identity", "users"); !slices.Equal(got, []string{"id"}) {
		t.Fatalf("the foreign schema was touched: %v", got)
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_namespace WHERE nspname LIKE 'svc\_%'`).Scan(&n); err != nil || n != 0 {
		t.Fatalf("%d svc_ schemas created: %v", n, err)
	}
}

// Concurrent first starts on one fresh database (several replicas) must converge on one
// svc_<service> schema each, with no stray legacy schema.
func TestConcurrentFirstMigration(t *testing.T) {
	need(t)
	pool := emptyDB(t)
	quiet := slog.New(slog.NewTextHandler(io.Discard, nil))
	errs := make(chan error, 4)
	for i := 0; i < 4; i++ {
		go func() {
			// A pool per starter, as separate processes have.
			own, err := pgxpool.New(context.Background(), pool.Config().ConnString())
			if err != nil {
				errs <- err
				return
			}
			defer own.Close()
			db := stdlib.OpenDBFromPool(own)
			defer db.Close()
			_, err = migrations.Up(context.Background(), db, quiet, migrations.Options{})
			errs <- err
		}()
	}
	for i := 0; i < 4; i++ {
		if err := <-errs; err != nil {
			t.Error(err)
		}
	}
	checkSchemas(t, pool)
}
