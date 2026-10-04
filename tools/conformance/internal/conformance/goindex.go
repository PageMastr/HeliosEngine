package conformance

import (
	"go/ast"
	"go/parser"
	"go/token"
	"path"
	"strconv"
	"strings"
)

// goFile is one parsed Go source file. The Go rules read syntax only, so working-tree code that does
// not type-check yet is still checked (D7); a file that does not parse is reported by the rule.
type goFile struct {
	Path string
	File *ast.File
	Err  error
	pkg  string            // directory, the package key of goIndex.consts
	imps map[string]string // local import name -> import path
}

// goIndex parses Go files on demand and resolves package-level constants, across packages of the
// same module (by the module paths of the tree's go.mod files), the way the rules need them: the
// bucket name in `KeyValue(ctx, orchestrator.BucketDirectory)` or the shift in `x << prefixShift`.
type goIndex struct {
	tree    *Tree
	fset    *token.FileSet
	files   map[string]*goFile
	consts  map[string]map[string]ast.Expr // package dir -> name -> value expression
	loaded  map[string]bool
	modules map[string]string // module path -> directory
}

func (t *Tree) goIndex() *goIndex {
	if t.gidx == nil {
		t.gidx = &goIndex{tree: t, fset: token.NewFileSet(), files: map[string]*goFile{},
			consts: map[string]map[string]ast.Expr{}, loaded: map[string]bool{}, modules: map[string]string{}}
		for _, f := range t.Files {
			if path.Base(f) != "go.mod" {
				continue
			}
			for _, l := range t.Lines(f) {
				if fs := strings.Fields(l); len(fs) == 2 && fs[0] == "module" {
					t.gidx.modules[strings.Trim(fs[1], `"`)] = path.Dir(f)
				}
			}
		}
	}
	return t.gidx
}

// file parses f (cached).
func (g *goIndex) file(f string) *goFile {
	if gf, ok := g.files[f]; ok {
		return gf
	}
	gf := &goFile{Path: f, pkg: path.Dir(f), imps: map[string]string{}}
	gf.File, gf.Err = parser.ParseFile(g.fset, f, g.tree.Text(f), parser.ParseComments|parser.SkipObjectResolution)
	if gf.File != nil {
		for _, im := range gf.File.Imports {
			p, _ := strconv.Unquote(im.Path.Value)
			name := path.Base(p)
			if im.Name != nil {
				name = im.Name.Name
			} else if v := path.Base(p); strings.HasPrefix(v, "v") && len(v) > 1 && v[1] >= '0' && v[1] <= '9' {
				name = path.Base(path.Dir(p)) // github.com/x/y/v2 is package y
			}
			gf.imps[name] = p
		}
	}
	g.files[f] = gf
	return gf
}

func (g *goIndex) line(pos token.Pos) int { return g.fset.Position(pos).Line }

// pkgConsts indexes the package-level constants of the package in dir (non-test files).
func (g *goIndex) pkgConsts(dir string) map[string]ast.Expr {
	if g.loaded[dir] {
		return g.consts[dir]
	}
	g.loaded[dir] = true
	m := map[string]ast.Expr{}
	for _, f := range g.tree.Files {
		if path.Dir(f) != dir || !strings.HasSuffix(f, ".go") || strings.HasSuffix(f, "_test.go") {
			continue
		}
		for k, v := range fileConsts(g.file(f).File) {
			m[k] = v
		}
	}
	g.consts[dir] = m
	return m
}

// fileConsts returns a file's package-level constants.
func fileConsts(file *ast.File) map[string]ast.Expr {
	m := map[string]ast.Expr{}
	if file == nil {
		return m
	}
	for _, d := range file.Decls {
		gd, ok := d.(*ast.GenDecl)
		if !ok || gd.Tok != token.CONST {
			continue
		}
		for _, s := range gd.Specs {
			vs := s.(*ast.ValueSpec)
			for i, n := range vs.Names {
				if i < len(vs.Values) {
					m[n.Name] = vs.Values[i]
				}
			}
		}
	}
	return m
}

// lookup resolves an identifier or a pkg.Name selector to its constant expression and the package
// directory it lives in.
func (g *goIndex) lookup(gf *goFile, dir string, e ast.Expr) (ast.Expr, string) {
	switch x := e.(type) {
	case *ast.Ident:
		if gf != nil && dir == gf.pkg { // the file's own constants first: a _test.go file is not indexed
			if v, ok := fileConsts(gf.File)[x.Name]; ok {
				return v, dir
			}
		}
		if v, ok := g.pkgConsts(dir)[x.Name]; ok {
			return v, dir
		}
	case *ast.SelectorExpr:
		id, ok := x.X.(*ast.Ident)
		if !ok || gf == nil || dir != gf.pkg {
			return nil, ""
		}
		imp, ok := gf.imps[id.Name]
		if !ok {
			return nil, ""
		}
		for mod, mdir := range g.modules {
			if imp == mod || strings.HasPrefix(imp, mod+"/") {
				pdir := path.Join(mdir, strings.TrimPrefix(strings.TrimPrefix(imp, mod), "/"))
				if v, ok := g.pkgConsts(pdir)[x.Sel.Name]; ok {
					return v, pdir
				}
			}
		}
	}
	return nil, ""
}

// String evaluates a constant string expression (literals, + and named constants), or reports false.
func (g *goIndex) String(gf *goFile, e ast.Expr) (string, bool) { return g.str(gf, gf.pkg, e, 0) }

func (g *goIndex) str(gf *goFile, dir string, e ast.Expr, depth int) (string, bool) {
	if depth > 16 {
		return "", false
	}
	switch x := e.(type) {
	case *ast.BasicLit:
		if x.Kind == token.STRING {
			s, err := strconv.Unquote(x.Value)
			return s, err == nil
		}
	case *ast.ParenExpr:
		return g.str(gf, dir, x.X, depth+1)
	case *ast.BinaryExpr:
		if x.Op == token.ADD {
			a, ok1 := g.str(gf, dir, x.X, depth+1)
			b, ok2 := g.str(gf, dir, x.Y, depth+1)
			return a + b, ok1 && ok2
		}
	case *ast.Ident, *ast.SelectorExpr:
		if v, vdir := g.lookup(gf, dir, x); v != nil {
			return g.str(gf, vdir, v, depth+1)
		}
	}
	return "", false
}

// Int evaluates a constant integer expression (literals, + - * << | &, parentheses, named constants).
func (g *goIndex) Int(gf *goFile, e ast.Expr) (int64, bool) { return g.int(gf, gf.pkg, e, 0) }

func (g *goIndex) int(gf *goFile, dir string, e ast.Expr, depth int) (int64, bool) {
	if depth > 16 {
		return 0, false
	}
	switch x := e.(type) {
	case *ast.BasicLit:
		if x.Kind == token.INT {
			v, err := strconv.ParseInt(strings.ReplaceAll(x.Value, "_", ""), 0, 64)
			return v, err == nil
		}
	case *ast.ParenExpr:
		return g.int(gf, dir, x.X, depth+1)
	case *ast.CallExpr: // conversions: int64(x), uint(x)
		if len(x.Args) == 1 {
			return g.int(gf, dir, x.Args[0], depth+1)
		}
	case *ast.BinaryExpr:
		a, ok1 := g.int(gf, dir, x.X, depth+1)
		b, ok2 := g.int(gf, dir, x.Y, depth+1)
		if !ok1 || !ok2 {
			return 0, false
		}
		switch x.Op {
		case token.ADD:
			return a + b, true
		case token.SUB:
			return a - b, true
		case token.MUL:
			return a * b, true
		case token.SHL:
			return a << uint(b&63), true
		case token.OR:
			return a | b, true
		case token.AND:
			return a & b, true
		}
	case *ast.Ident, *ast.SelectorExpr:
		if v, vdir := g.lookup(gf, dir, x); v != nil {
			return g.int(gf, vdir, v, depth+1)
		}
	}
	return 0, false
}

// calleeName is the called function or method name: `Sel` of x.Sel(...), or the identifier.
func calleeName(c *ast.CallExpr) string {
	switch f := c.Fun.(type) {
	case *ast.SelectorExpr:
		return f.Sel.Name
	case *ast.Ident:
		return f.Name
	}
	return ""
}

// typeName is the last identifier of a composite literal's type (`jetstream.KeyValueConfig` ->
// KeyValueConfig), looking through & and pointers.
func typeName(e ast.Expr) string {
	switch t := e.(type) {
	case *ast.SelectorExpr:
		return t.Sel.Name
	case *ast.Ident:
		return t.Name
	case *ast.StarExpr:
		return typeName(t.X)
	}
	return ""
}

// field returns the value of a keyed field of a composite literal.
func field(cl *ast.CompositeLit, name string) ast.Expr {
	for _, el := range cl.Elts {
		if kv, ok := el.(*ast.KeyValueExpr); ok {
			if k, ok := kv.Key.(*ast.Ident); ok && k.Name == name {
				return kv.Value
			}
		}
	}
	return nil
}
