package hxl

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"math/bits"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// Go runner of the shared corpus tests/corpus/hxl/*.jsonc (format: tests/corpus/hxl/README.md). The
// C++ runner (engine/hxl/tests/corpus.cpp) performs the same checks on the same files.

// corpusDir is tests/corpus/hxl of the checkout. A missing corpus fails the test (GP-1 must never
// pass vacuously); set HXL_CORPUS_OPTIONAL=1 to skip instead, e.g. when vendoring the package alone.
func corpusDir(t testing.TB) string {
	_, file, _, _ := runtime.Caller(0)
	dir := filepath.Join(filepath.Dir(file), "..", "..", "..", "tests", "corpus", "hxl")
	if _, err := os.Stat(dir); err != nil {
		if os.Getenv("HXL_CORPUS_OPTIONAL") != "" {
			t.Skipf("corpus not found at %s: %v", dir, err)
		}
		t.Fatalf("corpus not found at %s: %v (set HXL_CORPUS_OPTIONAL=1 to skip)", dir, err)
	}
	return dir
}

// stripJSONC removes // and /* */ comments and trailing commas outside strings.
func stripJSONC(in []byte) []byte {
	var out bytes.Buffer
	inStr, esc := false, false
	for i := 0; i < len(in); i++ {
		c := in[i]
		if inStr {
			out.WriteByte(c)
			if esc {
				esc = false
			} else if c == '\\' {
				esc = true
			} else if c == '"' {
				inStr = false
			}
			continue
		}
		switch {
		case c == '"':
			inStr = true
			out.WriteByte(c)
		case c == '/' && i+1 < len(in) && in[i+1] == '/':
			for i < len(in) && in[i] != '\n' {
				i++
			}
			out.WriteByte('\n')
		case c == '/' && i+1 < len(in) && in[i+1] == '*':
			i += 2
			for i+1 < len(in) && !(in[i] == '*' && in[i+1] == '/') {
				i++
			}
			i++
		default:
			out.WriteByte(c)
		}
	}
	// Trailing commas: a comma followed only by whitespace before ] or }.
	re := regexp.MustCompile(`,(\s*[\]}])`)
	return re.ReplaceAll(out.Bytes(), []byte("$1"))
}

func hexDigit(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	case c >= 'A' && c <= 'F':
		return int(c-'A') + 10
	}
	return -1
}

// parseHexFloat mirrors the C++ exact hex-float parser (text after "0x", no sign).
func parseHexFloat(s string) (float64, bool) {
	i := 0
	var mant uint64
	exp := 0
	digits := false
	add := func(d int, fraction bool) bool {
		digits = true
		if mant == 0 && d == 0 {
			if fraction {
				exp -= 4
			}
			return true
		}
		if mant>>60 != 0 {
			return false
		}
		mant = mant*16 + uint64(d)
		if fraction {
			exp -= 4
		}
		return true
	}
	for i < len(s) && hexDigit(s[i]) >= 0 {
		if !add(hexDigit(s[i]), false) {
			return 0, false
		}
		i++
	}
	if i < len(s) && s[i] == '.' {
		i++
		for i < len(s) && hexDigit(s[i]) >= 0 {
			if !add(hexDigit(s[i]), true) {
				return 0, false
			}
			i++
		}
	}
	if !digits || i >= len(s) || (s[i] != 'p' && s[i] != 'P') {
		return 0, false
	}
	i++
	neg := false
	if i < len(s) && (s[i] == '+' || s[i] == '-') {
		neg = s[i] == '-'
		i++
	}
	if i >= len(s) {
		return 0, false
	}
	e := 0
	for ; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
		e = e*10 + int(s[i]-'0')
		if e > 100000 {
			return 0, false
		}
	}
	if neg {
		e = -e
	}
	exp += e
	if mant == 0 {
		return 0, true
	}
	h := 63 - bits.LeadingZeros64(mant)
	top := h + exp
	if top > 1023 {
		return 0, false
	}
	var b uint64
	if top >= -1022 {
		shift := h - 52
		var m53 uint64
		if shift > 0 {
			if mant&(1<<uint(shift)-1) != 0 {
				return 0, false
			}
			m53 = mant >> uint(shift)
		} else {
			m53 = mant << uint(-shift)
		}
		b = uint64(top+1023)<<52 | m53&(1<<52-1)
	} else {
		s2 := exp + 1074
		var m uint64
		if s2 >= 0 {
			if s2 > 52 {
				return 0, false
			}
			m = mant << uint(s2)
			if m>>uint(s2) != mant {
				return 0, false
			}
		} else {
			if -s2 >= 64 || mant&(1<<uint(-s2)-1) != 0 {
				return 0, false
			}
			m = mant >> uint(-s2)
		}
		if m >= 1<<52 {
			return 0, false
		}
		b = m
	}
	return math.Float64frombits(b), true
}

// parseCorpusNumber mirrors helios::hxl::test::parseCorpusNumber.
func parseCorpusNumber(text string) (float64, bool) {
	if text == "nan" {
		return math.Float64frombits(CanonicalNaNBits), true
	}
	s := text
	neg := false
	if len(s) > 0 && (s[0] == '-' || s[0] == '+') {
		neg = s[0] == '-'
		s = s[1:]
	}
	var v float64
	switch {
	case s == "inf":
		v = math.Inf(1)
	case len(s) > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'):
		var ok bool
		if v, ok = parseHexFloat(s[2:]); !ok {
			return 0, false
		}
	default:
		if len(s) == 0 || len(s) > MaxNumberBytes || !((s[0] >= '0' && s[0] <= '9') || s[0] == '.') ||
			strings.ContainsAny(s, "xXnN_") {
			return 0, false
		}
		f, ok := parseNumber(s) // decimal text follows the literal rule: zero or a normal double
		if !ok {
			return 0, false
		}
		v = f
	}
	if neg {
		v = -v
	}
	return v, true
}

func describeBits(v float64) string { return fmt.Sprintf("0x%016x (%v)", math.Float64bits(v), v) }

type corpusStats struct {
	files, cases, errorCases, evaluations, fmaRows, bytecodeHashes int
	failures                                                       []string
}

type corpusRunner struct {
	stats *corpusStats
	file  string
	name  string
	names map[string]bool
}

func (r *corpusRunner) fail(format string, args ...any) {
	prefix := r.file + ": "
	if r.name != "" {
		prefix += r.name + ": "
	}
	r.stats.failures = append(r.stats.failures, prefix+fmt.Sprintf(format, args...))
}

func (r *corpusRunner) checkKeys(obj map[string]any, what string, allowed ...string) bool {
	ok := true
	for k := range obj {
		found := false
		for _, a := range allowed {
			if a == k {
				found = true
			}
		}
		if !found {
			r.fail("unknown key '%s' in %s", k, what)
			ok = false
		}
	}
	return ok
}

func (r *corpusRunner) number(v any) (float64, bool) {
	switch x := v.(type) {
	case json.Number:
		f, ok := parseCorpusNumber(string(x))
		if !ok {
			r.fail("bad number '%s'", x)
		}
		return f, ok
	case string:
		f, ok := parseCorpusNumber(x)
		if !ok {
			r.fail("bad number '%s'", x)
		}
		return f, ok
	}
	r.fail("expected a number, got %T", v)
	return 0, false
}

func (r *corpusRunner) curves(v any, out map[string]*Curve) bool {
	obj, ok := v.(map[string]any)
	if !ok {
		r.fail("curves must be an object")
		return false
	}
	for name, cv := range obj {
		co, ok := cv.(map[string]any)
		if !ok || !r.checkKeys(co, "a curve", "keys", "values") {
			return false
		}
		c := &Curve{}
		for _, part := range []string{"keys", "values"} {
			arr, ok := co[part].([]any)
			if !ok {
				r.fail("curve keys/values must be arrays")
				return false
			}
			for _, e := range arr {
				x, ok := r.number(e)
				if !ok {
					return false
				}
				if part == "keys" {
					c.Keys = append(c.Keys, x)
				} else {
					c.Values = append(c.Values, x)
				}
			}
		}
		if err := c.Validate(); err != nil {
			r.fail("invalid curve '%s': %v", name, err)
			return false
		}
		out[name] = c
	}
	return true
}

func (r *corpusRunner) numberMap(v any, out map[string]map[string]float64) bool {
	obj, _ := v.(map[string]any)
	for param, pv := range obj {
		po, ok := pv.(map[string]any)
		if !ok {
			r.fail("attrs/fields entries must be objects")
			return false
		}
		if out[param] == nil {
			out[param] = map[string]float64{}
		}
		for k, e := range po {
			x, ok := r.number(e)
			if !ok {
				return false
			}
			out[param][k] = x
		}
	}
	return true
}

func (r *corpusRunner) inputs(v any, env *MapEnv) bool {
	obj, ok := v.(map[string]any)
	if !ok || !r.checkKeys(obj, "inputs", "attrs", "fields", "tags", "stacks", "level", "curves") {
		return false
	}
	if a, ok := obj["attrs"]; ok && !r.numberMap(a, env.Attrs) {
		return false
	}
	if f, ok := obj["fields"]; ok && !r.numberMap(f, env.Fields) {
		return false
	}
	if tg, ok := obj["tags"].(map[string]any); ok {
		for param, list := range tg {
			for _, e := range list.([]any) {
				env.Tags[param] = append(env.Tags[param], e.(string))
			}
		}
	}
	if s, ok := obj["stacks"]; ok {
		env.HasStacks = true
		if env.StacksVal, ok = r.number(s); !ok {
			return false
		}
	}
	if l, ok := obj["level"]; ok {
		env.HasLevel = true
		if env.LevelVal, ok = r.number(l); !ok {
			return false
		}
	}
	if c, ok := obj["curves"]; ok && !r.curves(c, env.Curves) {
		return false
	}
	return true
}

func (r *corpusRunner) expectValue(expect any, got Value, where string) bool {
	if b, ok := expect.(bool); ok {
		if got.Type != Bool || got.Bool() != b {
			r.fail("%sexpected %v, got %s %s", where, b, got.Type, describeBits(got.Number))
			return false
		}
		return true
	}
	want, ok := r.number(expect)
	if !ok {
		return false
	}
	if got.Type != Number || math.Float64bits(got.Number) != math.Float64bits(want) {
		r.fail("%sexpected %s, got %s %s", where, describeBits(want), got.Type, describeBits(got.Number))
		return false
	}
	return true
}

// fused returns the FMA results of a row; the row is FMA-sensitive if none equals the expected value.
func fused(kind string, in []float64) ([]float64, bool) {
	switch kind {
	case "mul_add":
		return []float64{math.FMA(in[0], in[1], in[2])}, true
	case "mul_sub":
		return []float64{math.FMA(in[0], in[1], -in[2])}, true
	case "sub_mul":
		return []float64{math.FMA(-in[0], in[1], in[2])}, true
	case "dot2":
		ab := float64(in[0] * in[1])
		cd := float64(in[2] * in[3])
		return []float64{math.FMA(in[0], in[1], cd), math.FMA(in[2], in[3], ab)}, true
	case "lerp":
		return []float64{math.FMA(in[1]-in[0], in[2], in[0])}, true
	}
	return nil, false
}

func hasAnyKey(obj map[string]any, keys ...string) bool {
	for _, k := range keys {
		if _, ok := obj[k]; ok {
			return true
		}
	}
	return false
}

// unfused is the IEEE definition of an FMA-sensitive row's expression, one rounding per operation
// (every product converted, 06 §1.2 rule 1): the expected value must equal it, so a vector cannot
// pin a wrong result that both VMs happen to share.
func unfused(kind string, in []float64) (float64, bool) {
	switch kind {
	case "mul_add":
		return float64(in[0]*in[1]) + in[2], true
	case "mul_sub":
		return float64(in[0]*in[1]) - in[2], true
	case "sub_mul":
		return in[2] - float64(in[0]*in[1]), true
	case "dot2":
		return float64(in[0]*in[1]) + float64(in[2]*in[3]), true
	case "lerp":
		return in[0] + float64((in[1]-in[0])*in[2]), true
	}
	return 0, false
}

func (r *corpusRunner) runCase(c map[string]any, fileCurves map[string]*Curve) {
	name, ok := c["name"].(string)
	if !ok {
		r.name = "<unnamed>"
		r.fail("case without a name")
		return
	}
	r.name = name
	if r.names[name] {
		r.fail("duplicate case name")
	}
	r.names[name] = true
	if !r.checkKeys(c, "a case", "name", "src", "params", "expectType", "maxCost", "error", "at", "bytecode",
		"type", "inputs", "expect", "evalError", "rows", "fma", "comment") {
		return
	}
	r.stats.cases++
	src, ok := c["src"].(string)
	if !ok {
		r.fail("missing src")
		return
	}
	var opts CompileOptions
	if ps, ok := c["params"].([]any); ok {
		opts.Params = []string{}
		for _, p := range ps {
			opts.Params = append(opts.Params, p.(string))
		}
	}
	if et, ok := c["expectType"].(string); ok {
		t := Number
		if et == "bool" {
			t = Bool
		}
		opts.ExpectedType = &t
	}
	if mc, ok := c["maxCost"]; ok {
		x, ok := r.number(mc)
		if !ok {
			return
		}
		opts.MaxCost = int(x)
	}
	prog, err := Compile(src, opts)
	if e, ok := c["error"].(string); ok {
		r.stats.errorCases++
		at, _ := c["at"].(string)
		want := e + " " + at
		if err == nil {
			r.fail("expected compile error %s, but it compiled", want)
			return
		}
		d, _ := AsDiagnostic(err)
		got := fmt.Sprintf("%s %d:%d", d.Status, d.Line, d.Column)
		if got != want {
			r.fail("expected compile error %s, got %s (%s)", want, got, d.Message)
		}
		return
	}
	if err != nil {
		r.fail("compile failed: %v", err)
		return
	}
	// A case that compiles must check something: a name plus a source would pass vacuously.
	if !hasAnyKey(c, "bytecode", "expect", "evalError", "rows") {
		r.fail("the case compiles but checks nothing (add bytecode, expect, evalError or rows)")
	}
	encoded := prog.Encode()
	decoded, err := Decode(encoded)
	if err != nil {
		r.fail("Decode(Encode()) failed: %v", err)
		return
	}
	if !bytes.Equal(decoded.Encode(), encoded) {
		r.fail("Decode(Encode()) is not canonical")
	}
	if h, ok := c["bytecode"].(string); ok {
		r.stats.bytecodeHashes++
		if got := fmt.Sprintf("%016x", prog.Hash()); got != h {
			r.fail("bytecode hash %s != expected %s", got, h)
		}
	}
	if t, ok := c["type"].(string); ok && t != prog.ResultType().String() {
		r.fail("result type %s != expected %s", prog.ResultType(), t)
	}
	env := &MapEnv{Attrs: map[string]map[string]float64{}, Fields: map[string]map[string]float64{},
		Tags: map[string][]string{}, Curves: map[string]*Curve{}}
	for k, v := range fileCurves {
		env.Curves[k] = v
	}
	if in, ok := c["inputs"]; ok && !r.inputs(in, env) {
		return
	}
	expect, hasExpect := c["expect"]
	evalErr, hasEvalErr := c["evalError"].(string)
	if hasExpect || hasEvalErr {
		r.stats.evaluations++
		env.Bind(decoded)
		v, s := decoded.Eval(env)
		switch {
		case hasEvalErr:
			if s.String() != evalErr {
				r.fail("expected eval error %s, got %s", evalErr, s)
			}
		case s != OK:
			r.fail("eval failed: %s", s)
		default:
			r.expectValue(expect, v, "")
		}
	}
	if rows, ok := c["rows"].(map[string]any); ok {
		fma, _ := c["fma"].(string)
		r.runRows(rows, decoded, env, fma)
	}
}

func (r *corpusRunner) runRows(rows map[string]any, prog *Program, env *MapEnv, fma string) {
	if !r.checkKeys(rows, "rows", "param", "fields", "data") {
		return
	}
	param := "v"
	if p, ok := rows["param"].(string); ok {
		param = p
	}
	var fields []string
	if fs, ok := rows["fields"].([]any); ok {
		for _, f := range fs {
			fields = append(fields, f.(string))
		}
	}
	data, _ := rows["data"].([]any)
	if env.Fields[param] == nil {
		env.Fields[param] = map[string]float64{}
	}
	for i, rowAny := range data {
		row, _ := rowAny.([]any)
		if len(row) != len(fields)+1 {
			r.fail("row %d has %d values, expected %d", i, len(row), len(fields)+1)
			return
		}
		in := make([]float64, len(fields))
		for k := range fields {
			x, ok := r.number(row[k])
			if !ok {
				return
			}
			in[k] = x
			env.Fields[param][fields[k]] = x
		}
		env.Bind(prog)
		v, s := prog.Eval(env)
		r.stats.evaluations++
		if s != OK {
			r.fail("row %d: eval failed: %s", i, s)
			continue
		}
		if !r.expectValue(row[len(fields)], v, fmt.Sprintf("row %d: ", i)) {
			continue
		}
		if fma != "" {
			r.stats.fmaRows++
			fs, ok := fused(fma, in)
			if !ok {
				r.fail("unknown fma kind '%s'", fma)
				return
			}
			for _, f := range fs {
				if math.Float64bits(f) == math.Float64bits(v.Number) {
					r.fail("row %d: not FMA-sensitive (fused result equals the expected value)", i)
					break
				}
			}
			if ref, _ := unfused(fma, in); math.Float64bits(ref) != math.Float64bits(v.Number) {
				r.fail("row %d: the expected value %s is not the unfused reference %s", i, describeBits(v.Number), describeBits(ref))
			}
		}
	}
}

func (r *corpusRunner) runFile(path string) {
	r.file = filepath.Base(path)
	r.name = ""
	raw, err := os.ReadFile(path)
	if err != nil {
		r.fail("%v", err)
		return
	}
	dec := json.NewDecoder(bytes.NewReader(stripJSONC(raw)))
	dec.UseNumber()
	var root map[string]any
	if err := dec.Decode(&root); err != nil {
		r.fail("JSON error: %v", err)
		return
	}
	r.stats.files++
	if !r.checkKeys(root, "the file", "description", "curves", "cases") {
		return
	}
	fileCurves := map[string]*Curve{}
	if cv, ok := root["curves"]; ok && !r.curves(cv, fileCurves) {
		return
	}
	cases, _ := root["cases"].([]any)
	for _, c := range cases {
		cm, ok := c.(map[string]any)
		if !ok {
			r.fail("a case must be an object")
			continue
		}
		r.runCase(cm, fileCurves)
		r.name = ""
	}
}

func runCorpus(dir string) corpusStats {
	var stats corpusStats
	matches, _ := filepath.Glob(filepath.Join(dir, "*.jsonc"))
	sort.Strings(matches)
	r := &corpusRunner{stats: &stats, names: map[string]bool{}}
	for _, m := range matches {
		r.runFile(m)
	}
	return stats
}

// TestCorpus is GP-1's corpus clause on the Go side: every case matches C++ bit for bit.
func TestCorpus(t *testing.T) {
	stats := runCorpus(corpusDir(t))
	for _, f := range stats.failures {
		t.Error(f)
	}
	t.Logf("hxl corpus: %d files, %d cases (%d compile errors), %d evaluations, %d FMA-sensitive rows, %d bytecode hashes",
		stats.files, stats.cases, stats.errorCases, stats.evaluations, stats.fmaRows, stats.bytecodeHashes)
	// Floors just below the current corpus (and the same in C++), so losing a file or a block of cases
	// is noticed.
	if stats.files < 10 || stats.cases < 1600 || stats.errorCases < 440 || stats.evaluations < 2350 || stats.fmaRows < 1050 ||
		stats.bytecodeHashes < 1170 {
		t.Errorf("corpus too small: %+v", stats)
	}
}

// TestCorpusFill (HXL_CORPUS_FILL=1) replaces "?" placeholders in the corpus with the computed
// bytecode hashes and results. Review the diff: every value must also be checked against an
// independent reference (see tests/corpus/hxl/README.md) before it is committed.
func TestCorpusFill(t *testing.T) {
	if os.Getenv("HXL_CORPUS_FILL") == "" {
		t.Skip("set HXL_CORPUS_FILL=1 to fill corpus placeholders")
	}
	files, _ := filepath.Glob(filepath.Join(corpusDir(t), "*.jsonc"))
	placeholderHash := regexp.MustCompile(`"bytecode":(\s*)"\?"`)
	for _, path := range files {
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Contains(raw, []byte(`"?"`)) {
			continue
		}
		dec := json.NewDecoder(bytes.NewReader(stripJSONC(raw)))
		dec.UseNumber()
		var root map[string]any
		if err := dec.Decode(&root); err != nil {
			t.Fatalf("%s: %v", path, err)
		}
		r := &corpusRunner{stats: &corpusStats{}, names: map[string]bool{}}
		fileCurves := map[string]*Curve{}
		if cv, ok := root["curves"]; ok {
			r.curves(cv, fileCurves)
		}
		var hashes, values []string
		for _, ca := range root["cases"].([]any) {
			c := ca.(map[string]any)
			if _, isErr := c["error"]; isErr {
				continue
			}
			var opts CompileOptions
			if ps, ok := c["params"].([]any); ok {
				opts.Params = []string{}
				for _, p := range ps {
					opts.Params = append(opts.Params, p.(string))
				}
			}
			if et, ok := c["expectType"].(string); ok {
				ty := Number
				if et == "bool" {
					ty = Bool
				}
				opts.ExpectedType = &ty
			}
			if mc, ok := c["maxCost"]; ok {
				x, _ := r.number(mc)
				opts.MaxCost = int(x)
			}
			prog, err := Compile(c["src"].(string), opts)
			if err != nil {
				t.Fatalf("%s: %v: %v", path, c["name"], err)
			}
			if c["bytecode"] == "?" {
				hashes = append(hashes, fmt.Sprintf("%016x", prog.Hash()))
			}
			env := &MapEnv{Attrs: map[string]map[string]float64{}, Fields: map[string]map[string]float64{},
				Tags: map[string][]string{}, Curves: map[string]*Curve{}}
			for k, v := range fileCurves {
				env.Curves[k] = v
			}
			if in, ok := c["inputs"]; ok {
				r.inputs(in, env)
			}
			if c["expect"] == "?" {
				env.Bind(prog)
				v, s := prog.Eval(env)
				if s != OK {
					t.Fatalf("%s: %v: %s", path, c["name"], s)
				}
				values = append(values, formatCorpusValue(v))
			}
			if rows, ok := c["rows"].(map[string]any); ok {
				param := "v"
				if p, ok := rows["param"].(string); ok {
					param = p
				}
				var fields []string
				for _, f := range rows["fields"].([]any) {
					fields = append(fields, f.(string))
				}
				if env.Fields[param] == nil {
					env.Fields[param] = map[string]float64{}
				}
				for _, rowAny := range rows["data"].([]any) {
					row := rowAny.([]any)
					if row[len(row)-1] != "?" {
						continue
					}
					for k, f := range fields {
						x, _ := r.number(row[k])
						env.Fields[param][f] = x
					}
					env.Bind(prog)
					v, s := prog.Eval(env)
					if s != OK {
						t.Fatalf("%s: %v: %s", path, c["name"], s)
					}
					values = append(values, formatCorpusValue(v))
				}
			}
		}
		out := raw
		hi := 0
		out = placeholderHash.ReplaceAllFunc(out, func(m []byte) []byte {
			sub := placeholderHash.FindSubmatch(m)
			h := hashes[hi]
			hi++
			return []byte(`"bytecode":` + string(sub[1]) + `"` + h + `"`)
		})
		vi := 0
		out = regexp.MustCompile(`"\?"`).ReplaceAllFunc(out, func([]byte) []byte {
			v := values[vi]
			vi++
			return []byte(v)
		})
		if hi != len(hashes) || vi != len(values) {
			t.Fatalf("%s: placeholder count mismatch (%d/%d hashes, %d/%d values)", path, hi, len(hashes), vi, len(values))
		}
		if err := os.WriteFile(path, out, 0o644); err != nil {
			t.Fatal(err)
		}
		t.Logf("filled %s: %d hashes, %d values", filepath.Base(path), hi, vi)
	}
}

// formatCorpusValue renders a result exactly: JSON booleans, small integers as JSON numbers, and
// everything else as an exact hex float string.
func formatCorpusValue(v Value) string {
	if v.Type == Bool {
		return strconv.FormatBool(v.Bool())
	}
	x := v.Number
	switch {
	case math.IsNaN(x):
		return `"nan"`
	case math.IsInf(x, 1):
		return `"inf"`
	case math.IsInf(x, -1):
		return `"-inf"`
	case x == 0 && math.Signbit(x):
		return `"-0"`
	case x == math.Trunc(x) && math.Abs(x) < 1e15:
		return strconv.FormatFloat(x, 'f', -1, 64)
	}
	return `"` + strconv.FormatFloat(x, 'x', -1, 64) + `"`
}

func TestCorpusNumberSyntax(t *testing.T) {
	cases := []struct {
		text string
		bits uint64
		ok   bool
	}{
		{"0x1.8p+1", 0x4008000000000000, true},
		{"-0x1p-1074", 0x8000000000000001, true},
		{"0x0.0000000000001p-1022", 1, true},
		{"0x1.fffffffffffffp+1023", 0x7fefffffffffffff, true},
		{"0x1.8p+01", 0x4008000000000000, true}, // Go's %x style exponent
		{"0x1p+1024", 0, false},
		{"0x1.00000000000001p+0", 0, false},
		{"0x1p-1075", 0, false},
		{"0x1.8", 0, false},
		{"-0", 0x8000000000000000, true},
		{"0.1", 0x3fb999999999999a, true},
		{"-inf", 0xfff0000000000000, true},
		{"1.5x", 0, false},
		{"", 0, false},
	}
	for _, c := range cases {
		v, ok := parseCorpusNumber(c.text)
		if ok != c.ok || (ok && math.Float64bits(v) != c.bits) {
			t.Errorf("parseCorpusNumber(%q) = %#x, %v; want %#x, %v", c.text, math.Float64bits(v), ok, c.bits, c.ok)
		}
	}
	if v, ok := parseCorpusNumber("nan"); !ok || !math.IsNaN(v) {
		t.Errorf("nan")
	}
}
