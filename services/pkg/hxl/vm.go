package hxl

import (
	"fmt"
	"math"
	"strings"

	"github.com/PageMastr/scifi-test/services/pkg/hxl/det"
)

// The VM mirrors engine/hxl/src/vm.cpp op by op. Float rules (06 §1.2): every multiplication is the
// direct operand of float64() (no FMA fusion on arm64 or GOAMD64=v3), nothing is folded or
// reassociated, and transcendental functions come from package det, never from math.

// CanonicalNaNBits is the bit pattern of every NaN result.
const CanonicalNaNBits = 0x7ff8000000000000

func canonicalNaN() float64 { return math.Float64frombits(CanonicalNaNBits) }

// Curve is a piecewise-linear curve (keys strictly increasing and finite, values finite, >= 1 point).
type Curve struct {
	Keys   []float64
	Values []float64
}

// Validate checks the curve invariants.
func (c *Curve) Validate() error {
	if len(c.Keys) == 0 {
		return fmt.Errorf("curve has no points")
	}
	if len(c.Keys) != len(c.Values) {
		return fmt.Errorf("curve keys and values differ in length")
	}
	if len(c.Keys) > MaxCurvePoints {
		return fmt.Errorf("curve has more than %d points", MaxCurvePoints)
	}
	for i := range c.Keys {
		k, v := c.Keys[i], c.Values[i]
		if math.IsInf(k, 0) || math.IsNaN(k) || math.IsInf(v, 0) || math.IsNaN(v) {
			return fmt.Errorf("curve point %d is not finite", i)
		}
		if i > 0 && !(c.Keys[i-1] < k) {
			return fmt.Errorf("curve keys must increase strictly (point %d)", i)
		}
	}
	return nil
}

// Sample evaluates a valid curve: clamped at the ends, y = v[i] + (v[i+1]-v[i]) * t inside.
func (c *Curve) Sample(x float64) float64 {
	keys, values := c.Keys, c.Values
	n := len(keys)
	if math.IsNaN(x) {
		return canonicalNaN()
	}
	if x <= keys[0] {
		return values[0]
	}
	if x >= keys[n-1] {
		return values[n-1]
	}
	lo, hi := 0, n-1
	for hi-lo > 1 {
		mid := lo + (hi-lo)/2
		if keys[mid] <= x {
			lo = mid
		} else {
			hi = mid
		}
	}
	t := (x - keys[lo]) / (keys[hi] - keys[lo])
	d := values[hi] - values[lo]
	p := float64(d * t)
	return values[lo] + p
}

// Min is HXL's min: NaN-propagating, -0 < +0.
func Min(a, b float64) float64 {
	if math.IsNaN(a) || math.IsNaN(b) {
		return canonicalNaN()
	}
	if a < b {
		return a
	}
	if b < a {
		return b
	}
	if math.Signbit(a) {
		return a
	}
	return b
}

// Max is HXL's max: NaN-propagating, -0 < +0.
func Max(a, b float64) float64 {
	if math.IsNaN(a) || math.IsNaN(b) {
		return canonicalNaN()
	}
	if a > b {
		return a
	}
	if b > a {
		return b
	}
	if math.Signbit(a) {
		return b
	}
	return a
}

// Value is an evaluation result; bools are 0 / 1 in Number.
type Value struct {
	Type   Type
	Number float64
}

// Bool reports a bool result.
func (v Value) Bool() bool { return v.Number != 0 }

// Env supplies the inputs of one evaluation. Symbols index the program's symbol tables; returning
// ok=false reports E_MISSING_INPUT. A nil Curve is a missing curve.
type Env interface {
	Attr(param, symbol int) (float64, bool)
	Field(param, symbol int) (float64, bool)
	Tag(param, symbol int) (bool, bool)
	Curve(symbol int) *Curve
	Stacks() (float64, bool)
	Level() (float64, bool)
}

func boolNum(b bool) float64 {
	if b {
		return 1
	}
	return 0
}

// Eval runs a verified program. It never allocates. It fails with E_MISSING_INPUT, or with
// E_BYTECODE for a zero Program (every compiled or decoded program has code). An Env's Curve must
// return valid curves (Curve.Validate).
func (p *Program) Eval(env Env) (Value, Status) {
	code := p.code
	consts := p.constants
	if len(code) == 0 {
		return Value{}, EBytecode
	}
	var stack [MaxStack]float64
	sp, pc := 0, 0
	for pc < len(code) {
		switch Op(code[pc]) {
		case OpConst:
			stack[sp] = consts[readU16(code[pc+1:])]
			sp++
			pc += 3
		case OpTrue:
			stack[sp] = 1
			sp++
			pc++
		case OpFalse:
			stack[sp] = 0
			sp++
			pc++
		case OpAttr:
			v, ok := env.Attr(int(code[pc+1]), readU16(code[pc+2:]))
			if !ok {
				return Value{}, EMissingInput
			}
			stack[sp] = v
			sp++
			pc += 4
		case OpField:
			v, ok := env.Field(int(code[pc+1]), readU16(code[pc+2:]))
			if !ok {
				return Value{}, EMissingInput
			}
			stack[sp] = v
			sp++
			pc += 4
		case OpTag:
			b, ok := env.Tag(int(code[pc+1]), readU16(code[pc+2:]))
			if !ok {
				return Value{}, EMissingInput
			}
			stack[sp] = boolNum(b)
			sp++
			pc += 4
		case OpStacks:
			v, ok := env.Stacks()
			if !ok {
				return Value{}, EMissingInput
			}
			stack[sp] = v
			sp++
			pc++
		case OpLevel:
			v, ok := env.Level()
			if !ok {
				return Value{}, EMissingInput
			}
			stack[sp] = v
			sp++
			pc++
		case OpCurve:
			c := env.Curve(readU16(code[pc+1:]))
			if c == nil {
				return Value{}, EMissingInput
			}
			stack[sp-1] = c.Sample(stack[sp-1])
			pc += 3
		case OpNeg:
			stack[sp-1] = -stack[sp-1]
			pc++
		case OpAdd:
			sp--
			stack[sp-1] = stack[sp-1] + stack[sp]
			pc++
		case OpSub:
			sp--
			stack[sp-1] = stack[sp-1] - stack[sp]
			pc++
		case OpMul:
			sp--
			stack[sp-1] = float64(stack[sp-1] * stack[sp])
			pc++
		case OpDiv:
			sp--
			stack[sp-1] = stack[sp-1] / stack[sp]
			pc++
		case OpPow:
			sp--
			stack[sp-1] = det.Pow(stack[sp-1], stack[sp])
			pc++
		case OpMin:
			sp--
			stack[sp-1] = Min(stack[sp-1], stack[sp])
			pc++
		case OpMax:
			sp--
			stack[sp-1] = Max(stack[sp-1], stack[sp])
			pc++
		case OpClamp:
			sp -= 2
			stack[sp-1] = Min(Max(stack[sp-1], stack[sp]), stack[sp+1])
			pc++
		case OpLerp:
			sp -= 2
			a, b, t := stack[sp-1], stack[sp], stack[sp+1]
			d := b - a
			q := float64(d * t)
			stack[sp-1] = a + q
			pc++
		case OpSqrt:
			stack[sp-1] = math.Sqrt(stack[sp-1])
			pc++
		case OpExp:
			stack[sp-1] = det.Exp(stack[sp-1])
			pc++
		case OpLn:
			stack[sp-1] = det.Ln(stack[sp-1])
			pc++
		case OpAsinh:
			stack[sp-1] = det.Asinh(stack[sp-1])
			pc++
		case OpAbs:
			stack[sp-1] = math.Abs(stack[sp-1])
			pc++
		case OpFloor:
			stack[sp-1] = math.Floor(stack[sp-1])
			pc++
		case OpCeil:
			stack[sp-1] = math.Ceil(stack[sp-1])
			pc++
		case OpLt:
			sp--
			stack[sp-1] = boolNum(stack[sp-1] < stack[sp])
			pc++
		case OpLe:
			sp--
			stack[sp-1] = boolNum(stack[sp-1] <= stack[sp])
			pc++
		case OpGt:
			sp--
			stack[sp-1] = boolNum(stack[sp-1] > stack[sp])
			pc++
		case OpGe:
			sp--
			stack[sp-1] = boolNum(stack[sp-1] >= stack[sp])
			pc++
		case OpEqN, OpEqB:
			sp--
			stack[sp-1] = boolNum(stack[sp-1] == stack[sp])
			pc++
		case OpNeN, OpNeB:
			sp--
			stack[sp-1] = boolNum(stack[sp-1] != stack[sp])
			pc++
		case OpNot:
			stack[sp-1] = boolNum(stack[sp-1] == 0)
			pc++
		case OpJumpIfFalse:
			sp--
			pc += 3
			if stack[sp] == 0 {
				pc += readU16(code[pc-2:])
			}
		case OpJump:
			pc += 3 + readU16(code[pc+1:])
		default:
			return Value{}, EBytecode // unreachable for verified programs
		}
	}
	r := stack[0]
	if math.IsNaN(r) {
		r = canonicalNaN()
	}
	return Value{Type: p.resultType, Number: r}, OK
}

// Evaluate is Eval returning an error (*Diagnostic) instead of a status.
func (p *Program) Evaluate(env Env) (Value, error) {
	v, s := p.Eval(env)
	if s != OK {
		msg := "the environment has no value for an input"
		if s != EMissingInput {
			msg = "invalid program (zero value or unverified)"
		}
		return v, &Diagnostic{Status: s, Message: msg}
	}
	return v, nil
}

// MapEnv is a name-keyed environment for tools, tests and the corpus. Bind it to the program after
// filling the maps.
type MapEnv struct {
	Attrs     map[string]map[string]float64 // param -> attribute -> value
	Fields    map[string]map[string]float64 // param -> field -> value
	Tags      map[string][]string           // param -> tags; tag(p, T) matches T and T.*
	Curves    map[string]*Curve
	HasStacks bool
	StacksVal float64
	HasLevel  bool
	LevelVal  float64

	attrSlots  [][]slot
	fieldSlots [][]slot
	tagSlots   [][]bool
	curveSlots []*Curve
}

type slot struct {
	present bool
	value   float64
}

// Bind resolves the program's symbols against the maps. Curves that fail Validate are bound as
// missing.
func (e *MapEnv) Bind(p *Program) {
	fill := func(src map[string]map[string]float64, param string, symbols []string) []slot {
		out := make([]slot, len(symbols))
		m := src[param]
		for i, s := range symbols {
			if v, ok := m[s]; ok {
				out[i] = slot{true, v}
			}
		}
		return out
	}
	e.attrSlots = make([][]slot, len(p.params))
	e.fieldSlots = make([][]slot, len(p.params))
	e.tagSlots = make([][]bool, len(p.params))
	for i, param := range p.params {
		e.attrSlots[i] = fill(e.Attrs, param, p.attrs)
		e.fieldSlots[i] = fill(e.Fields, param, p.fields)
		e.tagSlots[i] = make([]bool, len(p.tags))
		for s, want := range p.tags {
			for _, have := range e.Tags[param] {
				if have == want || (strings.HasPrefix(have, want) && len(have) > len(want) && have[len(want)] == '.') {
					e.tagSlots[i][s] = true
					break
				}
			}
		}
	}
	e.curveSlots = make([]*Curve, len(p.curves))
	for i, name := range p.curves {
		// Sample requires a valid curve (an empty or ragged one would panic): an invalid curve is
		// reported like a missing one, E_MISSING_INPUT (same as C++).
		if c := e.Curves[name]; c != nil && c.Validate() == nil {
			e.curveSlots[i] = c
		}
	}
}

func (e *MapEnv) Attr(param, symbol int) (float64, bool) {
	if param >= len(e.attrSlots) || symbol >= len(e.attrSlots[param]) {
		return 0, false
	}
	s := e.attrSlots[param][symbol]
	return s.value, s.present
}

func (e *MapEnv) Field(param, symbol int) (float64, bool) {
	if param >= len(e.fieldSlots) || symbol >= len(e.fieldSlots[param]) {
		return 0, false
	}
	s := e.fieldSlots[param][symbol]
	return s.value, s.present
}

func (e *MapEnv) Tag(param, symbol int) (bool, bool) {
	if param >= len(e.tagSlots) || symbol >= len(e.tagSlots[param]) {
		return false, false
	}
	return e.tagSlots[param][symbol], true
}

func (e *MapEnv) Curve(symbol int) *Curve {
	if symbol >= len(e.curveSlots) {
		return nil
	}
	return e.curveSlots[symbol]
}

func (e *MapEnv) Stacks() (float64, bool) { return e.StacksVal, e.HasStacks }
func (e *MapEnv) Level() (float64, bool)  { return e.LevelVal, e.HasLevel }
