package hxl

import (
	"encoding/binary"
	"fmt"
	"math"
	"strings"
	"testing"

	"github.com/PageMastr/scifi-test/services/pkg/hxl/gamedef"
)

// Regression tests from the WP-0.19 adversarial review; each has a C++ twin in engine/hxl/tests.

// TestZeroProgramIsRejected: Eval of a zero Program returns E_BYTECODE (C++ used to read an
// uninitialized result slot while Go returned 0).
func TestZeroProgramIsRejected(t *testing.T) {
	var p Program
	v, s := p.Eval(&MapEnv{})
	if s != EBytecode || v != (Value{}) {
		t.Errorf("zero Program: got %v, %s; want E_BYTECODE", v, s)
	}
	if _, err := p.Evaluate(&MapEnv{}); err == nil {
		t.Error("Evaluate of a zero Program succeeded")
	} else if d, _ := AsDiagnostic(err); d.Status != EBytecode {
		t.Errorf("Evaluate status %s", d.Status)
	}
}

// TestMapEnvInvalidCurveIsMissing: invalid curves bind as missing (Sample used to panic on them).
func TestMapEnvInvalidCurveIsMissing(t *testing.T) {
	p := mustCompile(t, "curve(C, 0.5)")
	for _, bad := range []*Curve{{}, {Keys: []float64{0, 1}, Values: []float64{1}}, {Keys: []float64{1, 0}, Values: []float64{0, 1}},
		{Keys: []float64{0, math.Inf(1)}, Values: []float64{0, 1}}} {
		env := &MapEnv{Curves: map[string]*Curve{"C": bad}}
		env.Bind(p)
		if _, s := p.Eval(env); s != EMissingInput {
			t.Errorf("invalid curve %+v: status %s, want E_MISSING_INPUT", bad, s)
		}
	}
	env := &MapEnv{Curves: map[string]*Curve{"C": {Keys: []float64{0, 1}, Values: []float64{0, 4}}}}
	env.Bind(p)
	if v, s := p.Eval(env); s != OK || v.Number != 2 {
		t.Errorf("valid curve: %v %s", v, s)
	}
}

// TestDecodeLimitIsBytecodeAtOrigin mirrors the C++ test "decode reports size and cost limits as
// E_BYTECODE at 0:0".
func TestDecodeLimitIsBytecodeAtOrigin(t *testing.T) {
	b := []byte{'H', 'X', 'L', '1', 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 4, 0, 's', 'e', 'l', 'f', 1, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,
		0, 0, 0, 0, 0, 0, 0, 0}
	b = binary.LittleEndian.AppendUint32(b, 3+600)
	b = append(b, byte(OpConst), 0, 0)
	for i := 0; i < 600; i++ {
		b = append(b, byte(OpExp))
	}
	_, err := Decode(b)
	d, ok := AsDiagnostic(err)
	if !ok || d.Status != EBytecode || d.Line != 0 || d.Column != 0 || !strings.Contains(d.Message, "cost") {
		t.Errorf("got %v", err)
	}
	// Over budget from source: E_LIMIT at 1:1 (the cost is a property of the whole program).
	src := "max(exp(1)" + strings.Repeat(", exp(1)", 599) + ")"
	if _, err := Compile(src, CompileOptions{}); err == nil {
		t.Error("600 exp() calls compiled")
	} else if d, _ := AsDiagnostic(err); d.Status != ELimit || d.Line != 1 || d.Column != 1 {
		t.Errorf("compile: %v", err)
	}
}

// TestRecordFormulasFollowCppRules: Go content validation must reject what the C++ kernel rejects
// (AttributeLayout::bindFormula). CompileMagnitude used to accept context fields, curves and
// multi-parameter formula declarations, and CompileDerived multi-parameter declarations.
func TestRecordFormulasFollowCppRules(t *testing.T) {
	magnitude := func(src string) error {
		m := gamedef.NewModifierDef()
		m.Magnitude = gamedef.Magnitude{Hxl: &gamedef.MagnitudeHxl{Expr: gamedef.HxlExpr(src)}}
		_, err := CompileMagnitude(&m)
		return err
	}
	derived := func(src string) error {
		a := gamedef.NewAttributeDef()
		a.Id = "X"
		e := gamedef.HxlExpr(src)
		a.Derived = &e
		_, err := CompileDerived(&a)
		return err
	}
	for _, src := range []string{"self.speed * 2", "curve(Falloff, 0.5)", "formula F(a, b) = attr(a, X) + attr(b, X)",
		"formula F() = 1", "tag(self, T)"} {
		if magnitude(src) == nil {
			t.Errorf("magnitude %q accepted", src)
		}
		if derived(src) == nil {
			t.Errorf("derived %q accepted", src)
		}
	}
	for _, src := range []string{"stacks() * 5 + level()", "formula Bonus(ship) = attr(ship, Skill.X) * 0.05"} {
		if err := magnitude(src); err != nil {
			t.Errorf("magnitude %q: %v", src, err)
		}
	}
	if derived("stacks()") == nil {
		t.Error("derived stacks() accepted")
	}
	if err := derived("formula Ehp(ship) = attr(ship, Hull.Hp) * select(tag(ship, T), 2, 1)"); err != nil {
		t.Errorf("one-parameter derived formula: %v", err)
	}
}

// TestDeepestPrograms mirrors the C++ test "hxl compiler: the deepest programs within the limits
// compile and evaluate": one level below each nesting limit compiles and evaluates, one level more
// fails at the same position (the parsers use precedence climbing in both languages).
func TestDeepestPrograms(t *testing.T) {
	nest := MaxParseDepth - 1          // the top-level expression is nesting level 1
	eval := func(src string) float64 { // bool results are 0 / 1
		t.Helper()
		p := mustCompile(t, src)
		env := &MapEnv{}
		env.Bind(p)
		v, s := p.Eval(env)
		if s != OK {
			t.Fatalf("eval: %s", s)
		}
		return v.Number
	}
	errorOf := func(src string) string {
		t.Helper()
		_, err := Compile(src, CompileOptions{})
		if err == nil {
			return "OK"
		}
		d, _ := AsDiagnostic(err)
		return fmt.Sprintf("%s %d:%d", d.Status, d.Line, d.Column)
	}
	r := strings.Repeat
	check := func(what string, got, want any) {
		t.Helper()
		if got != want {
			t.Errorf("%s: got %v, want %v", what, got, want)
		}
	}
	check("parentheses", eval(r("(", nest)+"1"+r(")", nest)), 1.0)
	check("prefix", eval(r("-", nest)+"1"), -1.0)
	check("prefix+1", errorOf(r("-", nest+1)+"1"), "E_LIMIT 1:128")
	check("power", eval("1"+r("^1", nest)), 1.0)
	check("power+1", errorOf("1"+r("^1", nest+1)), "E_LIMIT 1:256")
	const calls = 126
	links := MaxAstDepth - calls - 1
	nestedMin := func(n int) string { return r("min(1, ", calls) + "1" + r("+1", n) + r(")", calls) }
	check("calls", eval(nestedMin(links)), 1.0)
	check("calls+1", errorOf(nestedMin(links+1)), "E_LIMIT 1:1")
	check("clamp", eval(r("clamp(1, 0, ", calls)+"1"+r("+1", links)+r(")", calls)), 1.0)
	check("select", eval(r("select(true, ", calls)+"true"+r("||true", links)+r(", false)", calls)), 1.0) // true
	levels := (MaxAstDepth - 1) / 7
	check("every level", eval(r("select(true || true && true == 1 < 1 + 1 * (", levels)+"1"+r("), 1, 0)", levels)), 1.0)
}
