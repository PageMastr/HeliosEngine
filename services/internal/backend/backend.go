// Package backend assembles helios-backend: infrastructure (stack), migrations, keys, the
// Phase 0 services (identity, session, orchestrator), the public API and ops listeners, and an
// ordered graceful shutdown. cmd/helios-backend and the integration test both use it, so the
// test boots exactly what developers run.
package backend

import (
	"context"
	"crypto/ed25519"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/pprof"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/prometheus/client_golang/prometheus"

	"github.com/PageMastr/scifi-test/services/internal/app"
	"github.com/PageMastr/scifi-test/services/internal/identity"
	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/internal/session"
	"github.com/PageMastr/scifi-test/services/internal/stack"
	"github.com/PageMastr/scifi-test/services/migrations"
	"github.com/PageMastr/scifi-test/services/pkg/authn"
	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/idgen"
	"github.com/PageMastr/scifi-test/services/pkg/keyring"
	"github.com/PageMastr/scifi-test/services/pkg/ratelimit"
)

// Key file names under <data>/keys (05 §5).
const (
	KeyFileJWT     = "jwt-ed25519.json"
	KeyFileNetcode = "netcode-shard.json"
	KeyFileTickets = "reconnect-ticket.json"
	// KeyFileNATS holds the embedded NATS fleet password (cells and gateways): the standard
	// base64 of the current secret, i.e. the "secret" string in the file as written.
	KeyFileNATS = "nats-fleet.json"
	// KeyFileSubjectKEK wraps every account's data key and KeyFileEmailPepper keys the e-mail
	// blind index (05 §6.6: dev uses local key files, prod the secret store until KMS). Losing
	// them makes every stored e-mail address unreadable and unfindable; never prune a KEK
	// generation that still wraps a DEK.
	KeyFileSubjectKEK  = "subject-kek.json"
	KeyFileEmailPepper = "email-bidx-pepper.json"
)

// Options tune Start for tests.
type Options struct {
	Version string
	// HashParams overrides the argon2id parameters (tests use tiny ones); nil = config.
	HashParams *identity.HashParams
	// TraceOut receives spans when traces = "stdout" (default os.Stderr).
	TraceOut io.Writer
}

// Backend is a running helios-backend.
type Backend struct {
	Cfg     *platform.Config
	Log     *slog.Logger
	Health  *platform.Health
	Metrics *prometheus.Registry

	PG    *stack.Postgres
	Bus   *stack.Bus
	Cache *stack.Cache

	Identity     *identity.Service
	Session      *session.Service
	Orchestrator *orchestrator.Service
	NetcodeKeys  *keyring.Ring
	// IDs mints this process's block IDs (accounts, refresh families).
	IDs *idgen.Minter

	API *platform.Server
	Ops *platform.Server

	runner    *app.Runner
	telemetry *platform.Telemetry
	lock      *stack.DataDirLock
	pii       *identity.PIIKeys // see piiKeys
}

// KeyPath returns the path of a key file (cfg.KeysDir, default <data>/keys).
func KeyPath(cfg *platform.Config, name string) string {
	if cfg.KeysDir != "" {
		return filepath.Join(cfg.KeysDir, name)
	}
	return filepath.Join(cfg.DataDir, "keys", name)
}

// LoadKeys opens a keyring. Dev generates missing files; prod refuses to, because a silently
// generated shard key would make every gateway reject the tokens (05 §6.5: keys come from the
// secret store).
func LoadKeys(cfg *platform.Config, name, purpose string, log *slog.Logger) (*keyring.Ring, error) {
	path := KeyPath(cfg, name)
	if cfg.Env == "prod" {
		ring, err := keyring.Load(path)
		if err != nil {
			return nil, fmt.Errorf("prod needs provisioned %s keys at %s: %w", purpose, path, err)
		}
		return ring, nil
	}
	ring, created, err := keyring.LoadOrCreate(path, purpose, nil, time.Now())
	if err != nil {
		return nil, err
	}
	if created {
		log.Info("generated key file", "purpose", purpose, "file", path)
	}
	return ring, nil
}

// Keyring purposes of the PII key files; a file with another purpose is refused.
const (
	purposeSubjectKEK  = "subject-kek"
	purposeEmailPepper = "email-bidx-pepper"
)

// LoadPIIKeys opens the keys that protect Identity's direct PII (05 §6.6); dev generates missing
// files, prod requires them (LoadKeys). Each file must carry its own purpose, and no KEK
// generation may equal the pepper, so another key file copied into place is refused. Use
// OpenPIIKeys when a database is at hand: it also refuses to generate keys over encrypted data.
func LoadPIIKeys(cfg *platform.Config, log *slog.Logger) (*identity.PIIKeys, error) {
	kek, err := LoadKeys(cfg, KeyFileSubjectKEK, purposeSubjectKEK, log)
	if err != nil {
		return nil, err
	}
	pepper, err := LoadKeys(cfg, KeyFileEmailPepper, purposeEmailPepper, log)
	if err != nil {
		return nil, err
	}
	for _, r := range []struct {
		ring    *keyring.Ring
		file    string
		purpose string
	}{{kek, KeyFileSubjectKEK, purposeSubjectKEK}, {pepper, KeyFileEmailPepper, purposeEmailPepper}} {
		if r.ring.Purpose != r.purpose {
			return nil, fmt.Errorf("%s has purpose %q, want %q", KeyPath(cfg, r.file), r.ring.Purpose, r.purpose)
		}
	}
	for _, k := range kek.Keys {
		for _, p := range pepper.Keys {
			if subtle.ConstantTimeCompare(k.Secret, p.Secret) == 1 {
				return nil, fmt.Errorf("%s and %s share a secret", KeyPath(cfg, KeyFileSubjectKEK), KeyPath(cfg, KeyFileEmailPepper))
			}
		}
	}
	return identity.NewPIIKeys(kek, pepper)
}

// OpenPIIKeys loads the PII keys for the database behind pool. While that database holds
// encrypted accounts it refuses to generate a missing key file (a new KEK or pepper would make
// every stored address unreadable or unfindable: crypto-shredding by accident), and it checks,
// on one account per KEK generation in use, that the KEK unwraps the stored DEK and that the
// pepper reproduces the stored blind index of the decrypted address. A wrong or replaced key
// file then stops the start instead of failing each request (or, for the pepper, letting every
// address register a second account).
func OpenPIIKeys(ctx context.Context, cfg *platform.Config, log *slog.Logger, pool *pgxpool.Pool) (*identity.PIIKeys, error) {
	var table, encrypted bool
	if err := pool.QueryRow(ctx, `SELECT to_regclass('svc_identity.subject_key') IS NOT NULL`).Scan(&table); err != nil {
		return nil, err
	}
	if table {
		if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM svc_identity.subject_key)`).Scan(&encrypted); err != nil {
			return nil, err
		}
	}
	if encrypted {
		for _, name := range []string{KeyFileSubjectKEK, KeyFileEmailPepper} {
			if _, err := os.Stat(KeyPath(cfg, name)); errors.Is(err, os.ErrNotExist) {
				return nil, fmt.Errorf("%s is missing but the database holds encrypted accounts: restore it "+
					"(a new key would make every stored address unreadable)", KeyPath(cfg, name))
			}
		}
	}
	keys, err := LoadPIIKeys(cfg, log)
	if err != nil || !encrypted {
		return keys, err
	}
	type sampled struct {
		key  identity.SubjectKey
		acct identity.Account
	}
	rows, err := pool.Query(ctx, `SELECT DISTINCT ON (k.kek_version) k.account_id, k.wrapped_dek, k.kek_version,
			a.email_ct, a.email_bidx
		FROM svc_identity.subject_key k JOIN svc_identity.account a USING (account_id)
		WHERE k.wrapped_dek IS NOT NULL ORDER BY k.kek_version, k.account_id`)
	if err != nil {
		return nil, err
	}
	sample, err := pgx.CollectRows(rows, func(row pgx.CollectableRow) (sampled, error) {
		var s sampled
		err := row.Scan(&s.key.AccountID, &s.key.WrappedDEK, &s.key.KEKVersion, &s.acct.EmailCT, &s.acct.EmailBidx)
		s.acct.ID = s.key.AccountID
		return s, err
	})
	if err != nil {
		return nil, err
	}
	for i := range sample {
		s := &sample[i]
		dek, err := keys.UnwrapSubjectKey(&s.key)
		if err != nil {
			return nil, fmt.Errorf("%s does not unwrap the stored keys of KEK generation %d (wrong or replaced key file): %w",
				KeyPath(cfg, KeyFileSubjectKEK), s.key.KEKVersion, err)
		}
		dek.Clear()
		if s.acct.EmailCT == nil {
			continue
		}
		email, err := keys.DecryptEmail(&s.acct, &s.key)
		if err != nil {
			return nil, fmt.Errorf("the stored address of account %d does not decrypt: %w", s.acct.ID, err)
		}
		if subtle.ConstantTimeCompare(keys.EmailIndex(email), s.acct.EmailBidx) != 1 {
			return nil, fmt.Errorf("%s does not match the stored blind indexes (wrong or replaced key file)",
				KeyPath(cfg, KeyFileEmailPepper))
		}
	}
	return keys, nil
}

// Start brings up everything. On error, whatever was started is torn down again.
func Start(ctx context.Context, cfg *platform.Config, log *slog.Logger, opts Options) (b *Backend, err error) {
	if cfg.Enabled(platform.ServiceSession) && !cfg.Enabled(platform.ServiceIdentity) {
		return nil, errors.New("session needs identity in the same process until services talk Connect RPC (Phase 1)")
	}
	b = &Backend{Cfg: cfg, Log: log, Health: platform.NewHealth(2 * time.Second), Metrics: platform.NewRegistry(),
		runner: app.NewRunner(log)}
	defer func() {
		if err != nil {
			sctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()
			_ = b.Shutdown(sctx)
			b = nil
		}
	}()
	if b.lock, err = stack.LockDataDir(cfg.DataDir); err != nil {
		return b, err
	}
	if err := os.MkdirAll(filepath.Join(cfg.DataDir, "logs"), 0o700); err != nil {
		return b, err
	}
	traceOut := opts.TraceOut
	if traceOut == nil {
		traceOut = os.Stderr
	}
	if b.telemetry, err = platform.SetupTelemetry(cfg.Telemetry, "helios-backend", opts.Version, traceOut); err != nil {
		return b, err
	}

	// Infrastructure.
	needDB := cfg.Enabled(platform.ServiceIdentity) || cfg.Enabled(platform.ServiceOrchestrator)
	if needDB {
		if b.PG, err = stack.OpenPostgres(ctx, cfg, log.With("component", "postgres")); err != nil {
			return b, err
		}
		db := b.PG.SQLDB()
		var mopts migrations.Options
		if cfg.Enabled(platform.ServiceIdentity) {
			// Only a process that runs Identity holds its keys; others cannot encrypt old rows.
			mopts.PIIKeys = func() (*identity.PIIKeys, error) { return b.piiKeys(ctx) }
		}
		_, err = migrations.Up(ctx, db, log, mopts)
		_ = db.Close()
		if err != nil {
			return b, err
		}
		b.Health.AddCheck("postgres", func(ctx context.Context) error { return b.PG.Pool.Ping(ctx) })
	}
	var busAuth stack.BusAuth
	if cfg.Bus.URL == "embedded" && cfg.Bus.Listen != "" {
		ring, err := LoadKeys(cfg, KeyFileNATS, "nats-fleet", log)
		if err != nil {
			return b, err
		}
		busAuth.FleetPassword = FleetPassword(ring)
	}
	if b.Bus, err = stack.OpenBus(ctx, cfg, busAuth, log); err != nil {
		return b, err
	}
	b.Health.AddCheck("nats", func(context.Context) error {
		if !b.Bus.Conn.IsConnected() {
			return errors.New("disconnected")
		}
		return nil
	})
	if b.Cache, err = stack.OpenCache(ctx, cfg, log); err != nil {
		return b, err
	}
	b.Health.AddCheck("valkey", func(ctx context.Context) error { return b.Cache.Client.Ping(ctx).Err() })

	clk := clock.System{}
	limiter := ratelimit.New(b.Cache.Client, clk, "rl:", log.With("component", "ratelimit"))
	var orchStore *orchestrator.PGStore
	if b.PG != nil {
		orchStore = orchestrator.NewPGStore(b.PG.Pool)
	}

	// Services, in dependency order: orchestrator (gateway registry), identity, session.
	if cfg.Enabled(platform.ServiceOrchestrator) {
		holder, err := leaderHolder(cfg)
		if err != nil {
			return b, err
		}
		sup := orchestrator.NewSupervisor(spawnSpecs(cfg), b.childEnv(), filepath.Join(cfg.DataDir, "logs"),
			platform.Service(log, "supervisor"), b.Metrics)
		if b.Orchestrator, err = orchestrator.New(orchestrator.Deps{Config: cfg.Orchestrator, Shard: cfg.Shard,
			ShardIndex: cfg.ShardIndex, Store: orchStore, NATS: b.Bus.Conn, Supervisor: sup, Holder: holder, Clock: clk,
			Log: platform.Service(log, platform.ServiceOrchestrator), Metrics: b.Metrics}); err != nil {
			return b, err
		}
		b.runner.Add(b.Orchestrator)
	}
	if cfg.Enabled(platform.ServiceIdentity) {
		// Identity mints account and refresh-family IDs from blocks of the shard's id_alloc row
		// (05 §1.4.5); in the modular monolith it reads the row directly, as the orchestrator's
		// AllocateIdBlocks would.
		src := idgen.SourceFunc(func(ctx context.Context, n int) ([]int64, error) {
			return orchStore.AllocateIdBlocks(ctx, cfg.ShardIndex, n)
		})
		if b.IDs, err = idgen.NewMinter(cfg.ShardIndex, src, idgen.Options{Clock: clk, Log: log.With("component", "idgen")}); err != nil {
			return b, err
		}
		if err := b.IDs.Prime(ctx); err != nil {
			return b, err
		}
		if b.Identity, err = b.newIdentity(cfg, log, limiter, b.IDs, clk, opts); err != nil {
			return b, err
		}
		b.runner.Add(b.Identity)
	}
	if cfg.Enabled(platform.ServiceSession) {
		if b.Session, err = b.newSession(cfg, log, limiter, clk); err != nil {
			return b, err
		}
		b.runner.Add(b.Session)
	}
	for _, s := range b.runner.Services() {
		b.Health.AddCheck("svc:"+s.Name(), s.Health)
	}
	if err := b.runner.Start(ctx); err != nil {
		return b, err
	}
	if cfg.Seed == "dev" && b.Identity != nil {
		n, err := b.Identity.SeedDev(ctx)
		if err != nil {
			return b, err
		}
		if n > 0 {
			log.Info("seeded dev accounts", "created", n, "login", "dev1@helios.test or dev1#0001", "password", "dev")
		}
	}

	// Listeners.
	b.API = platform.NewServer("api", cfg.HTTP.Addr, b.apiRouter(), cfg.HTTP.TLSCert, cfg.HTTP.TLSKey, log)
	if err := b.API.Start(); err != nil {
		return b, err
	}
	b.Ops = platform.NewServer("ops", cfg.Ops.Addr, b.opsRouter(), "", "", log)
	if err := b.Ops.Start(); err != nil {
		return b, err
	}
	b.Health.SetReady(true)
	return b, nil
}

// FleetPassword derives the embedded NATS fleet password from its keyring: the standard base64
// of the current secret, exactly the "secret" string stored in the file, so a C++ process that
// is not supervised by helios-backend can read it from there.
func FleetPassword(ring *keyring.Ring) string {
	return base64.StdEncoding.EncodeToString(ring.Current().Secret)
}

// leaderHolder names this backend in orch_leader: host plus absolute data directory. The
// data-dir lock makes that unique among live instances and it is stable across restarts, so a
// backend restarted after a crash reclaims its own leadership at once instead of waiting out
// the 10 s lease.
func leaderHolder(cfg *platform.Config) (string, error) {
	host, err := os.Hostname()
	if err != nil {
		host = "unknown-host"
	}
	dir, err := filepath.Abs(cfg.DataDir)
	if err != nil {
		return "", err
	}
	return host + ":" + dir, nil
}

// piiKeys opens the PII keys once; migrations (to encrypt pre-WP-0.15r rows) and identity share
// them. The first call may come from inside a migration, before any account is encrypted.
func (b *Backend) piiKeys(ctx context.Context) (*identity.PIIKeys, error) {
	if b.pii == nil {
		k, err := OpenPIIKeys(ctx, b.Cfg, b.Log, b.PG.Pool)
		if err != nil {
			return nil, err
		}
		b.pii = k
	}
	return b.pii, nil
}

func (b *Backend) newIdentity(cfg *platform.Config, log *slog.Logger, limiter *ratelimit.Limiter, ids *idgen.Minter,
	clk clock.Clock, opts Options) (*identity.Service, error) {
	ring, err := LoadKeys(cfg, KeyFileJWT, "jwt-ed25519", log)
	if err != nil {
		return nil, err
	}
	piiKeys, err := b.piiKeys(context.Background())
	if err != nil {
		return nil, err
	}
	var pubs []ed25519.PublicKey
	for _, k := range ring.Keys {
		pubs = append(pubs, ed25519.NewKeyFromSeed(k.Secret).Public().(ed25519.PublicKey))
	}
	signing := authn.NewSigningKey(ed25519.NewKeyFromSeed(ring.Current().Secret))
	params := identity.HashParams{MemoryKiB: cfg.Identity.Argon2.MemoryKiB, Iterations: cfg.Identity.Argon2.Iterations,
		Parallelism: cfg.Identity.Argon2.Parallelism, SaltLen: 16, KeyLen: 32}
	if opts.HashParams != nil {
		params = *opts.HashParams
	}
	return identity.New(identity.Deps{
		Config: cfg.Identity, Store: identity.NewPGStore(b.PG.Pool), PII: piiKeys,
		Hasher:  identity.NewHasher(params, cfg.Identity.Argon2.MaxConcurrent),
		Issuer:  authn.NewIssuer(signing, cfg.Identity.Issuer, cfg.Identity.Audience, cfg.Identity.AccessTTL.D(), clk),
		Keys:    authn.NewKeySet(pubs...),
		Limiter: limiter, Cache: b.Cache.Client, IDs: ids, Clock: clk,
		Log: platform.Service(log, platform.ServiceIdentity), Metrics: b.Metrics,
		// A ban ends the account's live game session at once (kick + tombstone).
		OnBan: func(ctx context.Context, accountID int64) {
			if b.Session == nil {
				return
			}
			if err := b.Session.EndAccount(ctx, accountID, "banned"); err != nil {
				log.WarnContext(ctx, "ending the banned account's session failed", "account", accountID, "err", err)
			}
		},
	})
}

func (b *Backend) newSession(cfg *platform.Config, log *slog.Logger, limiter *ratelimit.Limiter,
	clk clock.Clock) (*session.Service, error) {
	netcodeKeys, err := LoadKeys(cfg, KeyFileNetcode, "netcode-shard", log)
	if err != nil {
		return nil, err
	}
	b.NetcodeKeys = netcodeKeys
	ticketKeys, err := LoadKeys(cfg, KeyFileTickets, "reconnect-ticket", log)
	if err != nil {
		return nil, err
	}
	protocolID, err := cfg.ProtocolIDValue()
	if err != nil {
		return nil, err
	}
	deps := session.Deps{
		Config: cfg.Session, Shard: cfg.Shard, ProtocolID: protocolID, ShardKeys: netcodeKeys,
		Tickets: session.NewTicketSealer(ticketKeys), Store: session.NewStore(b.Cache.Client), Accounts: b.Identity,
		Limiter: limiter, NATS: b.Bus.Conn, Clock: clk,
		Log: platform.Service(log, platform.ServiceSession), Metrics: b.Metrics,
	}
	if cfg.Env == "dev" {
		// No character service until Phase 1: dev accepts any character ID the client names.
		// Elsewhere character IDs are refused rather than trusted.
		deps.Characters = session.AnyCharacter{}
	}
	if b.Orchestrator != nil {
		deps.Gateways = gatewayAdapter{b.Orchestrator.Registry()}
		deps.Zones = zoneAdapter{b.Orchestrator.Registry()}
	}
	return session.New(deps)
}

type gatewayAdapter struct{ reg *orchestrator.Registry }

// Gateways implements session.GatewayDirectory.
func (g gatewayAdapter) Gateways() []session.GatewayInstance {
	in := g.reg.Gateways()
	out := make([]session.GatewayInstance, len(in))
	for i, x := range in {
		out[i] = session.GatewayInstance{ProcessID: x.ProcessID, Address: x.Address, KeyID: x.KeyID, FreeSlots: x.FreeSlots}
	}
	return out
}

type zoneAdapter struct{ reg *orchestrator.Registry }

// ZoneExists implements session.ZoneChecker.
func (z zoneAdapter) ZoneExists(id int64) bool {
	for _, zone := range z.reg.Zones() {
		if zone.ID == id {
			return true
		}
	}
	return false
}

// childEnv is passed to supervised cells and gateways so they find the bus and the keys.
func (b *Backend) childEnv() []string {
	env := []string{
		"HELIOS_SHARD=" + b.Cfg.Shard,
		"HELIOS_NETCODE_KEYS=" + KeyPath(b.Cfg, KeyFileNetcode),
		"HELIOS_PROTOCOL_ID=" + b.Cfg.Session.ProtocolID,
		fmt.Sprintf("HELIOS_TOKEN_LIFETIME=%d", int(b.Cfg.Session.TokenExpiry.D()/time.Second)),
	}
	if b.Bus != nil && b.Bus.ClientURL != "" {
		env = append(env, "HELIOS_NATS_URL="+b.Bus.ClientURL)
		if b.Bus.FleetPassword != "" {
			env = append(env, "HELIOS_NATS_USER="+stack.FleetUser, "HELIOS_NATS_PASSWORD="+b.Bus.FleetPassword)
		}
	}
	return env
}

func spawnSpecs(cfg *platform.Config) []orchestrator.ProcessSpec {
	filter := strings.TrimSpace(cfg.Orchestrator.SpawnFilter)
	if filter == "none" {
		return nil
	}
	allowed := map[string]bool{}
	for _, n := range strings.Split(filter, ",") {
		allowed[strings.TrimSpace(n)] = true
	}
	var out []orchestrator.ProcessSpec
	for _, s := range cfg.Orchestrator.Spawn {
		if filter != "all" && filter != "" && !allowed[s.Name] {
			continue
		}
		out = append(out, orchestrator.ProcessSpec{Name: s.Name, Exe: s.Exe, Args: s.Args, Env: s.Env, Dir: s.Dir,
			Restart: s.Restart, BackoffMin: s.BackoffMin.D(), BackoffMax: s.BackoffMax.D(), StopTimeout: s.StopTimeout.D()})
	}
	return out
}

func (b *Backend) apiRouter() http.Handler {
	r := chi.NewRouter()
	r.Use(middleware.Recoverer)
	r.Use(platform.NewHTTPMetrics(b.Metrics, "api").Middleware)
	trusted, _ := platform.ParseTrustedProxies(b.Cfg.HTTP.TrustedProxies)
	if b.Identity != nil {
		r.Use(authn.Middleware(b.Identity.Verifier(), nil)) // lenient: stale tokens must not block Login/Refresh
		b.Identity.Mount(r, identity.HTTPOptions{TrustedProxies: trusted})
	}
	if b.Session != nil {
		b.Session.Mount(r, trusted)
	}
	r.Get("/healthz", b.Health.LiveHandler().ServeHTTP)
	return platform.TraceHTTP(r, "api")
}

func (b *Backend) opsRouter() http.Handler {
	r := chi.NewRouter()
	r.Use(middleware.Recoverer)
	r.Handle("/metrics", platform.MetricsHandler(b.Metrics))
	r.Handle("/healthz", b.Health.LiveHandler())
	r.Handle("/readyz", b.Health.ReadyHandler())
	if b.Cfg.Ops.Pprof {
		r.HandleFunc("/debug/pprof/*", pprof.Index)
		r.HandleFunc("/debug/pprof/cmdline", pprof.Cmdline)
		r.HandleFunc("/debug/pprof/profile", pprof.Profile)
		r.HandleFunc("/debug/pprof/symbol", pprof.Symbol)
		r.HandleFunc("/debug/pprof/trace", pprof.Trace)
	}
	if b.Orchestrator != nil {
		b.Orchestrator.Mount(r)
	}
	if b.Identity != nil && b.Cfg.Env == "dev" {
		b.Identity.MountAdmin(r)
	}
	return r
}

// Shutdown drains in reverse: readiness off, stop accepting API calls, stop services
// (supervised children first), then the ops listener and the infrastructure.
func (b *Backend) Shutdown(ctx context.Context) error {
	var errs []error
	b.Health.SetReady(false)
	if b.API != nil {
		errs = append(errs, b.API.Shutdown(ctx))
	}
	errs = append(errs, b.runner.Stop(ctx))
	if b.IDs != nil {
		b.IDs.Close()
	}
	if b.Ops != nil {
		errs = append(errs, b.Ops.Shutdown(ctx))
	}
	if b.Cache != nil {
		b.Cache.Close()
	}
	if b.Bus != nil {
		b.Bus.Close()
	}
	if b.PG != nil {
		errs = append(errs, b.PG.Close())
	}
	errs = append(errs, b.telemetry.Shutdown(ctx))
	errs = append(errs, b.lock.Unlock())
	return errors.Join(errs...)
}
