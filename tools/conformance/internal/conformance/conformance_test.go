package conformance

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// fixture is one seeded tree under testdata/<rule or "framework">/<case>/. Its expect.txt lists the
// failing findings the lint must report, one `<path>[:<line>]: <rule>` per line and nothing else; an
// optional first line `#! <args>` gives the CLI arguments (the CTest registration reads it too).
type fixture struct {
	dir  string
	opts Options
	want []string
}

func loadFixture(t *testing.T, dir string) fixture {
	data, err := os.ReadFile(filepath.Join(dir, "expect.txt"))
	if err != nil {
		t.Fatal(err)
	}
	fx := fixture{dir: dir, opts: Options{Root: dir}}
	for _, line := range strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n") {
		switch {
		case strings.HasPrefix(line, "#!"):
			args := strings.Fields(line[2:])
			for i := 0; i < len(args); i++ {
				switch args[i] {
				case "-strict":
					fx.opts.Strict = true
				case "-rules":
					i++
					fx.opts.Rules = strings.Split(args[i], ",")
				default:
					t.Fatalf("%s: unknown fixture argument %q", dir, args[i])
				}
			}
		case line == "" || strings.HasPrefix(line, "#"):
		default:
			fx.want = append(fx.want, line)
		}
	}
	return fx
}

// TestFixtures runs every seeded tree and compares the failing findings exactly: a rule that stops
// matching, or that starts to match more, fails its fixture.
func TestFixtures(t *testing.T) {
	dirs, err := filepath.Glob(filepath.Join("..", "..", "testdata", "*", "*"))
	if err != nil || len(dirs) == 0 {
		t.Fatalf("no fixtures (%v)", err)
	}
	seen := map[string]bool{}
	for _, dir := range dirs {
		fx := loadFixture(t, dir)
		name := filepath.Base(filepath.Dir(dir)) + "/" + filepath.Base(dir)
		if strings.HasSuffix(name, "/bad") {
			seen[filepath.Base(filepath.Dir(dir))] = true
		}
		t.Run(name, func(t *testing.T) {
			res, err := Run(fx.opts)
			if err != nil {
				t.Fatal(err)
			}
			var got []string
			for _, f := range res.Findings {
				if f.Failing() {
					got = append(got, f.where()+": "+f.Rule)
				}
			}
			if strings.Join(got, "\n") != strings.Join(fx.want, "\n") {
				t.Errorf("findings differ from expect.txt\ngot:\n  %s\nwant:\n  %s", strings.Join(got, "\n  "),
					strings.Join(fx.want, "\n  "))
				var sb strings.Builder
				res.WriteText(&sb)
				t.Log(sb.String())
			}
			if strings.HasSuffix(name, "/bad") && len(fx.want) == 0 {
				t.Error("a bad fixture must expect at least one finding")
			}
		})
	}
	// §5.10.3: "Each rule ships with a seeded violation that must fail it."
	for _, r := range All() {
		if !seen[r.ID] {
			t.Errorf("%s has no testdata/%s/bad fixture", r.ID, r.ID)
		}
	}
}

func TestMatch(t *testing.T) {
	for _, c := range []struct {
		glob, path string
		want       bool
	}{
		{"services/**", "services/a/b.go", true},
		{"services/**", "servicesx/a.go", false},
		{"**/CMakeLists.txt", "CMakeLists.txt", true},
		{"**/CMakeLists.txt", "engine/core/CMakeLists.txt", true},
		{"tools/**/*.cmake", "tools/lint/isa_audit.cmake", true},
		{"tools/**/*.cmake", "tools/lint/README.md", false},
		{"engine/core/src/platform/*/cpu_gate_hook.c", "engine/core/src/platform/win32/cpu_gate_hook.c", true},
		{"third_party/*/patches/**", "third_party/luau/patches/0001-x.patch", true},
		{"third_party/*/patches/**", "third_party/luau/src/x.c", false},
	} {
		if got := Match(c.glob, c.path); got != c.want {
			t.Errorf("Match(%q, %q) = %v, want %v", c.glob, c.path, got, c.want)
		}
	}
}

func TestCodeLines(t *testing.T) {
	src := []string{
		`a(); // SDL_x`,
		`/* one`,
		`two */ b("str // not a comment");`,
		`auto r = R"x(raw "quote" )" still)x"; c('\'');`,
		`int n = 1'000'000; d();`,
	}
	want := []string{
		`a();         `,
		`      `,
		`       b("                    ");`,
		`auto r = R"                        "; c('  ');`,
		`int n = 1'000'000; d();`,
	}
	got := codeLines(src, true)
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("line %d: got %q, want %q", i+1, got[i], want[i])
		}
	}
	if kept := codeLines(src, false)[2]; kept != `       b("str // not a comment");` {
		t.Errorf("strings kept: got %q", kept)
	}
}

func TestStripJSONC(t *testing.T) {
	got := stripJSONC("{\"a\": \"x // y\", // c\n /* b\n */ \"b\": [1,],\n}")
	want := "{\"a\": \"x // y\", \n \n \"b\": [1]\n}"
	if got != want {
		t.Errorf("got %q, want %q", got, want)
	}
}

// TestSuppressionReasons pins what a suppression records: its reason, ending where its comment does.
func TestSuppressionReasons(t *testing.T) {
	root := filepath.Join("..", "..", "testdata", "framework", "suppression_ok")
	res, err := Run(Options{Root: root, Rules: []string{"CONF-10"}})
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, f := range res.Findings {
		got = append(got, f.where()+": "+f.Suppressed)
	}
	want := "engine/x/a.cpp:1: fixture: a reviewed exception\nengine/x/a.cpp:2: a block-comment suppression"
	if strings.Join(got, "\n") != want {
		t.Errorf("got:\n%s\nwant:\n%s", strings.Join(got, "\n"), want)
	}
}

func TestParenBody(t *testing.T) {
	for in, want := range map[string]string{
		"(a INT, b NUMERIC(10, 2)) PARTITION BY RANGE (a)": "a INT, b NUMERIC(10, 2)",
		"(a INT) WITH (fillfactor = 70)":                   "a INT",
		"(unterminated":                                    "unterminated",
	} {
		if got := parenBody(in); got != want {
			t.Errorf("parenBody(%q) = %q, want %q", in, got, want)
		}
	}
}

// TestWalkFiles pins the tree outside git: only the top-level build/ is build output.
func TestWalkFiles(t *testing.T) {
	root := t.TempDir()
	for _, f := range []string{"build/gen.cpp", "tools/build/a.cpp", "engine/x/build/b.cpp", ".git/c", "d.cpp"} {
		p := filepath.Join(root, filepath.FromSlash(f))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("x\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	got, err := walkFiles(root)
	if err != nil {
		t.Fatal(err)
	}
	if want := "d.cpp engine/x/build/b.cpp tools/build/a.cpp"; strings.Join(got, " ") != want {
		t.Errorf("got %q, want %q", strings.Join(got, " "), want)
	}
}

func TestSpliceLines(t *testing.T) {
	logical, origin := spliceLines([]string{`a \`, `b\ `, `c`, `d`, `e\`})
	if got := strings.Join(logical, "|"); got != "a bc|d|e\\" {
		t.Errorf("logical %q", got)
	}
	if got := fmt.Sprint(origin); got != "[0 3 4]" {
		t.Errorf("origin %s", got)
	}
}

func TestInactiveLines(t *testing.T) {
	code := []string{
		`#if 0`, `dead`, `# elif X`, `live`, `#else`, `live`, `#endif`,
		`#if 1`, `live`, `#elif 1`, `dead`, `#else`, `dead`, `#endif`,
		`#ifdef X`, `live`, `#  if (0)`, `dead`, `#  endif`, `#else`, `live`, `#endif`,
		`#if false`, `#if 1`, `dead`, `#else`, `dead`, `#endif`, `#endif`, `#endif`, `live`,
	}
	var got []string
	for i, dead := range inactiveLines(code) {
		if code[i] == "dead" || code[i] == "live" {
			got = append(got, map[bool]string{true: "dead", false: "live"}[dead])
		} else if dead {
			got = append(got, "directive marked dead: "+code[i])
		}
	}
	var want []string
	for _, l := range code {
		if l == "dead" || l == "live" {
			want = append(want, l)
		}
	}
	if strings.Join(got, " ") != strings.Join(want, " ") {
		t.Errorf("got  %v\nwant %v", got, want)
	}
}

func TestYamlCode(t *testing.T) {
	for in, want := range map[string]string{
		`run: x # c`:              `run: x `,
		`# all`:                   ``,
		`run: echo "a # b" # c`:   `run: echo "a # b" `,
		`url: http://x/#frag`:     `url: http://x/#frag`,
		`k: 'it''s # here' # end`: `k: 'it''s # here' `,
	} {
		if got := yamlCode(in); got != want {
			t.Errorf("yamlCode(%q) = %q, want %q", in, got, want)
		}
	}
}

// TestRepositoryMap pins the scopes 09 §5.10.4 (c) relies on: the C++ cell host and gateway
// (engine/server, apps/cellserver, apps/gateway; WP-0.14) are checked by CONF-01, 02 and 04 through the
// repository's map.
func TestRepositoryMap(t *testing.T) {
	m, bad := loadMap(filepath.Join("..", "..", "map.jsonc"), "tools/conformance/map.jsonc", All())
	for _, b := range bad {
		t.Errorf("map: %s:%d: %s", b.Path, b.Line, b.Message)
	}
	for _, id := range []string{"CONF-01", "CONF-02", "CONF-04"} {
		r := Lookup(id)
		scope := append([]string(nil), r.Scope...)
		for _, e := range m {
			for _, rid := range e.Rules {
				if rid == id {
					scope = append(scope, e.Paths...)
				}
			}
		}
		for _, f := range []string{"engine/server/src/ids.cpp", "apps/cellserver/main.cpp", "apps/gateway/main.cpp"} {
			if !r.covers(scope, f) {
				t.Errorf("%s does not read %s (09 §5.10.4 (c))", id, f)
			}
		}
	}
}
