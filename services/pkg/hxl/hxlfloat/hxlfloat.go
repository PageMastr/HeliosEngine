// Package hxlfloat is the `hxlfloat` analyzer of 06 §1.2 rule 5: it enforces the cross-language
// float rules on services/pkg/hxl/... so the Go HXL interpreter stays bit-identical with C++.
//
// Rules (each finding names one):
//   - "unconverted-mul": a float multiplication that is not the direct operand of an explicit
//     float64()/float32() conversion (the Go spec lets the compiler fuse x*y + z into an FMA,
//     "possibly across statements"; an explicit conversion is a rounding point), and every `*=` on
//     floats;
//   - "const-arith": a float constant expression with more than one operand (Go folds untyped
//     constant expressions exactly, where C++ rounds every step): constants must be single literals;
//   - "banned-math": any use of a math function outside the exact or correctly rounded allowlist
//     (Sqrt, Abs, Floor, Ceil, Trunc, Copysign, the bit conversions, IsNaN, IsInf, Inf, NaN, Signbit).
//     math.FMA, Exp, Log, Pow, Asinh, Sin, Mod, Ldexp, Max, ... are banned, however they are named
//     (math.Exp, an aliased import or a dot import).
//
// It uses only the standard library (go/parser, go/types), so it runs as a unit test (see
// hxlfloat_test.go, which also checks planted violations) and from `go run ./pkg/hxl/hxlfloat/cmd`.
// Test files (_test.go) and generated files (a "Code generated ... DO NOT EDIT." line before the
// package clause, ast.IsGenerated) are not checked. In pkg/hxl that exempts only gamedef, which
// holds schemac's records and codecs; its one float product (a JSON duration in seconds, in
// helios_runtime.go) is not HXL arithmetic.
package hxlfloat

import (
	"fmt"
	"go/ast"
	"go/importer"
	"go/parser"
	"go/printer"
	"go/token"
	"go/types"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Finding is one rule violation.
type Finding struct {
	Pos     token.Position
	Rule    string
	Message string
}

func (f Finding) String() string { return fmt.Sprintf("%s: %s: %s", f.Pos, f.Rule, f.Message) }

// allowedMath are the math functions that are exact or correctly rounded on every GOARCH.
var allowedMath = map[string]bool{
	"Sqrt": true, "Abs": true, "Floor": true, "Ceil": true, "Trunc": true, "Copysign": true,
	"Float64bits": true, "Float64frombits": true, "Float32bits": true, "Float32frombits": true,
	"IsNaN": true, "IsInf": true, "Inf": true, "NaN": true, "Signbit": true,
}

// isGenerated follows the Go convention (ast.IsGenerated): the "Code generated ... DO NOT EDIT."
// marker must come before the package clause. A marker anywhere else does not exempt a file.
func isGenerated(f *ast.File) bool { return ast.IsGenerated(f) }

// CheckDir type-checks the package in dir (non-test, non-generated files) and returns its findings,
// sorted by position.
func CheckDir(dir string) ([]Finding, error) {
	fset := token.NewFileSet()
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var files []*ast.File
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".go") || strings.HasSuffix(name, "_test.go") {
			continue
		}
		f, err := parser.ParseFile(fset, filepath.Join(dir, name), nil, parser.ParseComments)
		if err != nil {
			return nil, err
		}
		if !isGenerated(f) {
			files = append(files, f)
		}
	}
	if len(files) == 0 {
		return nil, nil
	}
	info := &types.Info{
		Types: map[ast.Expr]types.TypeAndValue{},
		Uses:  map[*ast.Ident]types.Object{},
	}
	conf := types.Config{Importer: importer.ForCompiler(fset, "source", nil)}
	if _, err := conf.Check(files[0].Name.Name, fset, files, info); err != nil {
		return nil, fmt.Errorf("hxlfloat: type-checking %s: %w", dir, err)
	}
	return CheckFiles(fset, files, info), nil
}

// isFloat reports a floating-point type, including a type parameter whose type set contains one
// (T ~float64: gc instantiates such code with float arithmetic and fuses it like any other).
func isFloat(t types.Type) bool {
	if t == nil {
		return false
	}
	if tp, ok := t.(*types.TypeParam); ok {
		iface, ok := tp.Constraint().Underlying().(*types.Interface)
		if !ok {
			return false
		}
		for i := 0; i < iface.NumEmbeddeds(); i++ {
			switch e := iface.EmbeddedType(i).(type) {
			case *types.Union:
				for j := 0; j < e.Len(); j++ {
					if isFloat(e.Term(j).Type()) {
						return true
					}
				}
			default:
				if isFloat(e) {
					return true
				}
			}
		}
		return false
	}
	b, ok := t.Underlying().(*types.Basic)
	return ok && b.Info()&types.IsFloat != 0
}

func unparen(e ast.Expr) ast.Expr {
	for {
		p, ok := e.(*ast.ParenExpr)
		if !ok {
			return e
		}
		e = p.X
	}
}

// CheckFiles applies the rules to type-checked files (info needs Types and Uses).
func CheckFiles(fset *token.FileSet, files []*ast.File, info *types.Info) []Finding {
	var out []Finding
	report := func(n ast.Node, rule, format string, args ...any) {
		out = append(out, Finding{fset.Position(n.Pos()), rule, fmt.Sprintf(format, args...)})
	}
	isConst := func(e ast.Expr) bool {
		tv, ok := info.Types[e]
		return ok && tv.Value != nil
	}
	floatType := func(e ast.Expr) bool {
		tv, ok := info.Types[e]
		return ok && isFloat(tv.Type)
	}
	// Direct operands of float conversions: float64(a*b) (parentheses allowed).
	converted := map[ast.Expr]bool{}
	for _, f := range files {
		ast.Inspect(f, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok || len(call.Args) != 1 {
				return true
			}
			// A conversion to a type parameter is not accepted as a rounding point: whether gc
			// rounds there depends on how it instantiates the code, so it is flagged like a bare *.
			if tv, ok := info.Types[call.Fun]; ok && tv.IsType() && isFloat(tv.Type) {
				if _, generic := tv.Type.(*types.TypeParam); !generic {
					converted[unparen(call.Args[0])] = true
				}
			}
			return true
		})
	}
	for _, f := range files {
		var stack []ast.Node
		ast.Inspect(f, func(n ast.Node) bool {
			if n == nil {
				stack = stack[:len(stack)-1]
				return true
			}
			stack = append(stack, n)
			switch x := n.(type) {
			case *ast.BinaryExpr:
				if !floatType(x) {
					return true
				}
				if isConst(x) {
					// Report only the outermost constant expression.
					if p, ok := unparenParent(stack).(*ast.BinaryExpr); ok && isConst(p) {
						return true
					}
					report(x, "const-arith", "float constant expression %s: use a single typed hex-float literal (06 §1.2 rule 2)",
						exprString(fset, x))
					return true
				}
				if x.Op == token.MUL && !converted[x] {
					report(x, "unconverted-mul", "float multiplication %s must be the direct operand of float64() (06 §1.2 rule 1)",
						exprString(fset, x))
				}
			case *ast.AssignStmt:
				if x.Tok == token.MUL_ASSIGN && len(x.Lhs) == 1 && floatType(x.Lhs[0]) {
					report(x, "unconverted-mul", "float *= cannot be converted; write x = float64(x * y) (06 §1.2 rule 1)")
				}
			case *ast.Ident:
				// Every use of a math function: math.Exp, an aliased import (m.Exp) and a dot
				// import (Exp) all resolve to the same *types.Func.
				fn, ok := info.Uses[x].(*types.Func)
				if !ok || fn.Pkg() == nil || fn.Pkg().Path() != "math" {
					return true
				}
				if !allowedMath[fn.Name()] {
					report(x, "banned-math", "math.%s is not exact or correctly rounded on every target; use package det (06 §1.2 rule 3)",
						fn.Name())
				}
			}
			return true
		})
	}
	sort.Slice(out, func(i, j int) bool {
		a, b := out[i].Pos, out[j].Pos
		if a.Filename != b.Filename {
			return a.Filename < b.Filename
		}
		if a.Line != b.Line {
			return a.Line < b.Line
		}
		return a.Column < b.Column
	})
	return out
}

// unparenParent returns the nearest ancestor of the top of stack that is not a ParenExpr.
func unparenParent(stack []ast.Node) ast.Node {
	for i := len(stack) - 2; i >= 0; i-- {
		if _, ok := stack[i].(*ast.ParenExpr); !ok {
			return stack[i]
		}
	}
	return nil
}

func exprString(fset *token.FileSet, e ast.Expr) string {
	var sb strings.Builder
	if err := printer.Fprint(&sb, fset, e); err != nil {
		return "<expr>"
	}
	return sb.String()
}
