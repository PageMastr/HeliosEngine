package hxl

import (
	"bytes"
	"math"
	"strings"
	"testing"

	"github.com/PageMastr/scifi-test/services/pkg/hxl/det"
)

func mustCompile(t testing.TB, src string, params ...string) *Program {
	t.Helper()
	opts := CompileOptions{}
	if params != nil {
		opts.Params = params
	}
	p, err := Compile(src, opts)
	if err != nil {
		t.Fatalf("Compile(%q): %v", src, err)
	}
	return p
}

func TestCompileBasics(t *testing.T) {
	p := mustCompile(t, "formula ThrustToWeight(ship) = attr(ship, MaxThrust) / (attr(ship, Mass) * 9.81);")
	if p.Name() != "ThrustToWeight" || len(p.Params()) != 1 || p.Params()[0] != "ship" {
		t.Fatalf("header: %q %v", p.Name(), p.Params())
	}
	if got := strings.Join(p.AttrSymbols(), ","); got != "MaxThrust,Mass" {
		t.Fatalf("attr symbols %s", got)
	}
	env := &MapEnv{Attrs: map[string]map[string]float64{"ship": {"MaxThrust": 981, "Mass": 10}}}
	env.Bind(p)
	v, err := p.Evaluate(env)
	if err != nil {
		t.Fatal(err)
	}
	thrust, mass := 981.0, 10.0
	if want := thrust / float64(mass*9.81); v.Number != want {
		t.Fatalf("got %v want %v", v.Number, want)
	}
	// No constant folding: 1 + 2 + 1 is Const Const Add Const Add with deduplicated constants.
	q := mustCompile(t, "1 + 2 + 1")
	if !bytes.Equal(q.Code(), []byte{0x01, 0, 0, 0x01, 1, 0, 0x11, 0x01, 0, 0, 0x11}) || q.Cost() != 5 || q.MaxStack() != 2 {
		t.Fatalf("code %x cost %d stack %d", q.Code(), q.Cost(), q.MaxStack())
	}
	if !strings.Contains(q.Disassemble(), "Add") {
		t.Fatal("disassembly")
	}
	// Nested curves register in code order.
	c := mustCompile(t, "curve(Outer, curve(Inner, 1))")
	if strings.Join(c.CurveSymbols(), ",") != "Inner,Outer" {
		t.Fatalf("curve symbols %v", c.CurveSymbols())
	}
	refs := mustCompile(t, "attr(src, A) * ctx.d + attr(tgt, B)", "src", "tgt", "ctx").References()
	if len(refs) != 3 || refs[1].Op != OpField || refs[1].Param != 2 || refs[2].Param != 1 {
		t.Fatalf("references %+v", refs)
	}
}

func TestDiagnostics(t *testing.T) {
	cases := map[string]string{
		"1 + @":             "E_LEX 1:5",
		"12abc":             "E_NUMBER 1:1",
		"(1 + 2":            "E_SYNTAX 1:7",
		"formula F(x, x)=1": "E_DUPLICATE_PARAM 1:14",
		"foo":               "E_UNKNOWN_NAME 1:1",
		"stacks(1)":         "E_ARITY 1:1",
		"min(1)":            "E_ARITY 1:1",
		"1 + true":          "E_TYPE 1:3",
		"attr(1, Hp)":       "E_ENTITY_ARG 1:6",
		"curve(1, 2)":       "E_SYMBOL_ARG 1:7",
	}
	for src, want := range cases {
		_, err := Compile(src, CompileOptions{})
		d, ok := AsDiagnostic(err)
		if !ok {
			t.Errorf("%q: compiled, want %s", src, want)
			continue
		}
		if got := d.Status.String() + " " + itoaPos(d.Line, d.Column); got != want {
			t.Errorf("%q: %s (%s), want %s", src, got, d.Message, want)
		}
	}
	bt := Bool
	if _, err := Compile("1 + 2", CompileOptions{ExpectedType: &bt}); err == nil {
		t.Error("expected E_RESULT_TYPE")
	}
	if StatusFromName("E_LIMIT") != ELimit || StatusFromName("nope") != OK {
		t.Error("StatusFromName")
	}
}

func itoaPos(l, c int) string {
	return strings.TrimSpace(strings.Join([]string{itoa(l), itoa(c)}, ":"))
}

func itoa(i int) string {
	if i == 0 {
		return "0"
	}
	s := ""
	for i > 0 {
		s = string(rune('0'+i%10)) + s
		i /= 10
	}
	return s
}

func TestSemantics(t *testing.T) {
	env := &MapEnv{}
	num := func(src string) float64 {
		p := mustCompile(t, src)
		env.Bind(p)
		v, s := p.Eval(env)
		if s != OK {
			t.Fatalf("%s: %s", src, s)
		}
		return v.Number
	}
	negZero := math.Copysign(0, -1)
	checks := []struct {
		src  string
		want float64
	}{
		{"-2 ^ 2", -4}, {"2 ^ 3 ^ 2", 512}, {"min(0, -0)", negZero}, {"max(-0, 0)", 0}, {"clamp(0.5, 2, 1)", 1},
		{"ceil(-0.5)", negZero}, {"exp(1.5)", det.Exp(1.5)}, {"asinh(-3.25)", det.Asinh(-3.25)},
		{"select(true, 5, attr(self, Missing))", 5},
	}
	for _, c := range checks {
		if got := num(c.src); math.Float64bits(got) != math.Float64bits(c.want) {
			t.Errorf("%s = %v (%#x), want %v", c.src, got, math.Float64bits(got), c.want)
		}
	}
	if got := num("0/0"); math.Float64bits(got) != CanonicalNaNBits {
		t.Errorf("NaN not canonical: %#x", math.Float64bits(got))
	}
	p := mustCompile(t, "true && attr(self, Missing) > 0")
	env.Bind(p)
	if _, s := p.Eval(env); s != EMissingInput {
		t.Errorf("missing input: %s", s)
	}
}

func TestCurve(t *testing.T) {
	c := &Curve{Keys: []float64{0, 10, 20}, Values: []float64{0, 100, 50}}
	if err := c.Validate(); err != nil {
		t.Fatal(err)
	}
	for x, want := range map[float64]float64{-5: 0, 5: 50, 10: 100, 15: 75, 25: 50} {
		if got := c.Sample(x); got != want {
			t.Errorf("Sample(%v) = %v, want %v", x, got, want)
		}
	}
	for _, bad := range []*Curve{{}, {Keys: []float64{1, 1}, Values: []float64{0, 1}}, {Keys: []float64{0}, Values: nil},
		{Keys: []float64{0, math.Inf(1)}, Values: []float64{0, 1}}} {
		if bad.Validate() == nil {
			t.Errorf("invalid curve accepted: %+v", bad)
		}
	}
}

func TestEncodeDecodeRoundTrip(t *testing.T) {
	for _, src := range []string{
		"1",
		"attr(self, A.B) * self.x + stacks() - level()",
		"select(tag(self, T.U), curve(C, 2), min(1, 2, 3))",
		"true && (false || !(1 < 2))",
		"formula F(a, b) = exp(ln(attr(a, X))) ^ asinh(b.y)",
	} {
		p := mustCompile(t, src)
		b := p.Encode()
		q, err := Decode(b)
		if err != nil {
			t.Fatalf("%s: %v", src, err)
		}
		if !bytes.Equal(q.Encode(), b) || q.Hash() != p.Hash() {
			t.Errorf("%s: round trip is not canonical", src)
		}
	}
	// The same pinned encoding as the C++ test (test_bytecode.cpp).
	want := []byte{'H', 'X', 'L', '1', 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 4, 0, 's', 'e', 'l', 'f', 1, 0, 0, 0, 0, 0, 0, 0,
		0xf0, 0x3f, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0x01, 0, 0}
	if got := mustCompile(t, "1").Encode(); !bytes.Equal(got, want) {
		t.Errorf("encoding of \"1\": %x", got)
	}
}

func TestDecodeRejectsMalformed(t *testing.T) {
	p := mustCompile(t, "select(1 < 2, attr(self, A), 3)")
	good := p.Encode()
	code := len(good) - len(p.Code())
	mutate := func(f func(b []byte) []byte) Status {
		b := f(append([]byte(nil), good...))
		_, err := Decode(b)
		if err == nil {
			return OK
		}
		d, _ := AsDiagnostic(err)
		return d.Status
	}
	checks := []func(b []byte) []byte{
		func(b []byte) []byte { b[0] = 'X'; return b },
		func(b []byte) []byte { b[4] = 2; return b },
		func(b []byte) []byte { b[5] = 1; return b },
		func(b []byte) []byte { b[6] = 9; return b },
		func(b []byte) []byte { b[8] ^= 1; return b },
		func(b []byte) []byte { return append(b, 0) },
		func(b []byte) []byte { return b[:len(b)-1] },
		func(b []byte) []byte { b[code] = 0xEE; return b },
		func(b []byte) []byte { b[code+1] = 7; return b },
		func(b []byte) []byte { b[code+8], b[code+9] = 1, 0; return b },
		func(b []byte) []byte { b[code+8] = 0xFF; return b },
		func(b []byte) []byte { b[code+15] = 0; return b },
		func(b []byte) []byte { b[code+11] = 1; return b },
		func(b []byte) []byte { b[code+12] = 1; return b },
		func(b []byte) []byte { b[code+6] = byte(OpAdd); return b },
		func(b []byte) []byte { b[code+6] = byte(OpNot); return b },
		func(b []byte) []byte { return nil },
	}
	for i, f := range checks {
		if s := mutate(f); s != EBytecode {
			t.Errorf("mutation %d: status %s, want E_BYTECODE", i, s)
		}
	}
}

// totalEnv answers every input so any decodable program evaluates (same values as the C++ fuzz).
type totalEnv struct{ c Curve }

func (e *totalEnv) Attr(int, int) (float64, bool)  { return 1.5, true }
func (e *totalEnv) Field(int, int) (float64, bool) { return -2, true }
func (e *totalEnv) Tag(_, s int) (bool, bool)      { return s&1 != 0, true }
func (e *totalEnv) Curve(int) *Curve               { return &e.c }
func (e *totalEnv) Stacks() (float64, bool)        { return 3, true }
func (e *totalEnv) Level() (float64, bool)         { return 4, true }

func checkDecoded(t *testing.T, b []byte) {
	p, err := Decode(b)
	if err != nil {
		return
	}
	if p.MaxStack() > MaxStack || p.Cost() > MaxCost {
		t.Fatalf("decoded program exceeds limits")
	}
	env := &totalEnv{c: Curve{Keys: []float64{0, 1}, Values: []float64{0, 2}}}
	v, s := p.Eval(env)
	if s != OK || v.Type != p.ResultType() {
		t.Fatalf("decoded program failed: %s", s)
	}
	if !bytes.Equal(p.Encode(), b) {
		t.Fatalf("accepted a non-canonical encoding")
	}
}

// TestDecodeMutations mirrors the C++ mutation fuzz with the same PRNG and edit scheme.
func TestDecodeMutations(t *testing.T) {
	seeds := []*Program{
		mustCompile(t, "select(tag(self, T), attr(self, A) * 2 + curve(C, self.x), min(stacks(), level(), 3))"),
		mustCompile(t, "true && (1 < 2 || !false) && exp(1) > ln(2)"),
		mustCompile(t, "formula F(a, b) = clamp(lerp(attr(a, X), b.y, 0.5), -1, pow(2, 3)) ^ 0.5"),
	}
	state := uint64(0x9E3779B97F4A7C15)
	next := func() uint64 {
		state ^= state << 13
		state ^= state >> 7
		state ^= state << 17
		return state
	}
	decoded := 0
	acceptedHash := uint64(0xcbf29ce484222325) // FNV-1a over every accepted encoding, in order
	for iter := 0; iter < 30000; iter++ {
		b := seeds[iter%3].Encode()
		edits := 1 + int(next()%4)
		for e := 0; e < edits; e++ {
			at := int(next() % uint64(len(b)))
			switch next() % 4 {
			case 0:
				b[at] = byte(next())
			case 1:
				b[at] ^= 1 << (next() % 8)
			case 2:
				b = append(b[:at], append([]byte{byte(next())}, b[at:]...)...)
			default:
				if len(b) > 1 {
					b = append(b[:at], b[at+1:]...)
				}
			}
		}
		if _, err := Decode(b); err == nil {
			decoded++
			for _, c := range b {
				acceptedHash ^= uint64(c)
				acceptedHash *= 0x100000001b3
			}
		}
		checkDecoded(t, b)
	}
	t.Logf("mutants that decoded: %d", decoded)
	// Pinned to the same values as the C++ test (kAcceptedMutants, kAcceptedMutantsHash): both
	// verifiers must accept exactly the same hostile programs.
	if decoded != 1067 || acceptedHash != 0x2b6c209b5e06162c {
		t.Errorf("accepted mutants: %d, hash %#016x; want 1067, 0x2b6c209b5e06162c (as C++)", decoded, acceptedHash)
	}
}

// FuzzDecode: `go test -fuzz FuzzDecode ./pkg/hxl/` explores beyond the deterministic mutations.
func FuzzDecode(f *testing.F) {
	for _, src := range []string{"1", "select(tag(self, T), attr(self, A), 2)", "curve(C, self.x) ^ 2 && true || false"} {
		if p, err := Compile(src, CompileOptions{}); err == nil {
			f.Add(p.Encode())
		}
	}
	f.Fuzz(func(t *testing.T, b []byte) { checkDecoded(t, b) })
}

// FuzzCompile: the compiler never panics and whatever it accepts round-trips.
func FuzzCompile(f *testing.F) {
	for _, s := range []string{"1 + 2", "formula F(a) = attr(a, X) ^ 2", "select(tag(self, T), 1, 2)", "((((1))))"} {
		f.Add(s)
	}
	f.Fuzz(func(t *testing.T, src string) {
		p, err := Compile(src, CompileOptions{})
		if err != nil {
			if _, ok := AsDiagnostic(err); !ok {
				t.Fatalf("non-diagnostic error %v", err)
			}
			return
		}
		checkDecoded(t, p.Encode())
	})
}

func TestEvalDoesNotAllocate(t *testing.T) {
	p := mustCompile(t, "0.5 ^ ((attr(self, A) * 40000 / self.x)^2 + curve(C, 0.5)) + select(tag(self, T), 1, 2)")
	env := &MapEnv{Attrs: map[string]map[string]float64{"self": {"A": 0.01}}, Fields: map[string]map[string]float64{"self": {"x": 2}},
		Tags: map[string][]string{"self": {"T.U"}}, Curves: map[string]*Curve{"C": {Keys: []float64{0, 1}, Values: []float64{0, 1}}}}
	env.Bind(p)
	allocs := testing.AllocsPerRun(100, func() {
		if _, s := p.Eval(env); s != OK {
			t.Fatal(s)
		}
	})
	if allocs != 0 {
		t.Errorf("Eval allocates %v times", allocs)
	}
}

func TestCheckGOAMD64(t *testing.T) {
	if err := CheckGOAMD64(false); err != nil {
		t.Errorf("non-production must pass: %v", err)
	}
	level := GOAMD64()
	err := CheckGOAMD64(true)
	if (level == "" || level == "v1") != (err == nil) {
		t.Errorf("GOAMD64=%q: CheckGOAMD64(true) = %v", level, err)
	}
	t.Logf("GOAMD64=%q", level)
}

func BenchmarkEvalTurret(b *testing.B) {
	p := mustCompile(b, "formula TurretHitChance(src, tgt, ctx) = 0.5 ^ ((ctx.w * 40000 / (attr(src, T) * attr(tgt, S)))^2 + (max(0, ctx.d - attr(src, O)) / attr(src, F))^2)")
	env := &MapEnv{Attrs: map[string]map[string]float64{"src": {"T": 0.05, "O": 10000, "F": 5000}, "tgt": {"S": 40}},
		Fields: map[string]map[string]float64{"ctx": {"w": 0.01, "d": 15000}}}
	env.Bind(p)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		p.Eval(env)
	}
}
