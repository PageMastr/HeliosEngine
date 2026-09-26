package hxl

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// TestGenerateDifferentialCorpus (HXL_CORPUS_GEN_DIFF=1) writes tests/corpus/hxl/differential.jsonc:
// seeded random programs (well-typed ones, ill-typed ones and byte-level mutations of both) with
// random inputs, including every special float class. It is a differential test of the two
// interpreters: the expected compile outcome (status and position), bytecode hash and result come
// from this Go implementation, and the C++ runner must reproduce all of them bit for bit. It
// exercises parser, type-checker and VM paths that the hand-written files reach only by example.
//
// HXL_CORPUS_GEN_DIFF_OUT overrides the output path and HXL_CORPUS_GEN_DIFF_COUNT the case count
// (for long exploratory runs outside the committed corpus).
func TestGenerateDifferentialCorpus(t *testing.T) {
	if os.Getenv("HXL_CORPUS_GEN_DIFF") == "" {
		t.Skip("set HXL_CORPUS_GEN_DIFF=1 to regenerate differential.jsonc")
	}
	count := 1200
	if s := os.Getenv("HXL_CORPUS_GEN_DIFF_COUNT"); s != "" {
		n, err := strconv.Atoi(s)
		if err != nil || n <= 0 {
			t.Fatalf("bad HXL_CORPUS_GEN_DIFF_COUNT %q", s)
		}
		count = n
	}
	out := filepath.Join(corpusDir(t), "differential.jsonc")
	if s := os.Getenv("HXL_CORPUS_GEN_DIFF_OUT"); s != "" {
		out = s
	}
	data, stats := generateDifferentialCorpus(0xd1ff5eed, count)
	if err := os.WriteFile(out, data, 0o644); err != nil {
		t.Fatal(err)
	}
	t.Logf("wrote %s: %s", out, stats)
}

type diffGen struct {
	rng splitMix64
}

func (g *diffGen) intn(n int) int           { return int(g.rng.next() % uint64(n)) }
func (g *diffGen) chance(num, den int) bool { return g.intn(den) < num }

func (g *diffGen) pick(options ...string) string { return options[g.intn(len(options))] }

// literal returns number literal text, occasionally one that fails to lex (E_NUMBER).
func (g *diffGen) literal() string {
	switch g.intn(12) {
	case 0:
		return g.pick("0", "1", "2", "3", "10", "100", "0.5", "0.25", "0.1", "0.2", "0.3", "2.67")
	case 1:
		return g.pick("1e308", "1.7976931348623157e308", "2.2250738585072014e-308", "1e-300", "4.9e-324",
			"1e400", "1e-400", "0e999", "0.0e-5", "123456789012345678901234567890", "9007199254740993")
	case 2:
		return g.pick(".5", "7.", "1e3", "1E-3", "1e+2", "00012", "1.e5", "3.", ".0")
	case 3:
		return g.pick("1e", "1e+", "1.2.3", "5x", "1_0", "0x10", "2e5e5")
	default:
		// A random decimal with up to 20 digits and an optional exponent.
		var sb strings.Builder
		n := 1 + g.intn(8)
		for i := 0; i < n; i++ {
			sb.WriteByte(byte('0' + g.intn(10)))
		}
		if g.chance(1, 2) {
			sb.WriteByte('.')
			m := g.intn(12)
			for i := 0; i < m; i++ {
				sb.WriteByte(byte('0' + g.intn(10)))
			}
		}
		if g.chance(1, 3) {
			sb.WriteByte("eE"[g.intn(2)])
			if g.chance(1, 2) {
				sb.WriteByte("+-"[g.intn(2)])
			}
			if g.chance(1, 10) {
				sb.WriteString(strconv.Itoa(g.intn(330))) // mostly out of range: E_NUMBER
			} else {
				sb.WriteString(strconv.Itoa(g.intn(40)))
			}
		}
		return sb.String()
	}
}

var diffUnary = []string{"sqrt", "exp", "ln", "asinh", "abs", "floor", "ceil"}

// num generates a number-typed expression (rarely a deliberately ill-typed or ill-formed one).
func (g *diffGen) num(depth int) string {
	if g.chance(1, 60) {
		return g.boolean(depth - 1) // type error
	}
	if depth <= 0 || g.chance(1, 4) {
		switch g.intn(10) {
		case 0, 1, 2:
			return g.literal()
		case 3:
			return "attr(" + g.pick("self", "self", "other", "ctx", "nobody", "1") + ", " +
				g.pick("A", "B.C", "D", "A", "Missing.X", "(A)", "1", "B . C") + ")"
		case 4:
			return g.pick("self.x", "ctx.dist", "ctx.t", "ctx.dist", "other.y", "self.missing", "self.x.y", "nobody.x")
		case 5:
			return g.pick("stacks()", "level()", "stacks(1)", "level()")
		case 6:
			return g.pick("self", "x", "pi", "true", "A.B")
		default:
			return g.literal()
		}
	}
	d := depth - 1
	switch g.intn(14) {
	case 0, 1, 2, 3:
		return g.num(d) + " " + g.pick("+", "-", "*", "/", "^", "+", "*") + " " + g.num(d)
	case 4:
		return "-" + g.num(d)
	case 5:
		return "(" + g.num(d) + ")"
	case 6:
		fn := g.pick("min", "max")
		n := 2 + g.intn(3)
		if g.chance(1, 20) {
			n = 1
		}
		args := make([]string, n)
		for i := range args {
			args[i] = g.num(d)
		}
		return fn + "(" + strings.Join(args, ", ") + ")"
	case 7:
		fn := g.pick("clamp", "lerp", "pow", "clamp", "lerp")
		n := 3
		if fn == "pow" {
			n = 2
		}
		if g.chance(1, 20) {
			n += g.intn(3) - 1
		}
		args := make([]string, n)
		for i := range args {
			args[i] = g.num(d)
		}
		return fn + "(" + strings.Join(args, ", ") + ")"
	case 8, 9:
		fn := diffUnary[g.intn(len(diffUnary))]
		if g.chance(1, 40) {
			fn = g.pick("sin", "log", "Sqrt", "fma")
		}
		return fn + "(" + g.num(d) + ")"
	case 10:
		return "select(" + g.boolean(d) + ", " + g.num(d) + ", " + g.num(d) + ")"
	case 11:
		return "curve(" + g.pick("C1", "C2", "C1", "Missing", "C1.Sub", "1") + ", " + g.num(d) + ")"
	default:
		return g.num(d) + " " + g.pick("*", "/") + " " + g.num(d) + " " + g.pick("+", "-") + " " + g.num(d)
	}
}

// boolean generates a bool-typed expression (rarely an ill-typed one).
func (g *diffGen) boolean(depth int) string {
	if g.chance(1, 60) {
		return g.num(depth - 1)
	}
	if depth <= 0 || g.chance(1, 4) {
		switch g.intn(4) {
		case 0:
			return g.pick("true", "false")
		default:
			return "tag(" + g.pick("self", "other", "ctx", "self", "zz") + ", " +
				g.pick("T", "T.U", "T.U.V", "W", "T.X", "W.Q", "Tx", "1") + ")"
		}
	}
	d := depth - 1
	switch g.intn(9) {
	case 0, 1, 2:
		return g.num(d) + " " + g.pick("<", "<=", ">", ">=", "==", "!=") + " " + g.num(d)
	case 3:
		return g.boolean(d) + " " + g.pick("==", "!=") + " " + g.boolean(d)
	case 4, 5:
		return g.boolean(d) + " " + g.pick("&&", "||") + " " + g.boolean(d)
	case 6:
		return "!" + g.boolean(d)
	case 7:
		return "(" + g.boolean(d) + ")"
	default:
		return "select(" + g.boolean(d) + ", " + g.boolean(d) + ", " + g.boolean(d) + ")"
	}
}

// mutate applies one byte-level edit: most mutants are syntax or lexical errors at varied positions.
func (g *diffGen) mutate(src string) string {
	if len(src) == 0 {
		return src
	}
	const alphabet = "()+-*/^,.<>=!&|;@#$ ab1_\n\t\r\"'"
	at := g.intn(len(src))
	switch g.intn(4) {
	case 0:
		return src[:at] + src[at+1:]
	case 1:
		return src[:at] + string(alphabet[g.intn(len(alphabet))]) + src[at:]
	case 2:
		if g.chance(1, 10) {
			return src[:at] + "\xc3\xa9" + src[at:] // a non-ASCII byte sequence
		}
		return src[:at] + string(alphabet[g.intn(len(alphabet))]) + src[at+1:]
	default:
		end := at + 1 + g.intn(4)
		if end > len(src) {
			end = len(src)
		}
		return src[:at] + src[at:end] + src[at:]
	}
}

// value returns a random input value covering every special class.
func (g *diffGen) value() float64 {
	switch g.intn(16) {
	case 0:
		return 0
	case 1:
		return math.Copysign(0, -1)
	case 2:
		return math.Inf(1)
	case 3:
		return math.Inf(-1)
	case 4:
		return math.Float64frombits(CanonicalNaNBits)
	case 5:
		return math.Float64frombits(1 + g.rng.next()%(1<<52-1)) // subnormal
	case 6:
		return float64(g.intn(21) - 10)
	case 7:
		return float64(g.intn(9)) * 0.125
	default:
		m := 1 + float64(float64(g.rng.next()>>11)*0x1p-53)
		e := g.intn(80) - 40
		if g.chance(1, 8) {
			e = g.intn(2040) - 1020
		}
		v := math.Ldexp(m, e)
		if g.chance(1, 2) {
			v = -v
		}
		return v
	}
}

// corpusNumberText renders an input exactly in the corpus number syntax.
func corpusNumberText(v float64) string {
	switch {
	case math.IsNaN(v):
		return "nan"
	case math.IsInf(v, 1):
		return "inf"
	case math.IsInf(v, -1):
		return "-inf"
	case v == 0 && math.Signbit(v):
		return "-0"
	}
	return strconv.FormatFloat(v, 'x', -1, 64)
}

func jsonString(s string) string {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(s); err != nil {
		panic(err)
	}
	return strings.TrimRight(buf.String(), "\n")
}

type diffStats struct{ cases, errors, evalErrors, values int }

func (s diffStats) String() string {
	return fmt.Sprintf("%d cases: %d compile errors, %d evaluation errors, %d results", s.cases, s.errors, s.evalErrors, s.values)
}

// caseSource generates one case's source and compile options.
func (g *diffGen) caseSource(allowMutation bool) (string, CompileOptions, []string) {
	depth := 1 + g.intn(5)
	var src string
	if g.chance(1, 4) {
		src = g.boolean(depth)
	} else {
		src = g.num(depth)
	}
	if g.chance(1, 8) {
		params := g.pick("self, other, ctx", "self, other, ctx", "ctx, self, other", "self, self", "self,", "", "1")
		src = "formula F(" + params + ") = " + src
	}
	if g.chance(1, 10) {
		src += g.pick(";", " ;", "// comment", "\n", ";;", " 1")
	}
	if allowMutation && g.chance(1, 3) {
		src = g.mutate(src)
	}
	opts := CompileOptions{Params: []string{"self", "other", "ctx"}}
	var extra []string
	if g.chance(1, 10) {
		ty := Number
		if g.chance(1, 2) {
			ty = Bool
		}
		opts.ExpectedType = &ty
		extra = append(extra, `"expectType": "`+ty.String()+`"`)
	}
	if g.chance(1, 12) {
		opts.MaxCost = 1 + g.intn(40)
		extra = append(extra, `"maxCost": `+strconv.Itoa(opts.MaxCost))
	}
	return src, opts, extra
}

func generateDifferentialCorpus(seed uint64, count int) ([]byte, diffStats) {
	g := &diffGen{rng: splitMix64{seed}}
	curves := map[string]*Curve{
		"C1": {Keys: []float64{-2, 0, 0.5, 3, 1e6}, Values: []float64{5, -1, 0.1, 1e300, -1e300}},
		"C2": {Keys: []float64{1}, Values: []float64{42}},
	}
	var sb strings.Builder
	sb.WriteString("{\n  \"description\": \"Differential corpus generated by services/pkg/hxl/diffgen_test.go " +
		"(HXL_CORPUS_GEN_DIFF=1; do not edit by hand): seeded random programs, ill-typed programs and byte-level " +
		"mutants with random inputs. Expected values come from the Go interpreter; the C++ interpreter must agree " +
		"on every status, position, bytecode hash and result bit.\",\n")
	sb.WriteString("  \"curves\": {\n" +
		"    \"C1\": { \"keys\": [-2, 0, 0.5, 3, 1e6], \"values\": [5, -1, 0.1, 1e300, -1e300] },\n" +
		"    \"C2\": { \"keys\": [1], \"values\": [42] }\n  },\n  \"cases\": [\n")
	var stats diffStats
	for i := 0; i < count; i++ {
		// Three in five cases retry until the program compiles (so evaluation is well covered); the
		// rest keep whatever the generator produced, including mutants.
		requireValid := g.intn(5) < 3
		var src string
		var opts CompileOptions
		var extra []string
		for try := 0; ; try++ {
			src, opts, extra = g.caseSource(!requireValid)
			if !requireValid || try == 50 {
				break
			}
			if _, err := Compile(src, opts); err == nil {
				break
			}
		}
		stats.cases++
		fmt.Fprintf(&sb, "    { \"name\": \"diff.%04d\", \"src\": %s, \"params\": [\"self\", \"other\", \"ctx\"]", i, jsonString(src))
		for _, e := range extra {
			sb.WriteString(", " + e)
		}
		prog, err := Compile(src, opts)
		if err != nil {
			d, _ := AsDiagnostic(err)
			stats.errors++
			fmt.Fprintf(&sb, ", \"error\": \"%s\", \"at\": \"%d:%d\" }", d.Status, d.Line, d.Column)
		} else {
			// Inputs: each one present with high probability, so missing inputs are also covered.
			env := &MapEnv{Attrs: map[string]map[string]float64{}, Fields: map[string]map[string]float64{},
				Tags: map[string][]string{}, Curves: map[string]*Curve{}}
			for k, v := range curves {
				env.Curves[k] = v
			}
			var in []string
			var attrs []string
			for _, p := range []struct{ param, names string }{{"self", "A B.C D"}, {"other", "A B.C"}} {
				var kv []string
				for _, n := range strings.Fields(p.names) {
					if g.chance(1, 12) {
						continue
					}
					v := g.value()
					if env.Attrs[p.param] == nil {
						env.Attrs[p.param] = map[string]float64{}
					}
					env.Attrs[p.param][n] = v
					kv = append(kv, fmt.Sprintf("%q: %q", n, corpusNumberText(v)))
				}
				attrs = append(attrs, fmt.Sprintf("%q: { %s }", p.param, strings.Join(kv, ", ")))
			}
			in = append(in, `"attrs": { `+strings.Join(attrs, ", ")+` }`)
			var fields []string
			for _, p := range []struct{ param, names string }{{"self", "x"}, {"ctx", "dist t"}, {"other", "y"}} {
				var kv []string
				for _, n := range strings.Fields(p.names) {
					if g.chance(1, 12) {
						continue
					}
					v := g.value()
					if env.Fields[p.param] == nil {
						env.Fields[p.param] = map[string]float64{}
					}
					env.Fields[p.param][n] = v
					kv = append(kv, fmt.Sprintf("%q: %q", n, corpusNumberText(v)))
				}
				fields = append(fields, fmt.Sprintf("%q: { %s }", p.param, strings.Join(kv, ", ")))
			}
			in = append(in, `"fields": { `+strings.Join(fields, ", ")+` }`)
			selfTags := []string{g.pick("T.U.V", "T.U", "T", "W", "Tx.U"), g.pick("W.Q.R", "X", "T.X")}
			otherTags := []string{g.pick("T", "W", "T.U.V.Z")}
			env.Tags["self"] = selfTags
			env.Tags["other"] = otherTags
			in = append(in, fmt.Sprintf(`"tags": { "self": [%q, %q], "other": [%q] }`, selfTags[0], selfTags[1], otherTags[0]))
			if !g.chance(1, 12) {
				v := g.value()
				env.HasStacks, env.StacksVal = true, v
				in = append(in, fmt.Sprintf(`"stacks": %q`, corpusNumberText(v)))
			}
			if !g.chance(1, 12) {
				v := g.value()
				env.HasLevel, env.LevelVal = true, v
				in = append(in, fmt.Sprintf(`"level": %q`, corpusNumberText(v)))
			}
			env.Bind(prog)
			v, s := prog.Eval(env)
			fmt.Fprintf(&sb, ", \"bytecode\": \"%016x\", \"type\": \"%s\",\n      \"inputs\": { %s }", prog.Hash(), prog.ResultType(),
				strings.Join(in, ", "))
			if s != OK {
				stats.evalErrors++
				fmt.Fprintf(&sb, ", \"evalError\": \"%s\" }", s)
			} else {
				stats.values++
				fmt.Fprintf(&sb, ", \"expect\": %s }", formatCorpusValue(v))
			}
		}
		if i+1 < count {
			sb.WriteByte(',')
		}
		sb.WriteByte('\n')
	}
	sb.WriteString("  ]\n}\n")
	return []byte(sb.String()), stats
}

// TestDifferentialGeneratorIsDeterministic: the committed differential.jsonc is exactly what the
// generator produces (so it can be regenerated, and a stale file is noticed).
func TestDifferentialGeneratorIsDeterministic(t *testing.T) {
	path := filepath.Join(corpusDir(t), "differential.jsonc")
	want, err := os.ReadFile(path)
	if err != nil {
		t.Skipf("no differential.jsonc: %v", err)
	}
	got, _ := generateDifferentialCorpus(0xd1ff5eed, 1200)
	if !bytes.Equal(got, want) {
		t.Errorf("differential.jsonc is stale: regenerate it with HXL_CORPUS_GEN_DIFF=1 go test ./pkg/hxl/ -run TestGenerateDifferentialCorpus")
	}
}
