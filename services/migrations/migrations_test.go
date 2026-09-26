package migrations

import (
	"database/sql"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"regexp"
	"slices"
	"strings"
	"testing"

	_ "github.com/jackc/pgx/v5/stdlib" // registers the "pgx" database/sql driver
)

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

func offlineDB(t *testing.T) *sql.DB {
	t.Helper()
	// goose can build a provider (parses file names, versions) without connecting.
	db, err := sql.Open("pgx", "postgres://nobody@127.0.0.1:1/none")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	return db
}

// qualified matches "<schema>." as a whole identifier, so "identity." does not match inside
// "svc_identity.".
func qualified(schema string) *regexp.Regexp {
	return regexp.MustCompile(`(^|[^A-Za-z0-9_])` + regexp.QuoteMeta(schema) + `\.`)
}

var createRE = regexp.MustCompile(`(?i)create\s+(table|schema|sequence|function|index\s+\S+\s+on)\s+(if\s+not\s+exists\s+)?([A-Za-z0-9_]+)\.`)

// TestMigrationFiles checks the embedded migrations without a database: every schema is named
// svc_<service> (05 §1.4, §3; CONF-06); SQL files and Go migrations together number 1..n; every
// file has Up and Down sections and balanced statement blocks; files up to LegacyVersion are
// written against the legacy name and every later one only against svc_<service>, with every
// object created in its own schema. Applying them against PostgreSQL, including the upgrade of a
// pre-WP-0.15r database, is covered by the integration build.
func TestMigrationFiles(t *testing.T) {
	for _, s := range Schemas {
		if !strings.HasPrefix(s.Name, "svc_") || s.Legacy == s.Name {
			t.Errorf("schema %q must be named svc_<service> (05 §3)", s.Name)
		}
		entries, err := fs.ReadDir(FS, s.Dir)
		if err != nil || len(entries) == 0 {
			t.Fatalf("%s: no migrations (%v)", s.Dir, err)
		}
		sqlVersions, err := versions(s)
		if err != nil {
			t.Fatal(err)
		}
		all := slices.Clone(sqlVersions)
		for _, m := range goMigrations(s, Options{}, quiet) {
			if slices.Contains(all, m.Version) {
				t.Fatalf("%s: Go migration %d collides with a SQL file", s.Dir, m.Version)
			}
			all = append(all, m.Version)
		}
		slices.Sort(all)
		for i, v := range all {
			if v != int64(i+1) {
				t.Fatalf("%s: versions %v are not 1..n", s.Dir, all)
			}
		}
		for i, e := range entries {
			name := s.Dir + "/" + e.Name()
			if !strings.HasPrefix(e.Name(), fmt.Sprintf("%05d_", sqlVersions[i])) || !strings.HasSuffix(e.Name(), ".sql") {
				t.Errorf("%s: expected a 5-digit version prefix and .sql suffix", name)
			}
			b, _ := fs.ReadFile(FS, name)
			text := string(b)
			if !strings.Contains(text, "-- +goose Up") || !strings.Contains(text, "-- +goose Down") {
				t.Errorf("%s: missing goose Up/Down", name)
			}
			if strings.Count(text, "-- +goose StatementBegin") != strings.Count(text, "-- +goose StatementEnd") {
				t.Errorf("%s: unbalanced StatementBegin/End", name)
			}
			want := s.Name
			if sqlVersions[i] <= s.LegacyVersion {
				want = s.Legacy // historical files keep the statements they were applied with
			} else if qualified(s.Legacy).MatchString(stripComments(text)) {
				t.Errorf("%s: names the legacy schema %s; use %s", name, s.Legacy, s.Name)
			}
			for _, m := range createRE.FindAllStringSubmatch(stripComments(text), -1) {
				if m[3] != want {
					t.Errorf("%s: %q creates an object outside schema %s", name, strings.TrimSpace(m[0]), want)
				}
			}
			if strings.Contains(strings.ToLower(text), "create table ") && !qualified(want).MatchString(text) {
				t.Errorf("%s: tables must be schema-qualified", name)
			}
		}

		p, err := newProvider(offlineDB(t), s, Options{}, quiet)
		if err != nil {
			t.Fatalf("%s: provider: %v", s.Name, err)
		}
		if n := len(p.ListSources()); n != len(all) {
			t.Fatalf("%s: goose sees %d sources, want %d", s.Name, n, len(all))
		}
		// The legacy provider sees exactly the files written against the old name.
		lp, err := legacyProvider(offlineDB(t), s, quiet)
		if err != nil {
			t.Fatalf("%s: legacy provider: %v", s.Name, err)
		}
		var legacy []int64
		for _, src := range lp.ListSources() {
			legacy = append(legacy, src.Version)
		}
		if len(legacy) != int(s.LegacyVersion) || legacy[len(legacy)-1] != s.LegacyVersion {
			t.Fatalf("%s: legacy provider sees %v, want 1..%d", s.Name, legacy, s.LegacyVersion)
		}
	}
}

func stripComments(sqlText string) string {
	var b strings.Builder
	for _, line := range strings.Split(sqlText, "\n") {
		if i := strings.Index(line, "--"); i >= 0 {
			line = line[:i]
		}
		b.WriteString(line)
		b.WriteByte('\n')
	}
	return b.String()
}

func TestQualifiedMatchesWholeSchemaNames(t *testing.T) {
	re := qualified("identity")
	for text, want := range map[string]bool{
		"CREATE TABLE identity.account": true, "(identity.account)": true, "identity.x": true,
		"svc_identity.account": false, "myidentity.x": false, "identity_x.y": false,
	} {
		if re.MatchString(text) != want {
			t.Errorf("%q: want %v", text, want)
		}
	}
}
