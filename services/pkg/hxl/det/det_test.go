package det

import (
	"math"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

// The input generators of engine/math/tests/test_det_exp.cpp, ported so the C++ golden hashes pin
// the Go port too. Every product in the generators is converted explicitly, so the inputs are the
// same at GOAMD64=v3 and on arm64.

type splitMix struct{ s uint64 }

func (r *splitMix) next() uint64 {
	r.s += 0x9e3779b97f4a7c15
	z := r.s
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9
	z = (z ^ (z >> 27)) * 0x94d049bb133111eb
	return z ^ (z >> 31)
}

func (r *splitMix) unit() float64 { return float64(float64(r.next()>>11) * 0x1p-53) }

func (r *splitMix) uniform(lo, hi float64) float64 {
	return lo + float64((hi-lo)*r.unit())
}

func (r *splitMix) logUniform(e0, e1 int) float64 {
	m := 1.0 + r.unit()
	e := e0 + int(r.next()%uint64(e1-e0))
	return math.Ldexp(m, e)
}

type hasher struct{ h uint64 }

func newHasher() hasher { return hasher{0xcbf29ce484222325} }

func (h *hasher) add(v float64) {
	bits := math.Float64bits(v)
	if math.IsNaN(v) {
		bits = 0x7ff8000000000000
	}
	for i := 0; i < 8; i++ {
		h.h ^= (bits >> (8 * i)) & 0xff
		h.h *= 0x100000001b3
	}
}

func expInputs() []float64 {
	r := splitMix{1}
	var v []float64
	for i := 0; i < 20000; i++ {
		v = append(v, r.uniform(-750.0, 712.0))
	}
	for i := 0; i < 4000; i++ {
		a := r.uniform(-1.0, 1.0)
		b := r.logUniform(-60, 0)
		v = append(v, float64(a*b))
	}
	return append(v, 0.0, math.Copysign(0, -1), 1.0, -1.0, 0.5, 709.782712893384, 709.79, -745.1332191019411,
		-745.14, -708.4, -720.0, 1e-300, -1e-300, 0x1p-1074, 0.34657359027997264, -0.34657359027997264,
		88.72283905206835)
}

func lnInputs() []float64 {
	r := splitMix{2}
	var v []float64
	for i := 0; i < 20000; i++ {
		v = append(v, math.Float64frombits(r.next()&0x7fefffffffffffff))
	}
	for i := 0; i < 4000; i++ {
		a := r.uniform(-1.0, 1.0)
		b := r.logUniform(-52, -1)
		v = append(v, 1.0+float64(a*b))
	}
	for k := -8; k <= 8; k++ {
		v = append(v, 1.0+float64(float64(k)*0x1p-52))
	}
	return append(v, 1.0, 2.0, 0.5, 0.7071067811865476, 1.4142135623730951, math.MaxFloat64, 0x1p-1022,
		0x1p-1074, 0x1p-1060, 2.718281828459045, 10.0, 1e-300, 1e300)
}

func powInputs() [][2]float64 {
	r := splitMix{3}
	var v [][2]float64
	for i := 0; i < 12000; i++ {
		x := r.logUniform(-40, 40)
		y := r.uniform(-30.0, 30.0)
		v = append(v, [2]float64{x, y})
	}
	for i := 0; i < 4000; i++ {
		x := 1.0 + r.uniform(-0.01, 0.01)
		y := r.uniform(-1e5, 1e5)
		v = append(v, [2]float64{x, y})
	}
	for i := 0; i < 4000; i++ {
		x := -r.logUniform(-10, 10)
		y := float64(int(r.next()%61) - 30)
		v = append(v, [2]float64{x, y})
	}
	for k := 0; k < 200; k++ {
		v = append(v, [2]float64{0.5, float64(float64(k) * 0.137)})
	}
	for k := 0; k < 50; k++ {
		v = append(v, [2]float64{0.8691199808003974, float64(k)})
	}
	return v
}

func asinhInputs() []float64 {
	r := splitMix{4}
	var v []float64
	for i := 0; i < 20000; i++ {
		m := r.logUniform(-40, 40)
		if r.next()&1 != 0 {
			v = append(v, m)
		} else {
			v = append(v, -m)
		}
	}
	for i := 0; i < 2000; i++ {
		v = append(v, r.uniform(-3.0, 3.0))
	}
	return append(v, 0x1p-28, 0x1p28, 0x1.0000000000001p28, 0x1.fffffffffffffp-29, 1e-300, 1e300,
		math.MaxFloat64, 0x1p-1074, 1.0, -1.0)
}

// TestGoldenHashesMatchCpp: the same ~95k outputs hash to the C++ goldens of test_det_exp.cpp.
func TestGoldenHashesMatchCpp(t *testing.T) {
	he, hl, hp, ha := newHasher(), newHasher(), newHasher(), newHasher()
	for _, x := range expInputs() {
		he.add(Exp(x))
	}
	for _, x := range lnInputs() {
		hl.add(Ln(x))
	}
	for _, xy := range powInputs() {
		hp.add(Pow(xy[0], xy[1]))
	}
	for _, x := range asinhInputs() {
		ha.add(Asinh(x))
	}
	check := func(name string, got, want uint64) {
		if got != want {
			t.Errorf("%s golden hash %#016x, C++ has %#016x", name, got, want)
		}
	}
	check("exp", he.h, 0x326543eaf9427a5f)
	check("ln", hl.h, 0x3fb4cab44bb60247)
	check("pow", hp.h, 0x0a9d6178cd84387b)
	check("asinh", ha.h, 0x8f30fd19714fe5f7)
}

func TestSpecialValues(t *testing.T) {
	inf := math.Inf(1)
	negZero := math.Copysign(0, -1)
	x := 1.1 // a variable: float64(1.1 * 1.1) would be exact constant arithmetic (06 §1.2 rule 2)
	same := func(a, b float64) bool {
		if math.IsNaN(a) || math.IsNaN(b) {
			return math.IsNaN(a) && math.IsNaN(b)
		}
		return math.Float64bits(a) == math.Float64bits(b)
	}
	cases := []struct {
		name      string
		got, want float64
	}{
		{"exp(0)", Exp(0), 1},
		{"exp(-inf)", Exp(-inf), 0},
		{"exp(inf)", Exp(inf), inf},
		{"exp(1000)", Exp(1000), inf},
		{"exp(-1000)", Exp(-1000), 0},
		{"ln(1)", Ln(1), 0},
		{"ln(0)", Ln(0), -inf},
		{"ln(-1)", Ln(-1), math.NaN()},
		{"ln(inf)", Ln(inf), inf},
		{"pow(nan,0)", Pow(math.NaN(), 0), 1},
		{"pow(1,nan)", Pow(1, math.NaN()), 1},
		{"pow(-8,1/3)", Pow(-8, 1.0/3.0), math.NaN()},
		{"pow(-2,3)", Pow(-2, 3), -8},
		{"pow(-0,-1)", Pow(negZero, -1), -inf},
		{"pow(-0,3)", Pow(negZero, 3), negZero},
		{"pow(-inf,-3)", Pow(math.Inf(-1), -3), negZero},
		{"pow(-inf,3)", Pow(math.Inf(-1), 3), -inf},
		{"pow(-1,inf)", Pow(-1, inf), 1},
		{"pow(0.5,inf)", Pow(0.5, inf), 0},
		{"pow(2,-inf)", Pow(2, -inf), 0},
		{"pow(x,2)", Pow(x, 2), float64(x * x)},
		{"pow(x,0.5)", Pow(2, 0.5), math.Sqrt(2)},
		{"pow(-2,1e300)", Pow(-2, 1e300), inf},
		{"asinh(-0)", Asinh(negZero), negZero},
		{"asinh(-inf)", Asinh(-inf), -inf},
	}
	// Invalid arguments give C++'s quiet NaN bit for bit (review regression, WP-0.19 round 1:
	// math.NaN() is 0x7ff8000000000001).
	for name, v := range map[string]float64{"ln(-1)": Ln(-1), "pow(-8,1/3)": Pow(-8, 1.0/3.0)} {
		if bits := math.Float64bits(v); bits != 0x7ff8000000000000 {
			t.Errorf("%s: NaN bits %#016x, want 0x7ff8000000000000", name, bits)
		}
	}
	for _, c := range cases {
		if !same(c.got, c.want) {
			t.Errorf("%s = %v (%#x), want %v", c.name, c.got, math.Float64bits(c.got), c.want)
		}
	}
}

// TestConstantsMatchCpp parses engine/math/src/det_exp.cpp and compares every constant (06 §1.2
// rule 2 stands in for `helios-tool hxl-gen-consts`). Skipped when the engine tree is absent.
func TestConstantsMatchCpp(t *testing.T) {
	_, file, _, _ := runtime.Caller(0)
	src := filepath.Join(filepath.Dir(file), "..", "..", "..", "..", "engine", "math", "src", "det_exp.cpp")
	data, err := os.ReadFile(src)
	if err != nil {
		t.Skipf("engine source not available: %v", err)
	}
	text := string(data)
	scalar := regexp.MustCompile(`constexpr f64 (k\w+) = (-?0x[0-9a-fA-F.]+p[+-]?\d+);`)
	want := map[string]float64{
		"kLn2Head": ln2Head, "kLn2Mid": ln2Mid, "kLn2Tail": ln2Tail, "kLn2Hi": ln2Hi, "kLn2Lo": ln2Lo,
		"kInvLn2": invLn2, "kTwoThirdsHi": twoThirdsHi, "kTwoThirdsLo": twoThirdsLo, "kSqrt2": sqrt2,
		"kExpOverflow": expOverflow, "kExpUnderflow": expUnderflow, "kTwo28": two28, "kTwoMinus28": twoMinus28,
		"kTwo64": two64,
	}
	seen := map[string]bool{}
	for _, m := range scalar.FindAllStringSubmatch(text, -1) {
		v, err := strconv.ParseFloat(m[2], 64)
		if err != nil {
			t.Fatalf("cannot parse %s = %s: %v", m[1], m[2], err)
		}
		g, ok := want[m[1]]
		if !ok {
			t.Errorf("C++ constant %s has no Go twin", m[1])
			continue
		}
		seen[m[1]] = true
		if math.Float64bits(g) != math.Float64bits(v) {
			t.Errorf("%s: Go %#x != C++ %#x", m[1], math.Float64bits(g), math.Float64bits(v))
		}
	}
	for name := range want {
		if !seen[name] {
			t.Errorf("Go constant for %s not found in the C++ source", name)
		}
	}
	arrays := map[string][13]float64{"kAtanhC": atanhC, "kExpC": expC}
	for name, goVals := range arrays {
		re := regexp.MustCompile(`constexpr f64 ` + name + `\[\] = \{([^}]*)\}`)
		m := re.FindStringSubmatch(text)
		if m == nil {
			t.Errorf("array %s not found", name)
			continue
		}
		var vals []float64
		for _, f := range strings.Split(m[1], ",") {
			f = strings.TrimSpace(f)
			if f == "" {
				continue
			}
			v, err := strconv.ParseFloat(f, 64)
			if err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			vals = append(vals, v)
		}
		if len(vals) != len(goVals) {
			t.Errorf("%s: %d C++ values, %d Go values", name, len(vals), len(goVals))
			continue
		}
		for i, v := range vals {
			if math.Float64bits(v) != math.Float64bits(goVals[i]) {
				t.Errorf("%s[%d]: Go %#x != C++ %#x", name, i, math.Float64bits(goVals[i]), math.Float64bits(v))
			}
		}
	}
	// Literals the C++ code uses inline.
	for _, lit := range []string{"0x1.0000002p+27", "0x1p+54", "0x1p-1000", "0x1p+53"} {
		if !strings.Contains(text, lit) {
			t.Errorf("inline C++ literal %s not found (Go twin out of date?)", lit)
		}
	}
}

func BenchmarkPow(b *testing.B) {
	x := 0.0
	for i := 0; i < b.N; i++ {
		x += Pow(1.0001, float64(i&1023))
	}
	_ = x
}
