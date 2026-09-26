package hxl

import (
	"fmt"
	"math"
	"strconv"
)

// Lexer and parser. Mirrors engine/hxl/src/syntax.cpp function by function: the corpus checks
// status codes and positions, so both must perform the same checks in the same order.

type tok uint8

const (
	tEnd tok = iota
	tIdent
	tNumber
	tTrue
	tFalse
	tFormula
	tLParen
	tRParen
	tComma
	tDot
	tPlus
	tMinus
	tStar
	tSlash
	tCaret
	tLt
	tLe
	tGt
	tGe
	tEqEq
	tNotEq
	tAndAnd
	tOrOr
	tBang
	tAssign
	tSemicolon
)

var tokNames = [...]string{
	"end of input", "a name", "a number", "'true'", "'false'", "'formula'", "'('", "')'", "','", "'.'",
	"'+'", "'-'", "'*'", "'/'", "'^'", "'<'", "'<='", "'>'", "'>='", "'=='", "'!='", "'&&'", "'||'", "'!'",
	"'='", "';'",
}

func (t tok) String() string { return tokNames[t] }

type token struct {
	kind   tok
	text   string
	line   int
	column int
	number float64
}

func isIdentStart(c byte) bool { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' }
func isDigit(c byte) bool      { return c >= '0' && c <= '9' }
func isIdentChar(c byte) bool  { return isIdentStart(c) || isDigit(c) }

func isIdentifier(s string) bool {
	if len(s) == 0 || len(s) > MaxNameBytes || !isIdentStart(s[0]) {
		return false
	}
	for i := 0; i < len(s); i++ {
		if !isIdentChar(s[i]) {
			return false
		}
	}
	return true
}

func isDottedName(s string) bool {
	if len(s) == 0 || len(s) > MaxNameBytes {
		return false
	}
	start := 0
	for i := 0; i <= len(s); i++ {
		if i == len(s) || s[i] == '.' {
			if !isIdentifier(s[start:i]) {
				return false
			}
			start = i + 1
		}
	}
	return true
}

func diag(status Status, line, col int, format string, args ...any) *Diagnostic {
	return &Diagnostic{Status: status, Line: line, Column: col, Message: fmt.Sprintf(format, args...)}
}

// parseNumber mirrors the C++ literal rule: zero or a normal finite double, correctly rounded. The
// lexer first bounds the literal to MaxNumberBytes: strconv.ParseFloat is not correctly rounded
// beyond 800 significant digits.
func parseNumber(text string) (float64, bool) {
	v, err := strconv.ParseFloat(text, 64)
	if err != nil || math.IsInf(v, 0) || math.IsNaN(v) {
		return 0, false
	}
	if v == 0 {
		for i := 0; i < len(text); i++ {
			c := text[i]
			if c == 'e' || c == 'E' {
				break
			}
			if c >= '1' && c <= '9' {
				return 0, false // underflow
			}
		}
	} else if v < 0x1p-1022 {
		return 0, false // subnormal
	}
	return v, true
}

func lex(src string) ([]token, *Diagnostic) {
	var out []token
	i, line, col := 0, 1, 1
	n := len(src)
	peek := func(k int) byte {
		if i+k < n {
			return src[i+k]
		}
		return 0
	}
	for {
		for i < n {
			c := src[i]
			if c == ' ' || c == '\t' || c == '\r' {
				i++
				col++
			} else if c == '\n' {
				i++
				line++
				col = 1
			} else if c == '/' && peek(1) == '/' {
				for i < n && src[i] != '\n' {
					i++
					col++
				}
			} else {
				break
			}
		}
		t := token{line: line, column: col}
		if i >= n {
			t.kind = tEnd
			out = append(out, t)
			return out, nil
		}
		start := i
		c := src[i]
		switch {
		case isIdentStart(c):
			for i < n && isIdentChar(src[i]) {
				i++
			}
			t.text = src[start:i]
			switch t.text {
			case "true":
				t.kind = tTrue
			case "false":
				t.kind = tFalse
			case "formula":
				t.kind = tFormula
			default:
				t.kind = tIdent
				if len(t.text) > MaxNameBytes {
					return nil, diag(ELimit, line, col, "name longer than %d bytes", MaxNameBytes)
				}
			}
		case isDigit(c) || (c == '.' && isDigit(peek(1))):
			ok := true
			for i < n && isDigit(src[i]) {
				i++
			}
			if i < n && src[i] == '.' {
				i++
				for i < n && isDigit(src[i]) {
					i++
				}
			}
			if i < n && (src[i] == 'e' || src[i] == 'E') {
				i++
				if i < n && (src[i] == '+' || src[i] == '-') {
					i++
				}
				if i >= n || !isDigit(src[i]) {
					ok = false
				}
				for i < n && isDigit(src[i]) {
					i++
				}
			}
			if i < n && (isIdentChar(src[i]) || src[i] == '.') {
				ok = false
			}
			t.text = src[start:i]
			t.kind = tNumber
			if !ok {
				return nil, diag(ENumber, line, col, "malformed number literal")
			}
			if len(t.text) > MaxNumberBytes {
				return nil, diag(ENumber, line, col, "number literal longer than %d bytes", MaxNumberBytes)
			}
			v, valid := parseNumber(t.text)
			if !valid {
				return nil, diag(ENumber, line, col, "number literal '%s' is out of range (zero or a normal double)", t.text)
			}
			t.number = v
		default:
			d := peek(1)
			length := 1
			switch c {
			case '(':
				t.kind = tLParen
			case ')':
				t.kind = tRParen
			case ',':
				t.kind = tComma
			case '.':
				t.kind = tDot
			case '+':
				t.kind = tPlus
			case '-':
				t.kind = tMinus
			case '*':
				t.kind = tStar
			case '/':
				t.kind = tSlash
			case '^':
				t.kind = tCaret
			case ';':
				t.kind = tSemicolon
			case '<':
				t.kind, length = tLt, 1
				if d == '=' {
					t.kind, length = tLe, 2
				}
			case '>':
				t.kind, length = tGt, 1
				if d == '=' {
					t.kind, length = tGe, 2
				}
			case '=':
				t.kind, length = tAssign, 1
				if d == '=' {
					t.kind, length = tEqEq, 2
				}
			case '!':
				t.kind, length = tBang, 1
				if d == '=' {
					t.kind, length = tNotEq, 2
				}
			case '&':
				if d != '&' {
					return nil, diag(ELex, line, col, "unexpected character '&' (did you mean '&&'?)")
				}
				t.kind, length = tAndAnd, 2
			case '|':
				if d != '|' {
					return nil, diag(ELex, line, col, "unexpected character '|' (did you mean '||'?)")
				}
				t.kind, length = tOrOr, 2
			default:
				if c >= 0x21 && c < 0x7f {
					return nil, diag(ELex, line, col, "unexpected character '%c'", c)
				}
				return nil, diag(ELex, line, col, "unexpected byte 0x%02x", c)
			}
			i += length
			t.text = src[start:i]
		}
		col += i - start
		out = append(out, t)
	}
}

type nodeKind uint8

const (
	nNumber nodeKind = iota
	nBool
	nPath
	nUnary
	nBinary
	nCall
)

type node struct {
	kind        nodeKind
	line        int // main token
	column      int
	startLine   int // first token (an enclosing '(' if parenthesized)
	startColumn int
	depth       int
	number      float64
	boolean     bool
	op          tok
	name        string
	segments    []string
	children    []int
}

type ast struct {
	nodes       []node
	root        int
	isFormula   bool
	formulaName string
	params      []string
}

type parser struct {
	toks  []token
	ast   *ast
	pos   int
	depth int
}

func (p *parser) cur() *token { return &p.toks[p.pos] }
func (p *parser) next() *token {
	if p.pos+1 < len(p.toks) {
		return &p.toks[p.pos+1]
	}
	return &p.toks[p.pos]
}
func (p *parser) advance() {
	if p.pos+1 < len(p.toks) {
		p.pos++
	}
}

func (p *parser) syntaxError(what string) *Diagnostic {
	c := p.cur()
	return diag(ESyntax, c.line, c.column, "%s, found %s", what, c.kind)
}

func depthError(at token) *Diagnostic {
	return diag(ELimit, at.line, at.column, "expression nested deeper than %d levels", MaxParseDepth)
}

func (p *parser) addNode(n node) int {
	p.ast.nodes = append(p.ast.nodes, n)
	return len(p.ast.nodes) - 1
}

func (p *parser) finishComposite(n *node) *Diagnostic {
	depth := 0
	for _, c := range n.children {
		if d := p.ast.nodes[c].depth; d > depth {
			depth = d
		}
	}
	n.depth = depth + 1
	if n.depth > MaxAstDepth {
		return diag(ELimit, n.line, n.column, "expression tree deeper than %d levels", MaxAstDepth)
	}
	return nil
}

func (p *parser) makeBinary(opTok token, lhs, rhs int) (int, *Diagnostic) {
	l := &p.ast.nodes[lhs]
	n := node{kind: nBinary, op: opTok.kind, line: opTok.line, column: opTok.column,
		startLine: l.startLine, startColumn: l.startColumn, children: []int{lhs, rhs}}
	if d := p.finishComposite(&n); d != nil {
		return 0, d
	}
	return p.addNode(n), nil
}

func (p *parser) parseSource() *Diagnostic {
	if p.cur().kind == tFormula {
		p.advance()
		if p.cur().kind != tIdent {
			return p.syntaxError("expected the formula name")
		}
		p.ast.isFormula = true
		p.ast.formulaName = p.cur().text
		p.advance()
		if p.cur().kind != tLParen {
			return p.syntaxError("expected '(' after the formula name")
		}
		p.advance()
		if p.cur().kind != tRParen {
			for {
				if p.cur().kind != tIdent {
					return p.syntaxError("expected a parameter name")
				}
				param := p.cur().text
				for _, q := range p.ast.params {
					if q == param {
						return diag(EDuplicateParam, p.cur().line, p.cur().column, "duplicate parameter '%s'", param)
					}
				}
				if len(p.ast.params) == MaxParams {
					return diag(ELimit, p.cur().line, p.cur().column, "more than %d parameters", MaxParams)
				}
				p.ast.params = append(p.ast.params, param)
				p.advance()
				if p.cur().kind == tComma {
					p.advance()
					continue
				}
				break
			}
		}
		if p.cur().kind != tRParen {
			return p.syntaxError("expected ',' or ')' in the parameter list")
		}
		p.advance()
		if p.cur().kind != tAssign {
			return p.syntaxError("expected '=' after the parameter list")
		}
		p.advance()
	}
	root, d := p.parseExpr()
	if d != nil {
		return d
	}
	p.ast.root = root
	if p.cur().kind == tSemicolon {
		p.advance()
	}
	if p.cur().kind != tEnd {
		return p.syntaxError("expected an operator or the end of the expression")
	}
	return nil
}

// binaryPrecedence ranks the binary operators, loosest first; 0 = not a binary operator. Every
// level is left-associative (or := and {'||' and}, ..., product := unary {('*'|'/') unary}).
func binaryPrecedence(k tok) int {
	switch k {
	case tOrOr:
		return 1
	case tAndAnd:
		return 2
	case tEqEq, tNotEq:
		return 3
	case tLt, tLe, tGt, tGe:
		return 4
	case tPlus, tMinus:
		return 5
	case tStar, tSlash:
		return 6
	}
	return 0
}

func (p *parser) parseExpr() (int, *Diagnostic) {
	p.depth++
	if p.depth > MaxParseDepth {
		return 0, depthError(*p.cur())
	}
	n, d := p.parseBinary(1)
	p.depth--
	return n, d
}

// parseBinary parses a chain of binary operators of precedence >= minPrec (precedence climbing,
// as in C++, where it keeps hostile nesting cheap on the stack). It builds exactly the trees, in
// exactly the node order, of one recursive-descent function per level.
func (p *parser) parseBinary(minPrec int) (int, *Diagnostic) {
	lhs, d := p.parseUnary()
	if d != nil {
		return 0, d
	}
	for prec := binaryPrecedence(p.cur().kind); prec >= minPrec; prec = binaryPrecedence(p.cur().kind) {
		opTok := *p.cur()
		p.advance()
		rhs, d := p.parseBinary(prec + 1)
		if d != nil {
			return 0, d
		}
		if lhs, d = p.makeBinary(opTok, lhs, rhs); d != nil {
			return 0, d
		}
	}
	return lhs, nil
}

func (p *parser) parseUnary() (int, *Diagnostic) {
	if k := p.cur().kind; k == tMinus || k == tBang {
		opTok := *p.cur()
		p.advance()
		p.depth++
		if p.depth > MaxParseDepth {
			return 0, depthError(opTok)
		}
		child, d := p.parseUnary()
		p.depth--
		if d != nil {
			return 0, d
		}
		n := node{kind: nUnary, op: opTok.kind, line: opTok.line, column: opTok.column,
			startLine: opTok.line, startColumn: opTok.column, children: []int{child}}
		if d := p.finishComposite(&n); d != nil {
			return 0, d
		}
		return p.addNode(n), nil
	}
	return p.parsePower()
}

func (p *parser) parsePower() (int, *Diagnostic) {
	base, d := p.parsePrimary()
	if d != nil {
		return 0, d
	}
	if p.cur().kind != tCaret {
		return base, nil
	}
	opTok := *p.cur()
	p.advance()
	p.depth++
	if p.depth > MaxParseDepth {
		return 0, depthError(opTok)
	}
	exponent, d := p.parseUnary()
	p.depth--
	if d != nil {
		return 0, d
	}
	return p.makeBinary(opTok, base, exponent)
}

func (p *parser) parsePrimary() (int, *Diagnostic) {
	t := *p.cur()
	n := node{line: t.line, column: t.column, startLine: t.line, startColumn: t.column, depth: 1}
	switch t.kind {
	case tNumber:
		n.kind = nNumber
		n.number = t.number
		p.advance()
		return p.addNode(n), nil
	case tTrue, tFalse:
		n.kind = nBool
		n.boolean = t.kind == tTrue
		p.advance()
		return p.addNode(n), nil
	case tLParen:
		p.advance()
		inner, d := p.parseExpr()
		if d != nil {
			return 0, d
		}
		if p.cur().kind != tRParen {
			return 0, p.syntaxError("expected ')'")
		}
		p.advance()
		p.ast.nodes[inner].startLine = t.line
		p.ast.nodes[inner].startColumn = t.column
		return inner, nil
	case tIdent:
		if p.next().kind == tLParen {
			n.kind = nCall
			n.name = t.text
			p.advance()
			p.advance()
			if p.cur().kind != tRParen {
				for {
					arg, d := p.parseExpr()
					if d != nil {
						return 0, d
					}
					n.children = append(n.children, arg)
					if p.cur().kind == tComma {
						p.advance()
						continue
					}
					break
				}
			}
			if p.cur().kind != tRParen {
				return 0, p.syntaxError("expected ',' or ')' in the argument list")
			}
			p.advance()
			if d := p.finishComposite(&n); d != nil {
				return 0, d
			}
			return p.addNode(n), nil
		}
		n.kind = nPath
		n.segments = []string{t.text}
		p.advance()
		for p.cur().kind == tDot {
			p.advance()
			if p.cur().kind != tIdent {
				return 0, p.syntaxError("expected a name after '.'")
			}
			n.segments = append(n.segments, p.cur().text)
			p.advance()
		}
		return p.addNode(n), nil
	default:
		return 0, p.syntaxError("expected an expression")
	}
}

func parse(toks []token) (*ast, *Diagnostic) {
	a := &ast{}
	p := parser{toks: toks, ast: a}
	if d := p.parseSource(); d != nil {
		return nil, d
	}
	return a, nil
}
