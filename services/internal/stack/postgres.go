// Package stack brings up the infrastructure helios-backend runs on: PostgreSQL (embedded via
// fergusstrange/embedded-postgres, or an external server), NATS with JetStream (embedded
// nats-server, or an external cluster) and Valkey (miniredis in-process, or an external
// server). The same service code runs over both (ADR-014, 05 §0 principle 4).
package stack

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"

	embeddedpostgres "github.com/fergusstrange/embedded-postgres"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/jackc/pgx/v5/stdlib"

	"github.com/PageMastr/scifi-test/services/internal/platform"
)

// PGVersion is the embedded PostgreSQL version (05 §3.5: PostgreSQL 18).
const PGVersion = embeddedpostgres.V18

// Postgres is an open connection pool, plus the embedded server when one was started.
type Postgres struct {
	URL      string
	Pool     *pgxpool.Pool
	embedded *embeddedpostgres.EmbeddedPostgres
	log      *slog.Logger
}

// ErrRunningAsRoot explains why embedded PostgreSQL cannot start.
var ErrRunningAsRoot = errors.New("embedded PostgreSQL refuses to run as root; run helios-backend as a normal user or pass --db postgres://...")

// PGPaths are the on-disk locations of the embedded server.
type PGPaths struct {
	Data     string // cluster data directory (persistent)
	Runtime  string // scratch directory the library recreates on every start
	Binaries string // extracted binaries, shared by all data dirs of this version
	Cache    string // downloaded archives
	Log      string // server log file
}

// EmbeddedPaths derives the embedded server's directories from the config.
func EmbeddedPaths(cfg *platform.Config) PGPaths {
	cache := cfg.PGCacheDir()
	return PGPaths{
		Data:     filepath.Join(cfg.DataDir, "pg"),
		Runtime:  filepath.Join(cfg.DataDir, "pg-runtime"),
		Binaries: filepath.Join(cache, string(PGVersion), runtime.GOOS+"-"+runtime.GOARCH),
		Cache:    cache,
		Log:      filepath.Join(cfg.DataDir, "logs", "postgres.log"),
	}
}

// OpenPostgres starts embedded PostgreSQL when cfg.DB.URL is "embedded" (first run downloads
// the binaries from Maven Central into the cache), then opens a pgx pool.
func OpenPostgres(ctx context.Context, cfg *platform.Config, log *slog.Logger) (*Postgres, error) {
	p := &Postgres{URL: cfg.DB.URL, log: log}
	if cfg.DB.URL == "embedded" {
		if err := p.startEmbedded(cfg); err != nil {
			return nil, err
		}
	}
	pcfg, err := pgxpool.ParseConfig(p.URL)
	if err != nil {
		p.stopEmbedded()
		return nil, fmt.Errorf("postgres url: %w", err)
	}
	if cfg.DB.MaxConns > 0 {
		pcfg.MaxConns = int32(cfg.DB.MaxConns)
	}
	pool, err := pgxpool.NewWithConfig(ctx, pcfg)
	if err != nil {
		p.stopEmbedded()
		return nil, err
	}
	// External servers may still be starting (compose): retry the first ping for a while.
	deadline := time.Now().Add(30 * time.Second)
	for {
		pctx, cancel := context.WithTimeout(ctx, 3*time.Second)
		err = pool.Ping(pctx)
		cancel()
		if err == nil {
			break
		}
		if time.Now().After(deadline) || ctx.Err() != nil {
			pool.Close()
			p.stopEmbedded()
			return nil, fmt.Errorf("postgres not reachable: %w", err)
		}
		time.Sleep(500 * time.Millisecond)
	}
	p.Pool = pool
	return p, nil
}

func (p *Postgres) startEmbedded(cfg *platform.Config) error {
	if isRoot() {
		return ErrRunningAsRoot
	}
	paths := EmbeddedPaths(cfg)
	for _, d := range []string{paths.Cache, paths.Binaries, filepath.Dir(paths.Log), filepath.Dir(paths.Data)} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
	if err := stopStalePostmaster(paths, p.log); err != nil {
		return err
	}
	logFile, err := os.OpenFile(paths.Log, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return err
	}
	_, statErr := os.Stat(filepath.Join(paths.Binaries, "bin", pgExe("pg_ctl")))
	if statErr != nil {
		p.log.Info("preparing embedded PostgreSQL binaries (first run downloads them from Maven Central)",
			"version", string(PGVersion), "cache", paths.Cache)
	}
	const user, password, db = "helios", "helios", "helios"
	ep := embeddedpostgres.NewDatabase(embeddedpostgres.DefaultConfig().
		Version(PGVersion).
		Port(uint32(cfg.DB.Port)).
		Username(user).Password(password).Database(db).
		DataPath(paths.Data).
		RuntimePath(paths.Runtime).
		BinariesPath(paths.Binaries).
		CachePath(paths.Cache).
		Locale("C").Encoding("UTF8").
		StartTimeout(90 * time.Second).
		StartParameters(map[string]string{"listen_addresses": "127.0.0.1"}).
		Logger(logFile))
	start := time.Now()
	if err := ep.Start(); err != nil {
		_ = logFile.Close()
		return fmt.Errorf("embedded postgres (log: %s): %w", paths.Log, err)
	}
	p.embedded = ep
	markExtracted(paths)
	p.URL = fmt.Sprintf("postgres://%s:%s@127.0.0.1:%d/%s?sslmode=disable", user, password, cfg.DB.Port, db)
	p.log.Info("embedded PostgreSQL started", "port", cfg.DB.Port, "data", paths.Data, "took", time.Since(start).Round(time.Millisecond))
	return nil
}

// stopStalePostmaster handles a postmaster.pid left by a crashed backend (05 §3.5): if that
// server is still running it is stopped with pg_ctl; a dead PID is left for PostgreSQL, which
// removes its own stale lock file on start.
func stopStalePostmaster(paths PGPaths, log *slog.Logger) error {
	b, err := os.ReadFile(filepath.Join(paths.Data, "postmaster.pid"))
	if err != nil {
		return nil
	}
	pid, err := strconv.Atoi(strings.TrimSpace(strings.SplitN(string(b), "\n", 2)[0]))
	if err != nil || pid <= 0 || !processAlive(pid) {
		return nil
	}
	pgctl := filepath.Join(paths.Binaries, "bin", pgExe("pg_ctl"))
	log.Warn("stopping PostgreSQL left running by a previous helios-backend", "pid", pid)
	out, err := exec.Command(pgctl, "stop", "-D", paths.Data, "-m", "fast", "-w").CombinedOutput()
	if err != nil {
		return fmt.Errorf("stale PostgreSQL (pid %d) holds %s and pg_ctl stop failed: %v: %s", pid, paths.Data, err, out)
	}
	return nil
}

// markExtracted works around embedded-postgres probing for "bin/pg_ctl" without the .exe
// suffix: on Windows it would otherwise re-extract the archive on every start (slow, and it
// fails while another instance runs those binaries). exec resolves "pg_ctl" to pg_ctl.exe, so
// an empty marker file is harmless.
func markExtracted(paths PGPaths) {
	if runtime.GOOS != "windows" {
		return
	}
	marker := filepath.Join(paths.Binaries, "bin", "pg_ctl")
	if _, err := os.Stat(marker + ".exe"); err == nil {
		if _, err := os.Stat(marker); errors.Is(err, os.ErrNotExist) {
			_ = os.WriteFile(marker, nil, 0o644)
		}
	}
}

func pgExe(name string) string {
	if runtime.GOOS == "windows" {
		return name + ".exe"
	}
	return name
}

// SQLDB returns a database/sql handle over the pool (goose needs one).
func (p *Postgres) SQLDB() *sql.DB { return stdlib.OpenDBFromPool(p.Pool) }

// Embedded reports whether this process owns the server.
func (p *Postgres) Embedded() bool { return p.embedded != nil }

// Close closes the pool and stops the embedded server.
func (p *Postgres) Close() error {
	if p.Pool != nil {
		p.Pool.Close()
	}
	return p.stopEmbedded()
}

func (p *Postgres) stopEmbedded() error {
	if p.embedded == nil {
		return nil
	}
	err := p.embedded.Stop()
	p.embedded = nil
	if err == nil {
		p.log.Info("embedded PostgreSQL stopped")
	}
	return err
}
