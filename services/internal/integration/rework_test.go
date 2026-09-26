//go:build integration

package integration

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"os"
	"path/filepath"
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
	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/migrations"
	"github.com/PageMastr/scifi-test/services/pkg/keyring"
)

// WP-0.15r (09 §5.10.4 (a)) against real PostgreSQL: schema names, encrypted PII, region
// leases, and the upgrade of a database the pre-WP-0.15r tree created.

var quietLog = slog.New(slog.NewTextHandler(io.Discard, nil))

func columns(t *testing.T, pool *pgxpool.Pool, schema, table string) []string {
	t.Helper()
	return queryStrings(t, pool, `SELECT column_name FROM information_schema.columns
		WHERE table_schema = $1 AND table_name = $2 ORDER BY column_name`, schema, table)
}

func queryStrings(t *testing.T, pool *pgxpool.Pool, q string, args ...any) []string {
	t.Helper()
	rows, err := pool.Query(context.Background(), q, args...)
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
// svc_<service> and none has its old name (CONF-06); every direct-PII column CONF-07 names
// (e-mail, IP address, date of birth, real name) is *_ct ciphertext or a *_bidx blind index in
// svc_identity; leases live in region_lease, not zone.
func checkSchemas(t *testing.T, pool *pgxpool.Pool) {
	t.Helper()
	schemas := queryStrings(t, pool, `SELECT nspname FROM pg_namespace WHERE nspname NOT LIKE 'pg\_%'
		AND nspname NOT IN ('information_schema', 'public') ORDER BY nspname`)
	if !slices.Equal(schemas, []string{"svc_identity", "svc_orch"}) {
		t.Fatalf("schemas %v, want exactly svc_identity and svc_orch", schemas)
	}
	pii := queryStrings(t, pool, `SELECT table_schema || '.' || table_name || '.' || column_name FROM information_schema.columns
		WHERE table_schema NOT IN ('pg_catalog', 'information_schema')
		  AND column_name ~* '(mail|(^|_)ip(_|$)|ip_?addr|(^|_)dob(_|$)|birth|real_?name)' ORDER BY 1`)
	want := []string{"svc_identity.account.email_bidx", "svc_identity.account.email_ct",
		"svc_identity.login_history.client_ip_ct", "svc_identity.refresh_token.client_ip_ct"}
	if !slices.Equal(pii, want) {
		t.Fatalf("direct-PII columns %v, want only %v (05 §6.6, CONF-07)", pii, want)
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
	if got := columns(t, pool, "svc_identity", "audit_log"); !slices.Contains(got, "note_digest") {
		t.Fatalf("audit_log columns %v", got)
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

func TestPIIIsEncryptedInPostgres(t *testing.T) {
	need(t)
	ctx := context.Background()
	keys, err := backend.LoadPIIKeys(stack.Cfg, quietLog)
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
	pair := login(t, "CIPHER.pilot@example.COM", "stellar cartography")
	if pair.AccountID != reg.AccountID {
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
	// The client IP: sealed on the refresh token row and in the login history, never in the audit log.
	h := sha256.Sum256([]byte(pair.RefreshToken))
	var tokIP []byte
	if err := stack.PG.Pool.QueryRow(ctx, `SELECT client_ip_ct FROM svc_identity.refresh_token WHERE token_hash = $1`, h[:]).
		Scan(&tokIP); err != nil {
		t.Fatal(err)
	}
	if ip, err := keys.Open(sk, identity.RefreshIPAAD(h[:]), tokIP); err != nil || ip != "127.0.0.1" {
		t.Fatalf("refresh token IP: %q %v", ip, err)
	}
	hist, err := store.LoginHistory(ctx, reg.AccountID)
	if err != nil || len(hist) != 2 || hist[0].Action != identity.ActionRegister || hist[1].Action != identity.ActionLogin {
		t.Fatalf("login history: %+v %v", hist, err)
	}
	if ip, err := keys.Open(sk, identity.LoginIPAAD(hist[1].ID), hist[1].ClientIPCT); err != nil || ip != "127.0.0.1" {
		t.Fatalf("login history IP: %q %v", ip, err)
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
	pool := emptyDB(t)
	for _, s := range migrations.Schemas {
		legacySchema(t, pool, s)
	}
	return pool
}

func legacySchema(t *testing.T, pool *pgxpool.Pool, s migrations.Schema) {
	t.Helper()
	ctx := context.Background()
	db := stdlib.OpenDBFromPool(pool)
	defer db.Close()
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

// legacyAudit appends rows to a pre-WP-0.15r audit log, chained in its row format 1 (with IPs).
type legacyAudit struct {
	seq  int64
	prev [32]byte
}

func (l *legacyAudit) add(t *testing.T, pool *pgxpool.Pool, e *identity.AuditEntry, ip string) {
	t.Helper()
	l.seq++
	e.Seq, e.PrevHash = l.seq, l.prev
	e.Hash = identity.LegacyChainHashV1(l.prev, e, ip)
	if _, err := pool.Exec(context.Background(), `INSERT INTO identity.audit_log
		(seq, at, actor_account, subject_account, action, client_ip, detail, prev_hash, hash) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)`,
		e.Seq, e.At, e.Actor, e.Subject, e.Action, ip, e.Detail, e.PrevHash[:], e.Hash[:]); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(context.Background(), `UPDATE identity.audit_head SET seq = $1, hash = $2 WHERE id = 1`, e.Seq, e.Hash[:]); err != nil {
		t.Fatal(err)
	}
	l.prev = e.Hash
}

func newPIIKeys(t *testing.T) *identity.PIIKeys {
	t.Helper()
	kek, _ := keyring.New("subject-kek", nil, time.Now())
	pepper, _ := keyring.New("email-bidx-pepper", nil, time.Now())
	keys, err := identity.NewPIIKeys(kek, pepper)
	if err != nil {
		t.Fatal(err)
	}
	return keys
}

func gooseVersion(t *testing.T, pool *pgxpool.Pool, schema string) int64 {
	t.Helper()
	var v int64
	if err := pool.QueryRow(context.Background(), "SELECT max(version_id) FROM "+schema+".goose_db_version").Scan(&v); err != nil {
		t.Fatal(err)
	}
	return v
}

// noPlainTextOnDisk flushes the buffers and reads the heap files of the tables that held plain
// text: after 00005's rewrite, no old row version or dropped column may keep it (PR #8 review,
// nit 7). The test runs as the same OS user as the embedded server, so it can read them.
func noPlainTextOnDisk(t *testing.T, pool *pgxpool.Pool, secrets ...string) {
	t.Helper()
	ctx := context.Background()
	if _, err := pool.Exec(ctx, "CHECKPOINT"); err != nil {
		t.Fatal(err)
	}
	var dataDir string
	if err := pool.QueryRow(ctx, "SHOW data_directory").Scan(&dataDir); err != nil {
		t.Fatal(err)
	}
	for _, rel := range []string{"svc_identity.account", "svc_identity.refresh_token", "svc_identity.audit_log"} {
		var path string
		if err := pool.QueryRow(ctx, "SELECT pg_relation_filepath($1)", rel).Scan(&path); err != nil {
			t.Fatal(err)
		}
		files, _ := filepath.Glob(filepath.Join(dataDir, path) + "*")
		if len(files) == 0 {
			t.Fatalf("%s: no files at %s", rel, path)
		}
		for _, f := range files {
			b, err := os.ReadFile(f)
			if err != nil {
				t.Fatal(err)
			}
			for _, s := range secrets {
				if bytes.Contains(bytes.ToLower(b), bytes.ToLower([]byte(s))) {
					t.Errorf("%s (%s) still holds %q", rel, filepath.Base(f), s)
				}
			}
		}
	}
}

func TestLegacyDatabaseUpgrade(t *testing.T) {
	need(t)
	ctx := context.Background()
	pool := legacyDB(t)
	now := time.Now().UTC().Truncate(time.Microsecond)
	// Rows the old code wrote: plain-text e-mail and client IPs, a placed zone at generation 7.
	for _, q := range []string{
		`INSERT INTO identity.account (account_id, email, email_norm, handle, handle_norm, discriminator, password_hash,
			created_at, updated_at) VALUES (4242, 'Legacy.Pilot@Example.com', 'legacy.pilot@example.com', 'Legacy', 'legacy', 7,
			'$argon2id$x', $1, $1)`,
		`INSERT INTO identity.refresh_token (token_hash, family_id, account_id, issued_at, expires_at, client_ip)
			VALUES ('\x01', 9, 4242, $1, $1, '198.51.100.7')`,
		`INSERT INTO identity.refresh_token (token_hash, family_id, account_id, issued_at, expires_at)
			VALUES ('\x02', 10, 4242, $1, $1)`,
		`INSERT INTO orchestrator.zone (zone_id, name, owner_process, lease_gen, updated_at) VALUES (1001, 'alpha', 55, 7, $1)`,
		`INSERT INTO orchestrator.placement_log (at, zone_id, process_id, lease_gen, action) VALUES ($1, 1001, 55, 7, 'assign')`,
	} {
		if _, err := pool.Exec(ctx, q, now); err != nil {
			t.Fatal(err)
		}
	}
	unkeyed := sha256.Sum256([]byte("ghost@example.com"))
	var la legacyAudit
	la.add(t, pool, identity.NewAudit(now.Add(-time.Hour), 4242, 4242, identity.ActionRegister,
		map[string]any{"seed": false, "tag": "Legacy#0007"}), "198.51.100.7")
	la.add(t, pool, identity.NewAudit(now.Add(-30*time.Minute), 0, 0, identity.ActionLoginFailed,
		map[string]any{"reason": "unknown_login", "login": hex.EncodeToString(unkeyed[:12])}), "203.0.113.66")
	la.add(t, pool, identity.NewAudit(now.Add(-100*24*time.Hour), 4242, 4242, identity.ActionLogin,
		map[string]any{"ua": "old"}), "198.51.100.8")
	la.add(t, pool, identity.NewAudit(now, 7, 4242, identity.ActionBan,
		map[string]any{"reason": "it", "until": "2099-01-01T00:00:00Z"}), "")
	db := stdlib.OpenDBFromPool(pool)
	defer db.Close()

	// Without the PII keys the upgrade stops before anything is dropped or half-done: the schema
	// is renamed and expanded, and migration 3 rolled back as a whole.
	if _, err := migrations.Up(ctx, db, quietLog, migrations.Options{}); !errors.Is(err, migrations.ErrNeedPIIKeys) {
		t.Fatalf("upgrade without keys: %v", err)
	}
	var keysN, sealedN, ipsN int
	if err := pool.QueryRow(ctx, `SELECT (SELECT count(*) FROM svc_identity.subject_key),
		(SELECT count(*) FROM svc_identity.account WHERE email_ct IS NOT NULL),
		(SELECT count(*) FROM svc_identity.audit_log WHERE client_ip <> '')`).Scan(&keysN, &sealedN, &ipsN); err != nil {
		t.Fatal(err)
	}
	if keysN != 0 || sealedN != 0 || ipsN != 3 || gooseVersion(t, pool, "svc_identity") != 2 {
		t.Fatalf("stopped state: %d keys, %d sealed, %d audit IPs, version %d", keysN, sealedN, ipsN,
			gooseVersion(t, pool, "svc_identity"))
	}

	keys := newPIIKeys(t)
	res, err := migrations.Up(ctx, db, quietLog, migrations.Options{PIIKeys: func() (*identity.PIIKeys, error) { return keys, nil }})
	if err != nil {
		t.Fatal(err)
	}
	if len(res) != 2 || res[0].Version != 5 || res[1].Version != 4 {
		t.Fatalf("versions after upgrade: %+v", res)
	}
	checkSchemas(t, pool)

	// The account kept everything but its plain text.
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
	var ct1, ct2 []byte
	if err := pool.QueryRow(ctx, `SELECT (SELECT client_ip_ct FROM svc_identity.refresh_token WHERE token_hash = '\x01'),
		(SELECT client_ip_ct FROM svc_identity.refresh_token WHERE token_hash = '\x02')`).Scan(&ct1, &ct2); err != nil {
		t.Fatal(err)
	}
	if ip, err := keys.Open(sk, identity.RefreshIPAAD([]byte{1}), ct1); err != nil || ip != "198.51.100.7" || ct2 != nil {
		t.Fatalf("migrated refresh-token IPs: %q %v, empty one %x", ip, err, ct2)
	}

	// The audit log verifies in row format 2, carries no IP, and records the re-chain with the
	// old head; the IP of the recent row with a subject moved to the login history, the others
	// (no subject, or past the 90-day retention) are gone.
	all, err := store.ListAudit(ctx, 0, 0)
	if err != nil || len(all) != 5 {
		t.Fatalf("audit rows: %d %v", len(all), err)
	}
	if err := identity.VerifyAuditChain([32]byte{}, all); err != nil {
		t.Fatal(err)
	}
	if all[1].Detail != `{"reason":"unknown_login"}` || all[4].Action != identity.ActionAuditRechain ||
		!strings.Contains(all[4].Detail, hex.EncodeToString(la.prev[:])) {
		t.Fatalf("scrubbed row %q, marker %+v", all[1].Detail, all[4])
	}
	hist, err := store.LoginHistory(ctx, 4242)
	if err != nil || len(hist) != 1 || hist[0].ID != -1 || hist[0].Action != identity.ActionRegister {
		t.Fatalf("login history: %+v %v", hist, err)
	}
	if ip, err := keys.Open(sk, identity.LoginIPAAD(-1), hist[0].ClientIPCT); err != nil || ip != "198.51.100.7" {
		t.Fatalf("moved IP: %q %v", ip, err)
	}
	if h0, _ := store.LoginHistory(ctx, 0); len(h0) != 0 {
		t.Fatalf("a subject-less IP was kept: %+v", h0)
	}
	if _, err := pool.Exec(ctx, "DELETE FROM svc_identity.audit_log"); err == nil || !strings.Contains(err.Error(), "svc_identity.audit_log is append-only") {
		t.Fatalf("audit guard after the re-chain: %v", err)
	}
	// Service writes continue the new chain.
	if err := store.AppendAudit(ctx, identity.NewAudit(now, 1, 4242, "test.after", nil)); err != nil {
		t.Fatal(err)
	}
	if all, _ = store.ListAudit(ctx, 0, 0); identity.VerifyAuditChain([32]byte{}, all) != nil || len(all) != 6 {
		t.Fatalf("chain after a new row: %d", len(all))
	}
	noPlainTextOnDisk(t, pool, "Legacy.Pilot@Example.com", "legacy.pilot@example.com", "198.51.100.7", "203.0.113.66",
		"198.51.100.8", hex.EncodeToString(unkeyed[:12]))

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
	res, err = migrations.Up(ctx, db, quietLog, migrations.Options{})
	if err != nil || res[0].Applied != 0 || res[1].Applied != 0 {
		t.Fatalf("re-run: %+v %v", res, err)
	}
}

// A pre-WP-0.15r chain that does not verify is never re-chained, which would launder it.
func TestLegacyBrokenAuditChainIsNotRechained(t *testing.T) {
	need(t)
	ctx := context.Background()
	pool := legacyDB(t)
	var la legacyAudit
	la.add(t, pool, identity.NewAudit(time.Now(), 0, 0, "test.one", nil), "")
	la.add(t, pool, identity.NewAudit(time.Now(), 0, 0, "test.two", nil), "")
	if _, err := pool.Exec(ctx, `ALTER TABLE identity.audit_log DISABLE TRIGGER audit_log_append_only;
		UPDATE identity.audit_log SET detail = '{"edited":true}' WHERE seq = 1;
		ALTER TABLE identity.audit_log ENABLE TRIGGER audit_log_append_only`); err != nil {
		t.Fatal(err)
	}
	db := stdlib.OpenDBFromPool(pool)
	defer db.Close()
	if _, err := migrations.Up(ctx, db, quietLog, migrations.Options{}); !errors.Is(err, migrations.ErrAuditChainBroken) {
		t.Fatalf("broken chain: %v", err)
	}
	if v := gooseVersion(t, pool, "svc_identity"); v != 2 {
		t.Fatalf("version %d after a refused re-chain", v)
	}
}

// Schemas that merely have a service's name or a legacy name (a shared database) are never
// adopted or renamed; an empty, pre-created svc_* schema is adopted as fresh; a legacy schema an
// older binary recreated next to an adopted one is reported (the upgrade is one-way).
func TestSchemaAdoptionGuards(t *testing.T) {
	need(t)
	ctx := context.Background()
	up := func(pool *pgxpool.Pool, log *slog.Logger) error {
		db := stdlib.OpenDBFromPool(pool)
		defer db.Close()
		_, err := migrations.Up(ctx, db, log, migrations.Options{})
		return err
	}
	for name, c := range map[string]struct {
		setup, table string   // the foreign schema's setup and its table, which must stay as it was
		svc          []string // the svc_ schemas afterwards
	}{
		"foreign table in identity": {`CREATE SCHEMA identity; CREATE TABLE identity.users (id INT)`, "identity.users", nil},
		"foreign goose-managed": {`CREATE SCHEMA identity; CREATE TABLE identity.goose_db_version (id SERIAL, version_id BIGINT,
			is_applied BOOLEAN, tstamp TIMESTAMP); INSERT INTO identity.goose_db_version (version_id, is_applied) VALUES (0, true), (1, true);
			CREATE TABLE identity.users (id INT)`, "identity.users", nil},
		"non-empty svc_identity": {`CREATE SCHEMA svc_identity; CREATE TABLE svc_identity.users (id INT)`, "svc_identity.users",
			[]string{"svc_identity"}},
		"svc_identity with privileges": {`CREATE SCHEMA svc_identity; GRANT USAGE ON SCHEMA svc_identity TO PUBLIC`, "",
			[]string{"svc_identity"}},
		"foreign orchestrator schema": {`CREATE SCHEMA orchestrator; CREATE TABLE orchestrator.jobs (id INT)`, "orchestrator.jobs",
			[]string{"svc_identity"}},
	} {
		t.Run(name, func(t *testing.T) {
			pool := emptyDB(t)
			if _, err := pool.Exec(ctx, c.setup); err != nil {
				t.Fatal(err)
			}
			if err := up(pool, quietLog); !errors.Is(err, migrations.ErrForeignSchema) {
				t.Fatalf("want ErrForeignSchema: %v", err)
			}
			for _, schema := range []string{"identity", "orchestrator"} {
				if strings.Contains(c.setup, "SCHEMA "+schema+";") && len(queryStrings(t, pool,
					`SELECT nspname FROM pg_namespace WHERE nspname = $1`, schema)) != 1 {
					t.Fatalf("the foreign schema %s was renamed", schema)
				}
			}
			if c.table != "" {
				schema, table, _ := strings.Cut(c.table, ".")
				if got := columns(t, pool, schema, table); !slices.Equal(got, []string{"id"}) {
					t.Fatalf("the foreign table %s was changed: %v", c.table, got)
				}
			}
			if got := queryStrings(t, pool, `SELECT nspname FROM pg_namespace WHERE nspname LIKE 'svc\_%' ORDER BY 1`); !slices.Equal(got, c.svc) {
				t.Fatalf("svc_ schemas %v, want %v", got, c.svc)
			}
		})
	}
	t.Run("legacy schema newer than any legacy file", func(t *testing.T) {
		pool := emptyDB(t)
		legacySchema(t, pool, migrations.Schemas[0])
		if _, err := pool.Exec(ctx, `INSERT INTO identity.goose_db_version (version_id, is_applied) VALUES (5, true)`); err != nil {
			t.Fatal(err)
		}
		if err := up(pool, quietLog); !errors.Is(err, migrations.ErrForeignSchema) {
			t.Fatalf("want ErrForeignSchema: %v", err)
		}
	})
	t.Run("empty pre-created svc schemas", func(t *testing.T) {
		pool := emptyDB(t)
		if _, err := pool.Exec(ctx, `CREATE SCHEMA svc_identity; CREATE SCHEMA svc_orch`); err != nil {
			t.Fatal(err)
		}
		if err := up(pool, quietLog); err != nil {
			t.Fatal(err)
		}
		checkSchemas(t, pool)
	})
	t.Run("stray legacy schema from an older binary is reported", func(t *testing.T) {
		pool := freshDB(t)
		legacySchema(t, pool, migrations.Schemas[0]) // what main's migrations.Up would do now
		var buf bytes.Buffer
		if err := up(pool, slog.New(slog.NewTextHandler(&buf, nil))); err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(buf.String(), "stray legacy schema") {
			t.Fatalf("no report of the stray schema:\n%s", buf.String())
		}
	})
}

// Concurrent first starts on one fresh database (several replicas) must converge on one
// svc_<service> schema each, with no stray legacy schema.
func TestConcurrentFirstMigration(t *testing.T) {
	need(t)
	pool := emptyDB(t)
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
			_, err = migrations.Up(context.Background(), db, quietLog, migrations.Options{})
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

// The PII key files are tied to the data they protect (PR #8 review, nit 8): with encrypted
// accounts in the database a missing file is not regenerated, and a wrong KEK stops the start.
func TestPIIKeysGuardEncryptedData(t *testing.T) {
	need(t)
	ctx := context.Background()
	pool := freshDB(t)
	cfg := platform.Default()
	cfg.DataDir = t.TempDir()
	keys, err := backend.OpenPIIKeys(ctx, cfg, quietLog, pool) // empty database: generates the files
	if err != nil {
		t.Fatal(err)
	}
	ct, bidx, sk, err := keys.EncryptEmail(1, "k@example.com")
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now().UTC()
	if err := identity.NewPGStore(pool).CreateAccount(ctx, &identity.Account{ID: 1, EmailCT: ct, EmailBidx: bidx, Handle: "k",
		HandleNorm: "k", Discriminator: 1, PasswordHash: "x", CreatedAt: now, UpdatedAt: now}, sk, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := backend.OpenPIIKeys(ctx, cfg, quietLog, pool); err != nil {
		t.Fatalf("the right keys: %v", err)
	}
	kekPath := backend.KeyPath(cfg, backend.KeyFileSubjectKEK)
	saved, _ := os.ReadFile(kekPath)
	_ = os.Remove(kekPath)
	if _, err := backend.OpenPIIKeys(ctx, cfg, quietLog, pool); err == nil || !strings.Contains(err.Error(), "missing") {
		t.Fatalf("a missing KEK with encrypted accounts: %v", err)
	}
	if _, err := os.Stat(kekPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("a new KEK was generated over encrypted data")
	}
	other, _ := keyring.New("subject-kek", nil, now)
	if err := other.Save(kekPath); err != nil {
		t.Fatal(err)
	}
	if _, err := backend.OpenPIIKeys(ctx, cfg, quietLog, pool); err == nil || !strings.Contains(err.Error(), "does not unwrap") {
		t.Fatalf("a replaced KEK: %v", err)
	}
	if err := os.WriteFile(kekPath, saved, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := backend.OpenPIIKeys(ctx, cfg, quietLog, pool); err != nil {
		t.Fatalf("the restored KEK: %v", err)
	}
}

// Concurrent starters upgrading one pre-WP-0.15r database converge: every account is encrypted
// exactly once and the re-chained log verifies.
func TestConcurrentLegacyUpgrade(t *testing.T) {
	need(t)
	ctx := context.Background()
	pool := legacyDB(t)
	now := time.Now().UTC().Truncate(time.Microsecond)
	var la legacyAudit
	for i := int64(1); i <= 5; i++ {
		if _, err := pool.Exec(ctx, `INSERT INTO identity.account (account_id, email, email_norm, handle, handle_norm, discriminator,
			password_hash, created_at, updated_at) VALUES ($1, $2, $2, 'p', 'p', $4, 'x', $3, $3)`, i, fmt.Sprintf("p%d@example.com", i), now,
			int16(i)); err != nil {
			t.Fatal(err)
		}
		la.add(t, pool, identity.NewAudit(now, i, i, identity.ActionRegister, nil), fmt.Sprintf("192.0.2.%d", i))
	}
	keys := newPIIKeys(t)
	errs := make(chan error, 4)
	for i := 0; i < 4; i++ {
		go func() {
			own, err := pgxpool.New(ctx, pool.Config().ConnString())
			if err != nil {
				errs <- err
				return
			}
			defer own.Close()
			db := stdlib.OpenDBFromPool(own)
			defer db.Close()
			_, err = migrations.Up(ctx, db, quietLog, migrations.Options{PIIKeys: func() (*identity.PIIKeys, error) { return keys, nil }})
			errs <- err
		}()
	}
	for i := 0; i < 4; i++ {
		if err := <-errs; err != nil {
			t.Error(err)
		}
	}
	checkSchemas(t, pool)
	var nKeys, nHist int
	if err := pool.QueryRow(ctx, `SELECT (SELECT count(*) FROM svc_identity.subject_key),
		(SELECT count(*) FROM svc_identity.login_history)`).Scan(&nKeys, &nHist); err != nil || nKeys != 5 || nHist != 5 {
		t.Fatalf("%d subject keys, %d history rows: %v", nKeys, nHist, err)
	}
	all, err := identity.NewPGStore(pool).ListAudit(ctx, 0, 0)
	if err != nil || len(all) != 6 || identity.VerifyAuditChain([32]byte{}, all) != nil {
		t.Fatalf("chain: %d rows %v", len(all), err)
	}
}
