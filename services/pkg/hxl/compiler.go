package hxl

import (
	"fmt"
	"math"
	"strings"
)

// Type checker and code generator. Mirrors engine/hxl/src/compiler.cpp: the same checks in the same
// order (children left to right, then the node), the same symbol/constant registration order and the
// same op sequence, so both produce byte-identical bytecode (the corpus pins the hashes).

// CompileOptions configures Compile.
type CompileOptions struct {
	// Params are the parameters of a bare expression (ignored for a `formula` declaration).
	// nil means ["self"].
	Params []string
	// ExpectedType, when non-nil, makes a program of another type fail with E_RESULT_TYPE.
	ExpectedType *Type
	// MaxCost is the static cost budget; 0 or anything above MaxCost means MaxCost.
	MaxCost int
}

type builtinID uint8

const (
	bAttr builtinID = iota
	bTag
	bCurve
	bStacks
	bLevel
	bSelect
	bMin
	bMax
	bClamp
	bLerp
	bPow
	bSqrt
	bExp
	bLn
	bAsinh
	bAbs
	bFloor
	bCeil
)

type builtin struct {
	name             string
	id               builtinID
	minArgs, maxArgs int // maxArgs variadic = unbounded
}

const variadic = -1

var builtins = []builtin{
	{"attr", bAttr, 2, 2}, {"tag", bTag, 2, 2}, {"curve", bCurve, 2, 2}, {"stacks", bStacks, 0, 0},
	{"level", bLevel, 0, 0}, {"select", bSelect, 3, 3}, {"min", bMin, 2, variadic}, {"max", bMax, 2, variadic},
	{"clamp", bClamp, 3, 3}, {"lerp", bLerp, 3, 3}, {"pow", bPow, 2, 2}, {"sqrt", bSqrt, 1, 1},
	{"exp", bExp, 1, 1}, {"ln", bLn, 1, 1}, {"asinh", bAsinh, 1, 1}, {"abs", bAbs, 1, 1},
	{"floor", bFloor, 1, 1}, {"ceil", bCeil, 1, 1},
}

var unaryOps = [...]Op{OpSqrt, OpExp, OpLn, OpAsinh, OpAbs, OpFloor, OpCeil}

func findBuiltin(name string) *builtin {
	for i := range builtins {
		if builtins[i].name == name {
			return &builtins[i]
		}
	}
	return nil
}

type gen struct {
	ast       *ast
	params    []string
	code      []byte
	constants []float64
	attrs     []string
	tags      []string
	fields    []string
	curves    []string
}

func (g *gen) failAt(n *node, s Status, format string, args ...any) *Diagnostic {
	return diag(s, n.line, n.column, format, args...)
}

func (g *gen) failAtStart(n *node, s Status, format string, args ...any) *Diagnostic {
	return diag(s, n.startLine, n.startColumn, format, args...)
}

func (g *gen) op(o Op) { g.code = append(g.code, byte(o)) }
func (g *gen) u16(v int) {
	g.code = append(g.code, byte(v), byte(v>>8))
}

func (g *gen) jump(o Op) int {
	g.op(o)
	at := len(g.code)
	g.u16(0)
	return at
}

func (g *gen) patch(at int) {
	offset := len(g.code) - (at + 2)
	g.code[at] = byte(offset)
	g.code[at+1] = byte(offset >> 8)
}

func (g *gen) paramIndex(name string) int {
	for i, p := range g.params {
		if p == name {
			return i
		}
	}
	return -1
}

func (g *gen) symbol(table *[]string, name, what string, at *node) (int, *Diagnostic) {
	if len(name) > MaxNameBytes {
		return 0, g.failAtStart(at, ELimit, "%s name longer than %d bytes", what, MaxNameBytes)
	}
	for i, s := range *table {
		if s == name {
			return i, nil
		}
	}
	if len(*table) >= MaxSymbols {
		return 0, g.failAtStart(at, ELimit, "more than %d %s names", MaxSymbols, what)
	}
	*table = append(*table, name)
	return len(*table) - 1, nil
}

func (g *gen) constant(v float64, at *node) *Diagnostic {
	bits := math.Float64bits(v)
	for i, c := range g.constants {
		if math.Float64bits(c) == bits {
			g.op(OpConst)
			g.u16(i)
			return nil
		}
	}
	if len(g.constants) >= MaxConstants {
		return g.failAt(at, ELimit, "more than %d distinct constants", MaxConstants)
	}
	g.constants = append(g.constants, v)
	g.op(OpConst)
	g.u16(len(g.constants) - 1)
	return nil
}

func (g *gen) emitNumber(index int, what string) *Diagnostic {
	t, d := g.emit(index)
	if d != nil {
		return d
	}
	if t != Number {
		return g.failAtStart(&g.ast.nodes[index], EType, "%s must be a number, found bool", what)
	}
	return nil
}

func (g *gen) emitPath(n *node) (Type, *Diagnostic) {
	segs := n.segments
	p := g.paramIndex(segs[0])
	if len(segs) == 1 {
		if p >= 0 {
			return 0, g.failAt(n, EType, "parameter '%s' is an entity: read a field (%s.name), attr() or tag()", segs[0], segs[0])
		}
		return 0, g.failAt(n, EUnknownName, "unknown name '%s'", segs[0])
	}
	if p < 0 {
		return 0, g.failAt(n, EUnknownName, "unknown parameter '%s'", segs[0])
	}
	if len(segs) > 2 {
		return 0, g.failAt(n, ESyntax, "context fields have one level ('%s.%s')", segs[0], segs[1])
	}
	sym, d := g.symbol(&g.fields, segs[1], "field", n)
	if d != nil {
		return 0, d
	}
	g.op(OpField)
	g.code = append(g.code, byte(p))
	g.u16(sym)
	return Number, nil
}

func (g *gen) emitEntityRef(call *node, o Op, fn string) *Diagnostic {
	e := &g.ast.nodes[call.children[0]]
	if e.kind != nPath || len(e.segments) != 1 {
		return g.failAtStart(e, EEntityArg, "the first argument of %s() must be a parameter name", fn)
	}
	p := g.paramIndex(e.segments[0])
	if p < 0 {
		return g.failAtStart(e, EUnknownName, "unknown parameter '%s'", e.segments[0])
	}
	s := &g.ast.nodes[call.children[1]]
	kind, table := "attribute", &g.attrs
	if o == OpTag {
		kind, table = "tag", &g.tags
	}
	if s.kind != nPath {
		return g.failAtStart(s, ESymbolArg, "the second argument of %s() must be a %s name", fn, kind)
	}
	sym, d := g.symbol(table, strings.Join(s.segments, "."), kind, s)
	if d != nil {
		return d
	}
	g.op(o)
	g.code = append(g.code, byte(p))
	g.u16(sym)
	return nil
}

func (g *gen) emitCall(n *node) (Type, *Diagnostic) {
	fn := findBuiltin(n.name)
	if fn == nil {
		return 0, g.failAt(n, EUnknownFunction, "unknown function '%s'", n.name)
	}
	argc := len(n.children)
	if argc < fn.minArgs || (fn.maxArgs != variadic && argc > fn.maxArgs) {
		// Same wording as C++ (arityError).
		expected := fmt.Sprintf("%d to %d", fn.minArgs, fn.maxArgs)
		if fn.maxArgs == variadic {
			expected = fmt.Sprintf("at least %d", fn.minArgs)
		} else if fn.minArgs == fn.maxArgs {
			expected = fmt.Sprintf("%d", fn.minArgs)
		}
		return 0, g.failAt(n, EArity, "%s() takes %s argument(s), got %d", n.name, expected, argc)
	}
	args := n.children
	switch fn.id {
	case bAttr:
		return Number, g.emitEntityRef(n, OpAttr, "attr")
	case bTag:
		return Bool, g.emitEntityRef(n, OpTag, "tag")
	case bCurve:
		s := &g.ast.nodes[args[0]]
		if s.kind != nPath {
			return 0, g.failAtStart(s, ESymbolArg, "the first argument of curve() must be a curve name")
		}
		if d := g.emitNumber(args[1], "the curve input"); d != nil {
			return 0, d
		}
		sym, d := g.symbol(&g.curves, strings.Join(s.segments, "."), "curve", s)
		if d != nil {
			return 0, d
		}
		g.op(OpCurve)
		g.u16(sym)
		return Number, nil
	case bStacks:
		g.op(OpStacks)
		return Number, nil
	case bLevel:
		g.op(OpLevel)
		return Number, nil
	case bSelect:
		c, d := g.emit(args[0])
		if d != nil {
			return 0, d
		}
		if c != Bool {
			return 0, g.failAtStart(&g.ast.nodes[args[0]], EType, "the condition of select() must be bool")
		}
		elseJump := g.jump(OpJumpIfFalse)
		a, d := g.emit(args[1])
		if d != nil {
			return 0, d
		}
		endJump := g.jump(OpJump)
		g.patch(elseJump)
		b, d := g.emit(args[2])
		if d != nil {
			return 0, d
		}
		if b != a {
			return 0, g.failAtStart(&g.ast.nodes[args[2]], EType, "select() branches differ in type (%s and %s)", a, b)
		}
		g.patch(endJump)
		return a, nil
	case bMin, bMax:
		o := OpMin
		if fn.id == bMax {
			o = OpMax
		}
		if d := g.emitNumber(args[0], "an argument of min()/max()"); d != nil {
			return 0, d
		}
		for _, a := range args[1:] {
			if d := g.emitNumber(a, "an argument of min()/max()"); d != nil {
				return 0, d
			}
			g.op(o)
		}
		return Number, nil
	case bClamp, bLerp, bPow:
		for _, a := range args {
			if d := g.emitNumber(a, "an argument of "+n.name+"()"); d != nil {
				return 0, d
			}
		}
		switch fn.id {
		case bClamp:
			g.op(OpClamp)
		case bLerp:
			g.op(OpLerp)
		default:
			g.op(OpPow)
		}
		return Number, nil
	default:
		if d := g.emitNumber(args[0], "the argument of "+n.name+"()"); d != nil {
			return 0, d
		}
		g.op(unaryOps[fn.id-bSqrt])
		return Number, nil
	}
}

func (g *gen) emitBinary(n *node) (Type, *Diagnostic) {
	l, r := n.children[0], n.children[1]
	if n.op == tAndAnd || n.op == tOrOr {
		opText := "&&"
		if n.op == tOrOr {
			opText = "||"
		}
		tl, d := g.emit(l)
		if d != nil {
			return 0, d
		}
		if tl != Bool {
			return 0, g.failAt(n, EType, "operands of '%s' must be bool", opText)
		}
		skip := g.jump(OpJumpIfFalse)
		if n.op == tAndAnd {
			tr, d := g.emit(r)
			if d != nil {
				return 0, d
			}
			if tr != Bool {
				return 0, g.failAt(n, EType, "operands of '&&' must be bool")
			}
			end := g.jump(OpJump)
			g.patch(skip)
			g.op(OpFalse)
			g.patch(end)
		} else {
			g.op(OpTrue)
			end := g.jump(OpJump)
			g.patch(skip)
			tr, d := g.emit(r)
			if d != nil {
				return 0, d
			}
			if tr != Bool {
				return 0, g.failAt(n, EType, "operands of '||' must be bool")
			}
			g.patch(end)
		}
		return Bool, nil
	}
	tl, d := g.emit(l)
	if d != nil {
		return 0, d
	}
	tr, d := g.emit(r)
	if d != nil {
		return 0, d
	}
	if n.op == tEqEq || n.op == tNotEq {
		if tl != tr {
			return 0, g.failAt(n, EType, "cannot compare %s with %s", tl, tr)
		}
		switch {
		case tl == Number && n.op == tEqEq:
			g.op(OpEqN)
		case tl == Number:
			g.op(OpNeN)
		case n.op == tEqEq:
			g.op(OpEqB)
		default:
			g.op(OpNeB)
		}
		return Bool, nil
	}
	if tl != Number || tr != Number {
		return 0, g.failAt(n, EType, "operands of %s must be numbers", n.op)
	}
	switch n.op {
	case tPlus:
		g.op(OpAdd)
	case tMinus:
		g.op(OpSub)
	case tStar:
		g.op(OpMul)
	case tSlash:
		g.op(OpDiv)
	case tCaret:
		g.op(OpPow)
	case tLt:
		g.op(OpLt)
		return Bool, nil
	case tLe:
		g.op(OpLe)
		return Bool, nil
	case tGt:
		g.op(OpGt)
		return Bool, nil
	case tGe:
		g.op(OpGe)
		return Bool, nil
	default:
		return 0, g.failAt(n, ESyntax, "unsupported operator")
	}
	return Number, nil
}

func (g *gen) emit(index int) (Type, *Diagnostic) {
	n := &g.ast.nodes[index]
	switch n.kind {
	case nNumber:
		return Number, g.constant(n.number, n)
	case nBool:
		if n.boolean {
			g.op(OpTrue)
		} else {
			g.op(OpFalse)
		}
		return Bool, nil
	case nPath:
		return g.emitPath(n)
	case nCall:
		return g.emitCall(n)
	case nUnary:
		t, d := g.emit(n.children[0])
		if d != nil {
			return 0, d
		}
		if n.op == tMinus {
			if t != Number {
				return 0, g.failAt(n, EType, "operand of unary '-' must be a number")
			}
			g.op(OpNeg)
			return Number, nil
		}
		if t != Bool {
			return 0, g.failAt(n, EType, "operand of '!' must be bool")
		}
		g.op(OpNot)
		return Bool, nil
	case nBinary:
		return g.emitBinary(n)
	}
	return 0, g.failAt(n, ESyntax, "unknown node")
}

// Compile compiles HXL source (grammar in doc.go). Errors are *Diagnostic values.
func Compile(source string, opts CompileOptions) (*Program, error) {
	if len(source) > MaxSourceBytes {
		return nil, diag(ELimit, 1, 1, "source longer than %d bytes", MaxSourceBytes)
	}
	toks, d := lex(source)
	if d != nil {
		return nil, d
	}
	a, d := parse(toks)
	if d != nil {
		return nil, d
	}
	var params []string
	if a.isFormula {
		params = append([]string{}, a.params...)
	} else {
		hostParams := opts.Params
		if hostParams == nil {
			hostParams = []string{"self"}
		}
		for i := range hostParams {
			// Host parameter names end up in the bytecode, whose verifier accepts only identifiers.
			if !isIdentifier(hostParams[i]) {
				return nil, diag(ESyntax, 1, 1, "invalid parameter name '%s'", hostParams[i])
			}
			for j := 0; j < i; j++ {
				if hostParams[i] == hostParams[j] {
					return nil, diag(EDuplicateParam, 1, 1, "duplicate parameter '%s'", hostParams[i])
				}
			}
		}
		if len(hostParams) > MaxParams {
			return nil, diag(ELimit, 1, 1, "more than %d parameters", MaxParams)
		}
		params = append([]string{}, hostParams...)
	}
	g := &gen{ast: a, params: params}
	t, d := g.emit(a.root)
	if d != nil {
		return nil, d
	}
	if opts.ExpectedType != nil && *opts.ExpectedType != t {
		root := &a.nodes[a.root]
		return nil, diag(EResultType, root.startLine, root.startColumn, "expected a %s expression, found %s",
			*opts.ExpectedType, t)
	}
	name := ""
	if a.isFormula {
		name = a.formulaName
	}
	p := &Program{name: name, params: params, constants: g.constants, attrs: g.attrs, tags: g.tags,
		fields: g.fields, curves: g.curves, code: g.code}
	maxCost := opts.MaxCost
	if maxCost <= 0 || maxCost > MaxCost {
		maxCost = MaxCost
	}
	if d := p.finish(maxCost); d != nil {
		if d.Status != ELimit {
			d.Message = fmt.Sprintf("internal compiler error: %s", d.Message)
		}
		return nil, d
	}
	return p, nil
}
