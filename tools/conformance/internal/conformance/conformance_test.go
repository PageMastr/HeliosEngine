package conformance

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
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
		// CMake reads expect.txt as a list (CMakeLists.txt here), where a square bracket in any line, a
		// comment too, stops the lines after it from splitting: the CTest would lose its expectations.
		if strings.ContainsAny(line, "[]") {
			t.Fatalf("%s/expect.txt: %q has a square bracket, which CMake's list reading cannot take", dir, line)
		}
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

func TestParenSplit(t *testing.T) {
	for in, want := range map[string]string{
		"(a INT, b NUMERIC(10, 2)) PARTITION BY RANGE (a)": "a INT, b NUMERIC(10, 2)",
		"(a INT) WITH (fillfactor = 70)":                   "a INT",
		"(unterminated":                                    "unterminated",
	} {
		if got, _ := parenSplit(in); got != want {
			t.Errorf("parenSplit(%q) = %q, want %q", in, got, want)
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

// TestCConstStrings pins which C and C++ declarations count as string constants for CONF-01 and CONF-02:
// a name that is not one stays unresolved, so a bucket or key passed through it fails closed. Parameters
// with defaults (after a braced or lambda default, or an #if in the list), non-static data members (also
// of a class whose base clause or template parameters hold parentheses or `=`), a const inside template
// arguments and `#if 0` groups are not constants; a local in a lambda or function body is.
func TestCConstStrings(t *testing.T) {
	src := newCSource(strings.Split(`#define kDef "def"
namespace n { constexpr std::string_view kView{"view"}; }
static const char kArr[] = "arr";
const std::string kStr = "str";
std::string const kParen("paren");
const char* const kPtr = "ptr";
char const* const kPtr2 = "ptr2";
const char* const Foo::kQual = "qual";
class C { public: static constexpr const char* kMember = "member"; };
[[maybe_unused]] static constexpr const char* kAttr = "attr" "s";
constexpr std::string_view kBraced = {"braced"};
struct Desc { const char* name; };
void init(Desc& d, Desc* p) { d.name = "Cell"; p->bucket = "B"; }
void f(const char* const dflt = "x") { plain = "y"; HELIOS_LOG_INFO("z"); auto v = get("w"); }
const char* mutablePtr = "m";
static const char* kMutable = "km";
std::string mutableStr = "s";
const auto lookedUp = find("k");
void logTo(LogOptions opts = {}, const std::string& pname = "cell", int depth = 0);
void elect(kvStore* kv, std::function<void()> done = [] {}, const char* const pkey = "zone.1", int n = 2);
kvStore* openBucket(jsCtx* js,
#if defined(HELIOS_TRACE_KV)
                    Tracer* tracer,
#endif
                    const std::string& pbucket = "DIRECTORY", int history = 1);
void run() { go([&] { const std::string kLocal = "local"; }); }
struct Binding { const std::string subject = "orders"; static const char* const kSubj; };
struct alignas(8) [[nodiscard]] Holder { static constexpr const char* kHeld = "held"; const char* const inst = "i"; };
std::optional<const std::string> opt = "o";
std::span<const char> view = "v";
std::unique_ptr<const char* const> held = "h";
struct Wide : Base<(N > 1)> { const char* const wideInst = "wi"; };
class Derived final : public Bar<decltype(x)>, Baz<N == 2> { const char* const derivedInst = "di"; };
template <typename T = int> struct Tmpl { const char* const tmplInst = "ti"; static constexpr const char* kTmpl = "ts"; };
void g(struct Desc* d) { const char* const kInFn = "fn"; }
#if 0
#define kDeadDef "dd"
constexpr const char* kDead = "dead";
#endif
`, "\n"))
	var got []string
	for name, lits := range cConstStrings(src) {
		got = append(got, name+"="+strings.Join(lits, "|"))
	}
	sort.Strings(got)
	want := `kArr="arr" kAttr="attr" "s" kBraced="braced" kDef="def" kHeld="held" kInFn="fn" kLocal="local" ` +
		`kMember="member" kParen="paren" kPtr2="ptr2" kPtr="ptr" kQual="qual" kStr="str" kTmpl="ts" kView="view"`
	if strings.Join(got, " ") != want {
		t.Errorf("got  %s\nwant %s", strings.Join(got, " "), want)
	}
	table := map[string][]string{"bucket": {"DIRECTORY"}, "kView": {"view"}}
	for expr, want := range map[string]string{
		`n::kView`: "view", `::n::kView`: "view", `std::string(kView).c_str()`: "view", `"a" "b"`: "ab",
		`o.bucket`: "unresolved", `p->bucket`: "unresolved", `bucket`: "DIRECTORY", `name`: "unresolved",
	} {
		vals, ok := cStrValues(expr, table)
		if got := map[bool]string{true: strings.Join(vals, "|"), false: "unresolved"}[ok]; got != want {
			t.Errorf("cStrValues(%s) = %s, want %s", expr, got, want)
		}
	}
}

// TestCEval pins CONF-04's C++ constant evaluation: every definition of a reused name counts, and only
// parentheses that enclose the whole expression are stripped.
func TestCEval(t *testing.T) {
	table := map[string][]string{"kA": {"17"}, "kB": {"5"}, "kPage": {"6", "12"}, "kSum": {"(kA) + (kB)"},
		"kNested": {"(kA + (kB)) + 0u"}}
	for expr, want := range map[string]string{
		`kA + kB`: "[22]", `(kA) + (kB)`: "[22]", `kSum`: "[22]", `ns::kSum`: "[22]", `kNested`: "[22]",
		`kPage`: "[6 12]", `kPage + kB`: "[11 17]", `((kA))`: "[17]", `kA) + (kB`: "[]", `kMissing + 1`: "[]",
	} {
		if got := fmt.Sprint(cEval(expr, table, 0)); got != want {
			t.Errorf("cEval(%s) = %s, want %s", expr, got, want)
		}
	}
	// Reused names multiply the combinations (here 21 x 14, nearly all distinct): a 22 that comes last
	// must not be lost to a bound on how many values one expression keeps.
	for i := 1; i <= 20; i++ {
		table["kWide"] = append(table["kWide"], strconv.Itoa(1000*i))
	}
	table["kWide"] = append(table["kWide"], "0")
	for i := 30; i <= 42; i++ {
		table["kNarrow"] = append(table["kNarrow"], strconv.Itoa(i))
	}
	table["kNarrow"] = append(table["kNarrow"], "22")
	if got := cEval("kWide + kNarrow", table, 0); !slices.Contains(got, 22) || len(got) != 14 {
		t.Errorf("cEval(kWide + kNarrow) = %v, want the 14 values below 64, 22 among them", got)
	}
	if got := cEval("kWide", table, 0); fmt.Sprint(got) != "[0]" {
		t.Errorf("cEval(kWide) = %v, want [0]: values of 64 and up are not shift amounts", got)
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
// repository's map. CONF-01's scope covers CONF-02's: a nats.c TTL is attributed to the buckets its file
// sets, and a lease or unresolved Bucket set where CONF-02 reads is left to CONF-01 (README, limits).
func TestRepositoryMap(t *testing.T) {
	m, bad := loadMap(filepath.Join("..", "..", "map.jsonc"), "tools/conformance/map.jsonc", All())
	for _, b := range bad {
		t.Errorf("map: %s:%d: %s", b.Path, b.Line, b.Message)
	}
	scopeOf := func(id string) []string {
		scope := append([]string(nil), Lookup(id).Scope...)
		for _, e := range m {
			for _, rid := range e.Rules {
				if rid == id {
					scope = append(scope, e.Paths...)
				}
			}
		}
		return scope
	}
	for _, id := range []string{"CONF-01", "CONF-02", "CONF-04"} {
		for _, f := range []string{"engine/server/src/ids.cpp", "apps/cellserver/main.cpp", "apps/gateway/main.cpp"} {
			if !Lookup(id).covers(scopeOf(id), f) {
				t.Errorf("%s does not read %s (09 §5.10.4 (c))", id, f)
			}
		}
	}
	for _, glob := range scopeOf("CONF-02") {
		f := strings.Replace(glob, "**", "src/kv.cpp", 1)
		if !Lookup("CONF-01").covers(scopeOf("CONF-01"), f) {
			t.Errorf("CONF-02 reads %s and CONF-01 does not", f)
		}
	}
}

// TestSARIF pins how findings reach code scanning: a failing finding is an error; a suppressed or
// known-failing one is a note with its suppression (code scanning ignores SARIF suppressions, so an
// error there would fail a PR that touches a reviewed line), and each carries its fingerprint.
func TestSARIF(t *testing.T) {
	type result struct {
		RuleID       string `json:"ruleId"`
		Level        string `json:"level"`
		Suppressions []struct {
			Kind string `json:"kind"`
		} `json:"suppressions"`
		PartialFingerprints map[string]string `json:"partialFingerprints"`
	}
	for _, c := range []struct{ fixture, want string }{
		{"suppression_ok", "note:inSource note:inSource"},
		{"known_ok", "note:external note:external"},
		{"known_scope", "note:external error:"},
	} {
		res, err := Run(Options{Root: filepath.Join("..", "..", "testdata", "framework", c.fixture), Rules: []string{"CONF-10"}})
		if err != nil {
			t.Fatal(err)
		}
		var sb strings.Builder
		if err := res.WriteSARIF(&sb); err != nil {
			t.Fatal(err)
		}
		var doc struct {
			Runs []struct {
				Results []result `json:"results"`
			} `json:"runs"`
		}
		if err := json.Unmarshal([]byte(sb.String()), &doc); err != nil {
			t.Fatal(err)
		}
		var got []string
		for _, r := range doc.Runs[0].Results {
			kind := ""
			if len(r.Suppressions) > 0 {
				kind = r.Suppressions[0].Kind
			}
			got = append(got, r.Level+":"+kind)
			if !fingerprintRE.MatchString(r.PartialFingerprints["heliosConformance/v1"]) {
				t.Errorf("%s: result without a fingerprint: %+v", c.fixture, r)
			}
		}
		if strings.Join(got, " ") != c.want {
			t.Errorf("%s: got %q, want %q", c.fixture, strings.Join(got, " "), c.want)
		}
	}
}

// TestSQLStatementsGooseAndLexing: the splitter skips what goose and PostgreSQL skip (goose's
// annotation spellings, dollar tags with digits, E-string escapes, multi-line strings, nested comments,
// quoted identifiers, '$' inside identifiers), so DDL that never runs cannot change the net schema.
func TestSQLStatementsGooseAndLexing(t *testing.T) {
	for _, c := range []struct {
		src  string
		want []string
	}{
		{"-- +goose Up\nCREATE TABLE a (x INT);\n-- +goose down\nDROP TABLE a;\n", []string{"CREATE TABLE a (x INT)"}},
		{"-- +goose up\nCREATE TABLE a (x INT);\n-- +goose DOWN\nDROP TABLE a;\n", []string{"CREATE TABLE a (x INT)"}},
		{"--+goose Up\nCREATE TABLE a (x INT);\n--+goose Down\nDROP TABLE a;\n", []string{"CREATE TABLE a (x INT)"}},
		{"-- +goose Up\n-- +goose StatementBegin\nCREATE TABLE a (x INT);\n-- +goose StatementEnd\n",
			[]string{"CREATE TABLE a (x INT)"}},
		{"CREATE FUNCTION f() RETURNS void AS $fn1$ BEGIN PERFORM 1; DROP TABLE a; END $fn1$ LANGUAGE plpgsql;",
			[]string{"CREATE FUNCTION f() RETURNS void AS $fn1$$fn1$ LANGUAGE plpgsql"}},
		{"INSERT INTO a VALUES (E'it\\'s');\nALTER TABLE a ADD COLUMN email TEXT;",
			[]string{"INSERT INTO a VALUES (E'')", "ALTER TABLE a ADD COLUMN email TEXT"}},
		{"INSERT INTO a VALUES (e'\\\\');\nALTER TABLE a ADD COLUMN email TEXT;",
			[]string{"INSERT INTO a VALUES (e'')", "ALTER TABLE a ADD COLUMN email TEXT"}},
		// A plain string ends at a backslash-quote; only E strings take backslash escapes.
		{"INSERT INTO a VALUES ('x\\'); ALTER TABLE a ADD COLUMN email TEXT;",
			[]string{"INSERT INTO a VALUES ('')", "ALTER TABLE a ADD COLUMN email TEXT"}},
		{"INSERT INTO a VALUES ('first\n'); ALTER TABLE a ADD COLUMN email TEXT;",
			[]string{"INSERT INTO a VALUES ('')", "ALTER TABLE a ADD COLUMN email TEXT"}},
		{"INSERT INTO a VALUES ('a;\nb;'); DROP TABLE a;", []string{"INSERT INTO a VALUES ('')", "DROP TABLE a"}},
		{"/* outer /* inner */ it's a comment */ ALTER TABLE a ADD COLUMN email TEXT;",
			[]string{"ALTER TABLE a ADD COLUMN email TEXT"}},
		{"ALTER/* x */TABLE a ADD COLUMN email TEXT;", []string{"ALTER TABLE a ADD COLUMN email TEXT"}},
		{`ALTER TABLE a ADD COLUMN "it's;" TEXT, ADD COLUMN email TEXT;`,
			[]string{`ALTER TABLE a ADD COLUMN "it's;" TEXT, ADD COLUMN email TEXT`}},
		{"ALTER TABLE a ADD COLUMN a$b$ TEXT; ALTER TABLE a ADD COLUMN email TEXT;",
			[]string{"ALTER TABLE a ADD COLUMN a$b$ TEXT", "ALTER TABLE a ADD COLUMN email TEXT"}},
		// The E of date'…' ends an identifier, so this is a plain string that a backslash does not escape.
		{"SELECT date'x\\'; ALTER TABLE a ADD COLUMN email TEXT;",
			[]string{"SELECT date''", "ALTER TABLE a ADD COLUMN email TEXT"}},
	} {
		var got []string
		for _, st := range sqlStatements(strings.Split(c.src, "\n")) {
			got = append(got, st.text)
		}
		if strings.Join(got, "|") != strings.Join(c.want, "|") {
			t.Errorf("%q:\n got  %q\n want %q", c.src, got, c.want)
		}
	}
}

// TestPIIReportOrder: the PII columns of one line are reported in the same (name) order every run, so
// the text and SARIF output is deterministic (they came out in map order).
func TestPIIReportOrder(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "services", "migrations", "identity")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	sql := "-- +goose Up\nCREATE TABLE svc_identity.p (surname TEXT, email TEXT, dob DATE, ip TEXT, birthday DATE);\n"
	if err := os.WriteFile(filepath.Join(dir, "00001_p.sql"), []byte(sql), 0o644); err != nil {
		t.Fatal(err)
	}
	var first string
	for i := 0; i < 20; i++ {
		res, err := Run(Options{Root: root, Rules: []string{"CONF-07"}, Strict: true})
		if err != nil {
			t.Fatal(err)
		}
		var sb strings.Builder
		res.WriteText(&sb)
		if i == 0 {
			first = sb.String()
			continue
		}
		if sb.String() != first {
			t.Fatalf("run %d differs:\n%s\nfirst:\n%s", i, sb.String(), first)
		}
	}
	last := -1
	for _, col := range []string{"birthday", "dob", "email", "ip", "surname"} {
		at := strings.Index(first, "svc_identity.p."+col+" ")
		if at < 0 || at < last {
			t.Fatalf("column %s is missing or out of name order:\n%s", col, first)
		}
		last = at
	}
}

// TestCMakeCommands: parentheses inside quoted arguments, bracket arguments and comments, and escaped
// ones do not count toward a command's end, so they cannot swallow the commands after them; a command
// still open at the end of the file is returned and reported as open.
func TestCMakeCommands(t *testing.T) {
	for _, c := range []struct {
		src  string
		want []string // name@line
		open int
	}{
		{"message(STATUS \"ISA (avx2\")\nadd_compile_options(-mavx2)", []string{"message@1", "add_compile_options@2"}, 0},
		{"string(REGEX REPLACE \"\\\\(.*\" \"\" s \"${v}\")\nx()", []string{"string@1", "x@2"}, 0},
		{"string(APPEND l \"|R\\\"[^()]*\\\\(\")\nx()", []string{"string@1", "x@2"}, 0},
		{"set(a \"multi\nline ( \\\" string\")\nx()", []string{"set@1", "x@3"}, 0},
		{"set(a [=[ ( ]] ]=])\nx()", []string{"set@1", "x@2"}, 0},
		{"#[[ (\n]] x()\ny()", []string{"x@2", "y@3"}, 0},
		{"#[[\nx(\n]]\ny()", []string{"y@4"}, 0},
		{"set(a \\()\nx() # comment (\ny( # (\n)", []string{"set@1", "x@2", "y@3"}, 0},
		{"if(a) x() endif()", []string{"if@1", "x@1", "endif@1"}, 0},
		{"set(a \"#\" b) # c(\nx()", []string{"set@1", "x@2"}, 0},
		{"ok()\nmessage(\"never closed\"\nx()", []string{"ok@1", "message@2"}, 2},
	} {
		cmds, open := cmakeCommands(strings.Split(c.src, "\n"))
		var got []string
		for _, cmd := range cmds {
			got = append(got, fmt.Sprintf("%s@%d", cmd.name, cmd.line))
		}
		if strings.Join(got, " ") != strings.Join(c.want, " ") || open != c.open {
			t.Errorf("%q:\n got  %v (open %d)\n want %v (open %d)", c.src, got, open, c.want, c.open)
		}
	}
}

// TestISAEnvRefOnce: an ISA level computed from $ENV{} is named once in its finding, in a grant and when
// passed to a wrapper (engine/clang/CMakeLists.txt:15-16 of the CONF-11 bad fixture).
func TestISAEnvRefOnce(t *testing.T) {
	res, err := Run(Options{Root: filepath.Join("..", "..", "testdata", "CONF-11", "bad"), Rules: []string{"CONF-11"},
		Strict: true})
	if err != nil {
		t.Fatal(err)
	}
	seen := 0
	for _, f := range res.Findings {
		if f.Path != "engine/clang/CMakeLists.txt" || f.Line < 15 || f.Line > 16 {
			continue
		}
		seen++
		if n := strings.Count(f.Message, "$ENV{HELIOS_ARCH}"); n != 1 {
			t.Errorf("%s:%d names $ENV{HELIOS_ARCH} %d times: %s", f.Path, f.Line, n, f.Message)
		}
	}
	if seen != 2 {
		t.Errorf("want 2 findings on engine/clang/CMakeLists.txt:15-16, got %d", seen)
	}
}

// TestCBlockBrace: how CONF-08 tells a block's `{` (a statement boundary) from an initializer's, by the
// statement text before it (blanked: a string keeps its quotes). It pins the cases the fixtures cannot reach
// in valid C++ (an empty statement before a brace, an unmatched `]`) and the rest beside them.
func TestCBlockBrace(t *testing.T) {
	for _, tc := range []struct {
		stmt  string
		block bool
	}{
		{"", true},                                    // a bare block at a statement's start
		{"  \n\t", true},                              // only white space since the boundary
		{"void f()", true},                            // a function body
		{"if (x)", true},                              // a control statement
		{"auto f() -> net::Address", true},            // a trailing return type
		{"[&]() mutable -> int", true},                // a lambda's trailing return type
		{"Probe::Probe(Net& n) : connect_{n}", true},  // a constructor body after its initializer list
		{"case Mode::Connect:", true},                 // a labelled block
		{"for (;;) x;", true},                         // a `;` that is no boundary, then a brace
		{"extern \" \"", true},                        // extern "C" {, blanked
		{"namespace helios::net", true},               // a namespace
		{"struct ConnectStats : Counters<int>", true}, // a class head with a templated base
		{"else", true},
		{"do", true},
		{"try", true},
		{"void f() const", true},
		{"void f() noexcept", true},
		{"void f() override", true},
		{"void f() final", true},
		{"[x]() mutable", true},
		{"[connect]", true},              // an immediately-invoked lambda at a statement's start
		{"run([&]", true},                // a lambda's introducer after `(`
		{"auto f = [x]", true},           // after `=`
		{"if (x) [[likely]]", true},      // an attribute before a block
		{"x]", true},                     // an unmatched `]`
		{"net::Address listen", false},   // an initializer after a name
		{"net::Address listen =", false}, // after `=`
		{"pick(kFallback,", false},       // after a comma
		{"pick(", false},                 // after `(`
		{"std::vector<std::string> listen{", false},
		{"return", false},                                 // a returned braced list
		{"auto listen = std::vector<std::string>", false}, // a template's `>`
		{"net::Address listen[1]", false},                 // an array bound
		{"std::string listen[1][1]", false},               // two bounds
		{"auto* listen = new std::array<int, 2>[n]", false},
		{"auto* listen = new T[n]", false},
		{"return [x]", true}, // a returned lambda
		{"throw [x]", true},
	} {
		if got := cBlockBrace(tc.stmt); got != tc.block {
			t.Errorf("cBlockBrace(%q) = %v, want %v", tc.stmt, got, tc.block)
		}
	}
}
