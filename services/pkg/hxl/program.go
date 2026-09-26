package hxl

import (
	"errors"
	"fmt"
	"math"
	"strings"
)

// Type is the static type of an HXL value. Booleans travel on the VM stack as 0 / 1.
type Type uint8

const (
	Number Type = 0
	Bool   Type = 1
)

func (t Type) String() string {
	if t == Bool {
		return "bool"
	}
	return "num"
}

// Status is a stable status code shared with C++ (helios::hxl::Status) and the corpus.
type Status uint16

const (
	OK Status = iota
	ELex
	ENumber
	ESyntax
	EUnknownName
	EUnknownFunction
	EArity
	EType
	EEntityArg
	ESymbolArg
	EDuplicateParam
	ELimit
	EResultType
	EBytecode
	EMissingInput
)

var statusNames = [...]string{
	"OK", "E_LEX", "E_NUMBER", "E_SYNTAX", "E_UNKNOWN_NAME", "E_UNKNOWN_FUNCTION", "E_ARITY", "E_TYPE",
	"E_ENTITY_ARG", "E_SYMBOL_ARG", "E_DUPLICATE_PARAM", "E_LIMIT", "E_RESULT_TYPE", "E_BYTECODE",
	"E_MISSING_INPUT",
}

func (s Status) String() string {
	if int(s) < len(statusNames) {
		return statusNames[s]
	}
	return "E_UNKNOWN"
}

// StatusFromName is the inverse of Status.String (OK if unknown).
func StatusFromName(name string) Status {
	for i, n := range statusNames {
		if n == name {
			return Status(i)
		}
	}
	return OK
}

// Diagnostic is a compile, load or evaluation failure with a 1-based source position (0:0 when not
// source related). It implements error.
type Diagnostic struct {
	Status  Status
	Line    int
	Column  int
	Message string
}

func (d *Diagnostic) Error() string {
	return fmt.Sprintf("%s %d:%d: %s", d.Status, d.Line, d.Column, d.Message)
}

// AsDiagnostic extracts the Diagnostic from an error returned by this package.
func AsDiagnostic(err error) (*Diagnostic, bool) {
	var d *Diagnostic
	ok := errors.As(err, &d)
	return d, ok
}

// Op is an opcode; operands are little-endian and follow the opcode byte.
type Op uint8

const (
	OpConst       Op = 0x01 // u16 constant
	OpTrue        Op = 0x02
	OpFalse       Op = 0x03
	OpAttr        Op = 0x04 // u8 param, u16 attribute symbol
	OpField       Op = 0x05 // u8 param, u16 field symbol
	OpTag         Op = 0x06 // u8 param, u16 tag symbol
	OpStacks      Op = 0x07
	OpLevel       Op = 0x08
	OpCurve       Op = 0x09 // u16 curve symbol; pops x
	OpNeg         Op = 0x10
	OpAdd         Op = 0x11
	OpSub         Op = 0x12
	OpMul         Op = 0x13
	OpDiv         Op = 0x14
	OpPow         Op = 0x15
	OpMin         Op = 0x16
	OpMax         Op = 0x17
	OpClamp       Op = 0x18
	OpLerp        Op = 0x19
	OpSqrt        Op = 0x1A
	OpExp         Op = 0x1B
	OpLn          Op = 0x1C
	OpAsinh       Op = 0x1D
	OpAbs         Op = 0x1E
	OpFloor       Op = 0x1F
	OpCeil        Op = 0x20
	OpLt          Op = 0x30
	OpLe          Op = 0x31
	OpGt          Op = 0x32
	OpGe          Op = 0x33
	OpEqN         Op = 0x34
	OpNeN         Op = 0x35
	OpEqB         Op = 0x36
	OpNeB         Op = 0x37
	OpNot         Op = 0x38
	OpJumpIfFalse Op = 0x40 // u16 forward offset from the next op; pops the condition
	OpJump        Op = 0x41 // u16 forward offset from the next op
)

// Limits shared with C++ (helios::hxl::limits).
const (
	MaxSourceBytes = 65536
	MaxParseDepth  = 128
	MaxAstDepth    = 256
	MaxParams      = 16
	MaxOps         = 4096
	MaxCodeBytes   = 16384
	MaxConstants   = 1024
	MaxSymbols     = 256
	MaxNameBytes   = 256
	MaxNumberBytes = 100 // longest number literal (strconv.ParseFloat is exact only up to 800 digits)
	MaxStack       = 256
	MaxCost        = 4096
	MaxCurvePoints = 4096
)

type opInfo struct {
	name         string // "" = invalid opcode
	operandBytes int
	pops         int
	in           Type
	pushes       bool
	out          Type
	cost         int
}

var opTable = func() [256]opInfo {
	var t [256]opInfo
	set := func(op Op, name string, operandBytes, pops int, in Type, pushes bool, out Type, cost int) {
		t[op] = opInfo{name, operandBytes, pops, in, pushes, out, cost}
	}
	N, B := Number, Bool
	set(OpConst, "Const", 2, 0, N, true, N, 1)
	set(OpTrue, "True", 0, 0, N, true, B, 1)
	set(OpFalse, "False", 0, 0, N, true, B, 1)
	set(OpAttr, "Attr", 3, 0, N, true, N, 2)
	set(OpField, "Field", 3, 0, N, true, N, 2)
	set(OpTag, "Tag", 3, 0, N, true, B, 2)
	set(OpStacks, "Stacks", 0, 0, N, true, N, 1)
	set(OpLevel, "Level", 0, 0, N, true, N, 1)
	set(OpCurve, "Curve", 2, 1, N, true, N, 4)
	set(OpNeg, "Neg", 0, 1, N, true, N, 1)
	set(OpAdd, "Add", 0, 2, N, true, N, 1)
	set(OpSub, "Sub", 0, 2, N, true, N, 1)
	set(OpMul, "Mul", 0, 2, N, true, N, 1)
	set(OpDiv, "Div", 0, 2, N, true, N, 1)
	set(OpPow, "Pow", 0, 2, N, true, N, 8)
	set(OpMin, "Min", 0, 2, N, true, N, 1)
	set(OpMax, "Max", 0, 2, N, true, N, 1)
	set(OpClamp, "Clamp", 0, 3, N, true, N, 1)
	set(OpLerp, "Lerp", 0, 3, N, true, N, 1)
	set(OpSqrt, "Sqrt", 0, 1, N, true, N, 2)
	set(OpExp, "Exp", 0, 1, N, true, N, 8)
	set(OpLn, "Ln", 0, 1, N, true, N, 8)
	set(OpAsinh, "Asinh", 0, 1, N, true, N, 8)
	set(OpAbs, "Abs", 0, 1, N, true, N, 1)
	set(OpFloor, "Floor", 0, 1, N, true, N, 1)
	set(OpCeil, "Ceil", 0, 1, N, true, N, 1)
	set(OpLt, "Lt", 0, 2, N, true, B, 1)
	set(OpLe, "Le", 0, 2, N, true, B, 1)
	set(OpGt, "Gt", 0, 2, N, true, B, 1)
	set(OpGe, "Ge", 0, 2, N, true, B, 1)
	set(OpEqN, "EqN", 0, 2, N, true, B, 1)
	set(OpNeN, "NeN", 0, 2, N, true, B, 1)
	set(OpEqB, "EqB", 0, 2, B, true, B, 1)
	set(OpNeB, "NeB", 0, 2, B, true, B, 1)
	set(OpNot, "Not", 0, 1, B, true, B, 1)
	set(OpJumpIfFalse, "JumpIfFalse", 2, 1, B, false, N, 1)
	set(OpJump, "Jump", 2, 0, N, false, N, 1)
	return t
}()

// OpCost is the static evaluation cost of one op (the unit of MaxCost).
func OpCost(op Op) int { return opTable[op].cost }

// Program is a verified HXL program: immutable after Compile or Decode; safe for concurrent use.
type Program struct {
	name       string
	params     []string
	resultType Type
	maxStack   int
	cost       int
	constants  []float64
	attrs      []string
	tags       []string
	fields     []string
	curves     []string
	code       []byte
	usesStacks bool
	usesLevel  bool
}

// Accessors. The returned slices are the program's own verified tables: callers must not modify
// them (a Program is shared between goroutines, and Eval trusts the verified code).

func (p *Program) Name() string           { return p.name }
func (p *Program) Params() []string       { return p.params }
func (p *Program) ResultType() Type       { return p.resultType }
func (p *Program) MaxStack() int          { return p.maxStack }
func (p *Program) Cost() int              { return p.cost }
func (p *Program) Constants() []float64   { return p.constants }
func (p *Program) AttrSymbols() []string  { return p.attrs }
func (p *Program) TagSymbols() []string   { return p.tags }
func (p *Program) FieldSymbols() []string { return p.fields }
func (p *Program) CurveSymbols() []string { return p.curves }
func (p *Program) Code() []byte           { return p.code }
func (p *Program) UsesStacks() bool       { return p.usesStacks }
func (p *Program) UsesLevel() bool        { return p.usesLevel }

func readU16(b []byte) int { return int(b[0]) | int(b[1])<<8 }

func bytecodeErr(format string, args ...any) *Diagnostic {
	return &Diagnostic{Status: EBytecode, Message: fmt.Sprintf(format, args...)}
}

// Size limits are E_LIMIT at 1:1 so the compiler reports them directly; Decode turns them into
// E_BYTECODE.
func limitErr(format string, args ...any) *Diagnostic {
	return &Diagnostic{Status: ELimit, Line: 1, Column: 1, Message: fmt.Sprintf(format, args...)}
}

func checkNames(names []string, dotted bool, what string) *Diagnostic {
	if len(names) > MaxSymbols {
		return bytecodeErr("too many %s symbols", what)
	}
	for i, n := range names {
		ok := isIdentifier(n)
		if dotted {
			ok = isDottedName(n)
		}
		if !ok {
			return bytecodeErr("invalid %s name '%s'", what, n)
		}
		for j := 0; j < i; j++ {
			if names[j] == n {
				return bytecodeErr("duplicate %s name '%s'", what, n)
			}
		}
	}
	return nil
}

type useOrder struct{ next int }

func (u *useOrder) use(i int) bool {
	if i == u.next {
		u.next++
		return true
	}
	return i < u.next
}

func sameStack(a, b []Type) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// finish verifies the program (mirror of C++ ProgramBuilder::finish) and fills the derived fields.
func (p *Program) finish(maxCost int) *Diagnostic {
	if p.name != "" && !isIdentifier(p.name) {
		return bytecodeErr("invalid formula name")
	}
	if len(p.params) > MaxParams {
		return bytecodeErr("too many parameters")
	}
	for _, c := range []struct {
		names  []string
		dotted bool
		what   string
	}{{p.params, false, "parameter"}, {p.attrs, true, "attribute"}, {p.tags, true, "tag"},
		{p.fields, false, "field"}, {p.curves, true, "curve"}} {
		if d := checkNames(c.names, c.dotted, c.what); d != nil {
			return d
		}
	}
	if len(p.constants) > MaxConstants {
		return bytecodeErr("too many constants")
	}
	for i, c := range p.constants {
		if math.IsInf(c, 0) || math.IsNaN(c) || math.Signbit(c) {
			return bytecodeErr("constants must be finite and non-negative")
		}
		for j := 0; j < i; j++ {
			if math.Float64bits(p.constants[j]) == math.Float64bits(c) {
				return bytecodeErr("duplicate constant")
			}
		}
	}
	code := p.code
	if len(code) > MaxCodeBytes {
		return limitErr("code larger than %d bytes", MaxCodeBytes)
	}
	stack := make([]Type, 0, 16)
	pending := map[int][]Type{}
	reachable := true
	maxStack, cost, ops := 0, 0, 0
	usesStacks, usesLevel := false, false
	var constOrder, attrOrder, tagOrder, fieldOrder, curveOrder useOrder

	join := func(at int) *Diagnostic {
		st, ok := pending[at]
		if !ok {
			return nil
		}
		if reachable {
			if !sameStack(st, stack) {
				return bytecodeErr("stack mismatch at jump target %d", at)
			}
		} else {
			stack = append(stack[:0], st...)
			reachable = true
		}
		delete(pending, at)
		return nil
	}
	addPending := func(target int) *Diagnostic {
		if st, ok := pending[target]; ok {
			if !sameStack(st, stack) {
				return bytecodeErr("stack mismatch at jump target %d", target)
			}
			return nil
		}
		pending[target] = append([]Type(nil), stack...)
		return nil
	}

	pc := 0
	for pc < len(code) {
		if d := join(pc); d != nil {
			return d
		}
		if !reachable {
			return bytecodeErr("unreachable code at %d", pc)
		}
		opcode := code[pc]
		info := &opTable[opcode]
		if info.name == "" {
			return bytecodeErr("invalid opcode 0x%02x at %d", opcode, pc)
		}
		if pc+1+info.operandBytes > len(code) {
			return bytecodeErr("truncated instruction at %d", pc)
		}
		operands := code[pc+1:]
		op := Op(opcode)
		nextPc := pc + 1 + info.operandBytes
		switch op {
		case OpConst:
			k := readU16(operands)
			if k >= len(p.constants) || !constOrder.use(k) {
				return bytecodeErr("bad constant operand at %d", pc)
			}
		case OpAttr, OpField, OpTag:
			param := int(operands[0])
			sym := readU16(operands[1:])
			table, order := p.attrs, &attrOrder
			if op == OpField {
				table, order = p.fields, &fieldOrder
			} else if op == OpTag {
				table, order = p.tags, &tagOrder
			}
			if param >= len(p.params) || sym >= len(table) || !order.use(sym) {
				return bytecodeErr("bad operand at %d", pc)
			}
		case OpCurve:
			k := readU16(operands)
			if k >= len(p.curves) || !curveOrder.use(k) {
				return bytecodeErr("bad curve operand at %d", pc)
			}
		case OpStacks:
			usesStacks = true
		case OpLevel:
			usesLevel = true
		}
		if len(stack) < info.pops {
			return bytecodeErr("stack underflow at %d", pc)
		}
		for k := 0; k < info.pops; k++ {
			if stack[len(stack)-1-k] != info.in {
				return bytecodeErr("operand type mismatch at %d", pc)
			}
		}
		stack = stack[:len(stack)-info.pops]
		if info.pushes {
			stack = append(stack, info.out)
			if len(stack) > maxStack {
				maxStack = len(stack)
			}
			if maxStack > MaxStack {
				return limitErr("stack deeper than %d", MaxStack)
			}
		}
		cost += info.cost
		ops++
		if op == OpJumpIfFalse || op == OpJump {
			target := nextPc + readU16(operands)
			if target > len(code) {
				return bytecodeErr("jump out of range at %d", pc)
			}
			if target == nextPc {
				return bytecodeErr("empty jump at %d", pc)
			}
			if d := addPending(target); d != nil {
				return d
			}
			if op == OpJump {
				reachable = false
			}
		}
		pc = nextPc
	}
	if d := join(len(code)); d != nil {
		return d
	}
	if len(pending) != 0 {
		return bytecodeErr("jump into the middle of an instruction")
	}
	if !reachable || len(stack) != 1 {
		return bytecodeErr("program must leave exactly one value")
	}
	if ops > MaxOps {
		return limitErr("more than %d ops", MaxOps)
	}
	if constOrder.next != len(p.constants) || attrOrder.next != len(p.attrs) || tagOrder.next != len(p.tags) ||
		fieldOrder.next != len(p.fields) || curveOrder.next != len(p.curves) {
		return bytecodeErr("unused table entries")
	}
	if cost > maxCost {
		return limitErr("evaluation cost %d exceeds the budget %d", cost, maxCost)
	}
	p.resultType = stack[0]
	p.maxStack = maxStack
	p.cost = cost
	p.usesStacks = usesStacks
	p.usesLevel = usesLevel
	return nil
}

// ---- encoding --------------------------------------------------------------------------------------

const formatVersion = 1

var magic = [4]byte{'H', 'X', 'L', '1'}

type writer struct{ b []byte }

func (w *writer) u8(v uint8)   { w.b = append(w.b, v) }
func (w *writer) u16(v uint16) { w.b = append(w.b, byte(v), byte(v>>8)) }
func (w *writer) u32(v uint32) {
	w.b = append(w.b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24))
}
func (w *writer) u64(v uint64) {
	for i := 0; i < 8; i++ {
		w.b = append(w.b, byte(v>>(8*i)))
	}
}
func (w *writer) str(s string) {
	w.u16(uint16(len(s)))
	w.b = append(w.b, s...)
}
func (w *writer) strs(v []string) {
	w.u16(uint16(len(v)))
	for _, s := range v {
		w.str(s)
	}
}

// Encode returns the canonical bytecode ("HXL1" container); byte-identical with C++.
func (p *Program) Encode() []byte {
	w := writer{b: make([]byte, 0, 64+len(p.code)+8*len(p.constants))}
	w.b = append(w.b, magic[:]...)
	w.u8(formatVersion)
	w.u8(uint8(p.resultType))
	w.u16(uint16(p.maxStack))
	w.u32(uint32(p.cost))
	w.str(p.name)
	w.u8(uint8(len(p.params)))
	for _, s := range p.params {
		w.str(s)
	}
	w.u16(uint16(len(p.constants)))
	for _, c := range p.constants {
		w.u64(math.Float64bits(c))
	}
	w.strs(p.attrs)
	w.strs(p.tags)
	w.strs(p.fields)
	w.strs(p.curves)
	w.u32(uint32(len(p.code)))
	w.b = append(w.b, p.code...)
	return w.b
}

type reader struct {
	b   []byte
	pos int
	ok  bool
}

func (r *reader) need(n int) bool {
	if !r.ok || len(r.b)-r.pos < n {
		r.ok = false
		return false
	}
	return true
}
func (r *reader) u8() uint8 {
	if !r.need(1) {
		return 0
	}
	v := r.b[r.pos]
	r.pos++
	return v
}
func (r *reader) u16() uint16 {
	if !r.need(2) {
		return 0
	}
	v := uint16(r.b[r.pos]) | uint16(r.b[r.pos+1])<<8
	r.pos += 2
	return v
}
func (r *reader) u32() uint32 {
	if !r.need(4) {
		return 0
	}
	var v uint32
	for i := 0; i < 4; i++ {
		v |= uint32(r.b[r.pos+i]) << (8 * i)
	}
	r.pos += 4
	return v
}
func (r *reader) u64() uint64 {
	if !r.need(8) {
		return 0
	}
	var v uint64
	for i := 0; i < 8; i++ {
		v |= uint64(r.b[r.pos+i]) << (8 * i)
	}
	r.pos += 8
	return v
}
func (r *reader) str() string {
	n := int(r.u16())
	if n > MaxNameBytes || !r.need(n) {
		r.ok = false
		return ""
	}
	s := string(r.b[r.pos : r.pos+n])
	r.pos += n
	return s
}
func (r *reader) strs(max int) []string {
	n := int(r.u16())
	if n > max {
		r.ok = false
	}
	if !r.ok {
		return nil
	}
	out := make([]string, 0, n)
	for i := 0; i < n && r.ok; i++ {
		out = append(out, r.str())
	}
	return out
}

// Decode parses and fully verifies untrusted bytecode (E_BYTECODE on any defect).
func Decode(b []byte) (*Program, error) {
	r := reader{b: b, ok: true}
	for _, c := range magic {
		if r.u8() != c {
			return nil, bytecodeErr("not HXL bytecode (bad magic)")
		}
	}
	version := r.u8()
	if r.ok && version != formatVersion {
		return nil, bytecodeErr("unsupported bytecode version %d", version)
	}
	p := &Program{}
	resultType := r.u8()
	maxStack := int(r.u16())
	cost := int(r.u32())
	p.name = r.str()
	paramCount := int(r.u8())
	if paramCount > MaxParams {
		return nil, bytecodeErr("too many parameters")
	}
	p.params = []string{}
	for i := 0; i < paramCount && r.ok; i++ {
		p.params = append(p.params, r.str())
	}
	constCount := int(r.u16())
	if constCount > MaxConstants {
		return nil, bytecodeErr("too many constants")
	}
	for i := 0; i < constCount && r.ok; i++ {
		p.constants = append(p.constants, math.Float64frombits(r.u64()))
	}
	p.attrs = r.strs(MaxSymbols)
	p.tags = r.strs(MaxSymbols)
	p.fields = r.strs(MaxSymbols)
	p.curves = r.strs(MaxSymbols)
	codeSize := int(r.u32())
	if r.ok && codeSize > MaxCodeBytes {
		return nil, bytecodeErr("code too large")
	}
	if r.ok && r.need(codeSize) {
		p.code = append([]byte(nil), r.b[r.pos:r.pos+codeSize]...)
		r.pos += codeSize
	}
	if !r.ok {
		return nil, bytecodeErr("truncated bytecode")
	}
	if r.pos != len(r.b) {
		return nil, bytecodeErr("trailing bytes after the bytecode")
	}
	if resultType > 1 {
		return nil, bytecodeErr("invalid result type")
	}
	if d := p.finish(MaxCost); d != nil {
		if d.Status == ELimit {
			d.Status, d.Line, d.Column = EBytecode, 0, 0
		}
		return nil, d
	}
	if uint8(p.resultType) != resultType || p.maxStack != maxStack || p.cost != cost {
		return nil, bytecodeErr("header does not match the code (result type, stack or cost)")
	}
	return p, nil
}

// Hash is FNV-1a 64 of Encode(): the corpus' "bytecode" value.
func (p *Program) Hash() uint64 {
	h := uint64(0xcbf29ce484222325)
	for _, b := range p.Encode() {
		h ^= uint64(b)
		h *= 0x100000001b3
	}
	return h
}

// Ref is an Attr, Field or Tag reference of the code.
type Ref struct {
	Op     Op
	Param  int
	Symbol int
}

// References lists every Attr/Field/Tag reference in code order.
func (p *Program) References() []Ref {
	var refs []Ref
	for pc := 0; pc < len(p.code); {
		op := Op(p.code[pc])
		if op == OpAttr || op == OpField || op == OpTag {
			refs = append(refs, Ref{op, int(p.code[pc+1]), readU16(p.code[pc+2:])})
		}
		pc += 1 + opTable[op].operandBytes
	}
	return refs
}

// Disassemble returns a human-readable listing (not a stable format).
func (p *Program) Disassemble() string {
	var sb strings.Builder
	name := p.name
	if name == "" {
		name = "<expr>"
	}
	fmt.Fprintf(&sb, "; %s(%s) -> %s  stack=%d cost=%d\n", name, strings.Join(p.params, ", "), p.resultType,
		p.maxStack, p.cost)
	for pc := 0; pc < len(p.code); {
		op := Op(p.code[pc])
		info := &opTable[op]
		fmt.Fprintf(&sb, "%04d  %s", pc, info.name)
		o := p.code[pc+1:]
		switch op {
		case OpConst:
			fmt.Fprintf(&sb, " %d  ; %v", readU16(o), p.constants[readU16(o)])
		case OpAttr:
			fmt.Fprintf(&sb, " %s.%s", p.params[o[0]], p.attrs[readU16(o[1:])])
		case OpField:
			fmt.Fprintf(&sb, " %s.%s", p.params[o[0]], p.fields[readU16(o[1:])])
		case OpTag:
			fmt.Fprintf(&sb, " %s.%s", p.params[o[0]], p.tags[readU16(o[1:])])
		case OpCurve:
			fmt.Fprintf(&sb, " %s", p.curves[readU16(o)])
		case OpJumpIfFalse, OpJump:
			fmt.Fprintf(&sb, " -> %04d", pc+3+readU16(o))
		}
		sb.WriteByte('\n')
		pc += 1 + info.operandBytes
	}
	return sb.String()
}
