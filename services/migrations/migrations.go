// Package migrations embeds the goose SQL migrations, one directory per service schema
// (05 §3.3), and applies them under a PostgreSQL advisory lock so concurrent starters (several
// replicas, or a dev backend restarted quickly) never race.
package migrations

import (
	"context"
	"database/sql"
	"embed"
	"fmt"
	"io/fs"
	"log/slog"

	"github.com/pressly/goose/v3"
	"github.com/pressly/goose/v3/lock"
)

//go:embed identity/*.sql orchestrator/*.sql
var FS embed.FS

// Schemas lists the service schemas in apply order. Each has its own goose version table
// (<schema>.goose_db_version) so services can later migrate independently.
var Schemas = []string{"identity", "orchestrator"}

// Result summarises one schema's run.
type Result struct {
	Schema  string
	Applied int
	Version int64
}

// Up creates each schema if needed and applies its pending migrations.
func Up(ctx context.Context, db *sql.DB, log *slog.Logger) ([]Result, error) {
	if log == nil {
		log = slog.Default()
	}
	var out []Result
	for _, schema := range Schemas {
		if _, err := db.ExecContext(ctx, "CREATE SCHEMA IF NOT EXISTS "+schema); err != nil {
			return out, fmt.Errorf("migrations: create schema %s: %w", schema, err)
		}
		p, err := provider(db, schema, log)
		if err != nil {
			return out, err
		}
		results, err := p.Up(ctx)
		if err != nil {
			return out, fmt.Errorf("migrations: %s: %w", schema, err)
		}
		v, err := p.GetDBVersion(ctx)
		if err != nil {
			return out, fmt.Errorf("migrations: %s version: %w", schema, err)
		}
		out = append(out, Result{Schema: schema, Applied: len(results), Version: v})
		log.Info("migrations applied", "schema", schema, "applied", len(results), "version", v)
	}
	return out, nil
}

func provider(db *sql.DB, schema string, log *slog.Logger) (*goose.Provider, error) {
	sub, err := fs.Sub(FS, schema)
	if err != nil {
		return nil, err
	}
	locker, err := lock.NewPostgresSessionLocker()
	if err != nil {
		return nil, err
	}
	return goose.NewProvider(goose.DialectPostgres, db, sub,
		goose.WithTableName(schema+".goose_db_version"),
		goose.WithSessionLocker(locker),
		goose.WithDisableGlobalRegistry(true),
		goose.WithSlog(log.With("component", "migrations", "schema", schema)),
	)
}
