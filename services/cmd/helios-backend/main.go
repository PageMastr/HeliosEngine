// Command helios-backend runs the Helios backend services in one process (05 §5, ADR-014).
// On a developer machine it needs nothing installed: PostgreSQL runs embedded (binaries are
// downloaded and cached on first run), NATS with JetStream runs in-process and miniredis stands
// in for Valkey. Flags point it at external PostgreSQL/NATS/Valkey for production-like runs.
//
//	helios-backend [run] [flags]      run the backend (default)
//	helios-backend migrate [flags]    apply database migrations and exit
//	helios-backend keys rotate [netcode|tickets|jwt|nats] [flags]
//	                                  add a key generation (gateways drain onto it, 05 §1.3)
//	helios-backend reset --yes [flags]  delete the data directory
//	helios-backend version
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"runtime/debug"
	"syscall"
	"time"

	"github.com/PageMastr/scifi-test/services/internal/backend"
	"github.com/PageMastr/scifi-test/services/internal/identity"
	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/internal/stack"
	"github.com/PageMastr/scifi-test/services/migrations"
	"github.com/PageMastr/scifi-test/services/pkg/keyring"
)

// version is set with -ldflags "-X main.version=..." by release builds.
var version = "dev"

func main() {
	os.Exit(run(os.Args[1:], os.Getenv, os.Stderr))
}

func buildVersion() string {
	v := version
	if info, ok := debug.ReadBuildInfo(); ok {
		for _, s := range info.Settings {
			if s.Key == "vcs.revision" && len(s.Value) >= 12 {
				v += "+" + s.Value[:12]
			}
		}
	}
	return v
}

func run(args []string, getenv func(string) string, stderr io.Writer) int {
	cmd := "run"
	if len(args) > 0 && len(args[0]) > 0 && args[0][0] != '-' {
		cmd, args = args[0], args[1:]
	}
	switch cmd {
	case "version":
		fmt.Fprintln(stderr, "helios-backend", buildVersion())
		return 0
	case "help":
		usage(stderr)
		return 0
	case "run", "migrate", "keys", "reset":
	default:
		fmt.Fprintf(stderr, "unknown command %q\n\n", cmd)
		usage(stderr)
		return 2
	}

	var sub []string
	if cmd == "keys" {
		// keys rotate [netcode|tickets|jwt]
		for len(args) > 0 && len(args[0]) > 0 && args[0][0] != '-' {
			sub, args = append(sub, args[0]), args[1:]
		}
	}
	yes := false
	if cmd == "reset" {
		for i := 0; i < len(args); i++ {
			if args[i] == "--yes" || args[i] == "-yes" {
				yes = true
				args = append(args[:i], args[i+1:]...)
				i--
			}
		}
	}
	cfg, rest, err := platform.Load("helios-backend "+cmd, args, getenv, stderr)
	if errors.Is(err, flag.ErrHelp) {
		return 0
	}
	if err != nil {
		fmt.Fprintln(stderr, "config:", err)
		return 2
	}
	if len(rest) > 0 {
		fmt.Fprintf(stderr, "unexpected arguments: %v\n", rest)
		return 2
	}
	log := platform.NewLogger(cfg.Log, cfg.Shard, stderr)
	slog.SetDefault(log)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	switch cmd {
	case "migrate":
		return runMigrate(ctx, cfg, log)
	case "keys":
		return runKeys(cfg, sub, log, stderr)
	case "reset":
		return runReset(cfg, yes, log, stderr)
	}
	return runBackend(ctx, cfg, log)
}

func usage(w io.Writer) {
	fmt.Fprint(w, `usage: helios-backend [command] [flags]

commands:
  run                      run the backend (default)
  migrate                  apply database migrations and exit
  keys rotate [netcode|tickets|jwt|nats]
                           add a key generation (default netcode)
  reset --yes              delete the data directory
  version                  print the version

Run "helios-backend run -h" for flags. Every flag also has a HELIOS_* environment variable and
a key in <data>/helios.toml (see services/README.md).
`)
}

func runBackend(ctx context.Context, cfg *platform.Config, log *slog.Logger) int {
	start := time.Now()
	log.Info("helios-backend starting", "version", buildVersion(), "data", cfg.DataDir, "services", cfg.Services,
		"db", redact(cfg.DB.URL), "bus", redact(cfg.Bus.URL), "cache", redact(cfg.Cache.URL), "config", cfg.ConfigFile)
	b, err := backend.Start(ctx, cfg, log, backend.Options{Version: buildVersion()})
	if err != nil {
		log.Error("startup failed", "err", err)
		return 1
	}
	log.Info("helios-backend ready", "api", b.API.URL(), "ops", b.Ops.URL(), "nats", redact(b.Bus.ClientURL),
		"took", time.Since(start).Round(time.Millisecond))
	<-ctx.Done()
	log.Info("shutting down")
	sctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	if err := b.Shutdown(sctx); err != nil {
		log.Error("shutdown", "err", err)
		return 1
	}
	log.Info("bye")
	return 0
}

func runMigrate(ctx context.Context, cfg *platform.Config, log *slog.Logger) int {
	lock, err := stack.LockDataDir(cfg.DataDir)
	if err != nil {
		log.Error("migrate", "err", err)
		return 1
	}
	defer lock.Unlock()
	pg, err := stack.OpenPostgres(ctx, cfg, log)
	if err != nil {
		log.Error("database", "err", err)
		return 1
	}
	defer pg.Close()
	db := pg.SQLDB()
	defer db.Close()
	// The PII keys are loaded only if pre-WP-0.15r plain-text rows must be encrypted.
	piiKeys := func() (*identity.PIIKeys, error) { return backend.OpenPIIKeys(ctx, cfg, log, pg.Pool) }
	if _, err := migrations.Up(ctx, db, log, migrations.Options{PIIKeys: piiKeys}); err != nil {
		log.Error("migrate", "err", err)
		return 1
	}
	return 0
}

func runKeys(cfg *platform.Config, sub []string, log *slog.Logger, stderr io.Writer) int {
	if len(sub) == 0 || sub[0] != "rotate" || len(sub) > 2 {
		fmt.Fprintln(stderr, "usage: helios-backend keys rotate [netcode|tickets|jwt|nats]")
		return 2
	}
	which := "netcode"
	if len(sub) == 2 {
		which = sub[1]
	}
	files := map[string]string{"netcode": backend.KeyFileNetcode, "tickets": backend.KeyFileTickets, "jwt": backend.KeyFileJWT,
		"nats": backend.KeyFileNATS}
	name, ok := files[which]
	if !ok {
		fmt.Fprintf(stderr, "unknown key set %q (netcode, tickets, jwt, nats)\n", which)
		return 2
	}
	path := backend.KeyPath(cfg, name)
	ring, _, err := keyring.LoadOrCreate(path, which, nil, time.Now())
	if err != nil {
		log.Error("load keys", "err", err)
		return 1
	}
	e, err := ring.Add(nil, time.Now())
	if err == nil {
		ring.Prune(3) // current + two previous generations still verify
		err = ring.Save(path)
	}
	if err != nil {
		log.Error("rotate", "err", err)
		return 1
	}
	log.Info("key generation added; restart helios-backend to use it", "set", which, "id", e.ID, "file", path)
	return 0
}

func runReset(cfg *platform.Config, yes bool, log *slog.Logger, stderr io.Writer) int {
	if !yes {
		fmt.Fprintf(stderr, "this deletes %s (database, bus state, keys); re-run with --yes\n", cfg.DataDir)
		return 2
	}
	abs, _ := filepath.Abs(cfg.DataDir)
	if _, err := os.Stat(cfg.DataDir); errors.Is(err, os.ErrNotExist) {
		log.Info("nothing to reset", "path", abs)
		return 0
	}
	lock, err := stack.LockDataDir(cfg.DataDir)
	if err != nil {
		log.Error("reset", "err", err)
		return 1
	}
	_ = lock.Unlock() // released before deletion: Windows cannot delete an open file
	if err := os.RemoveAll(cfg.DataDir); err != nil {
		log.Error("reset", "err", err)
		return 1
	}
	log.Info("data directory removed", "path", abs)
	return 0
}

// redact hides credentials in URLs for logging.
func redact(s string) string {
	if u, err := url.Parse(s); err == nil && u.User != nil {
		return u.Redacted()
	}
	return s
}
