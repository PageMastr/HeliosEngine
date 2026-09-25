package migrations

import (
	"database/sql"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"strings"
	"testing"

	_ "github.com/jackc/pgx/v5/stdlib" // registers the "pgx" database/sql driver
)

// TestMigrationFiles checks the embedded migrations without a database: every schema has
// sequentially numbered goose files with Up and Down sections and balanced statement blocks.
// Applying them against PostgreSQL is covered by the integration build.
func TestMigrationFiles(t *testing.T) {
	for _, schema := range Schemas {
		entries, err := fs.ReadDir(FS, schema)
		if err != nil || len(entries) == 0 {
			t.Fatalf("%s: no migrations (%v)", schema, err)
		}
		for i, e := range entries {
			want := fmt.Sprintf("%05d_", i+1)
			if !strings.HasPrefix(e.Name(), want) || !strings.HasSuffix(e.Name(), ".sql") {
				t.Errorf("%s/%s: expected prefix %s and .sql suffix", schema, e.Name(), want)
			}
			b, _ := fs.ReadFile(FS, schema+"/"+e.Name())
			s := string(b)
			if !strings.Contains(s, "-- +goose Up") || !strings.Contains(s, "-- +goose Down") {
				t.Errorf("%s/%s: missing goose Up/Down", schema, e.Name())
			}
			if strings.Count(s, "-- +goose StatementBegin") != strings.Count(s, "-- +goose StatementEnd") {
				t.Errorf("%s/%s: unbalanced StatementBegin/End", schema, e.Name())
			}
			if strings.Contains(strings.ToLower(s), "create table ") && !strings.Contains(s, "CREATE TABLE "+schema+".") {
				t.Errorf("%s/%s: tables must be schema-qualified", schema, e.Name())
			}
		}

		// goose can build a provider (parses file names, versions) without connecting.
		db, err := sql.Open("pgx", "postgres://nobody@127.0.0.1:1/none")
		if err != nil {
			t.Fatal(err)
		}
		p, err := provider(db, schema, slog.New(slog.NewTextHandler(io.Discard, nil)))
		if err != nil {
			t.Fatalf("%s: provider: %v", schema, err)
		}
		if n := len(p.ListSources()); n != len(entries) {
			t.Fatalf("%s: goose sees %d sources, want %d", schema, n, len(entries))
		}
		_ = db.Close()
	}
}
