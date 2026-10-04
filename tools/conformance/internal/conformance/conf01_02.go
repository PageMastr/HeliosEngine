package conformance

import (
	"bytes"
	"fmt"
	"go/ast"
	"go/token"
	"regexp"
	"slices"
	"sort"
	"strconv"
	"strings"
)

// The NATS rules (05 §1.4, §2.3; ADR-004): leases, generations and leadership live in PostgreSQL.
// JetStream KV holds only read projections (the DIRECTORY bucket), never lease or leader state.
var natsScope = []string{"services/**", "engine/net/**", "apps/**"}

var _ = register(&Rule{
	ID:     "CONF-01",
	Anchor: "05 §2.3, §1.4",
	Title:  "A JetStream KV bucket named LEASES, or matching lease|leader|fence, created, bound or read",
	Scope:  natsScope,
	Types:  regexp.MustCompile(`\.go$|` + cFamily.String()),
	Check:  func(p *Pass) { checkKV(p, false) },
})

var _ = register(&Rule{
	ID:     "CONF-02",
	Anchor: "05 §1.4.1–1.4.2",
	Title:  "Lease or leadership state in NATS: per-key TTL, MaxAge or TTL on such a bucket, or KV CAS for leadership",
	Scope:  natsScope,
	Types:  regexp.MustCompile(`\.go$|` + cFamily.String()),
	Check:  func(p *Pass) { checkKV(p, true) },
})

var (
	leaseRE     = regexp.MustCompile(`(?i)lease|leader|fence`)
	leaderKeyRE = regexp.MustCompile(`(?i)lease|leader|fence|elect|term|lock`)
	kvBindCalls = map[string]bool{"CreateKeyValue": true, "CreateOrUpdateKeyValue": true, "UpdateKeyValue": true,
		"KeyValue": true}
	kvTTLFields = []string{"TTL", "LimitMarkerTTL", "MaxAge"}
	// nats.c (C and C++): js_CreateKeyValue / js_KeyValue / js_UpdateKeyValue, kvConfig fields, and the
	// kvStore_Create / kvStore_Update compare-and-set calls.
	cKVBindRE = regexp.MustCompile(`\bjs_(Create|Update)?KeyValue\s*\(`)
	cBucketRE = regexp.MustCompile(`(?:\.|->)\s*Bucket\s*=\s*([^;]+);`)
	// kvConfig's expiry fields. MaxAge is a jsStreamConfig field: a stream's retention, lease expiry only on
	// a lease bucket's KV_ stream (cStreamNameRE), as on the Go side.
	cTTLRE        = regexp.MustCompile(`(?:\.|->)\s*(?:TTL|LimitMarkerTTL)\s*=`)
	cMaxAgeRE     = regexp.MustCompile(`(?:\.|->)\s*MaxAge\s*=`)
	cStreamNameRE = regexp.MustCompile(`(?:\.|->)\s*Name\s*=\s*([^;]+);`)
	// nats.c code: a file that includes nats.h (`<nats.h>` or `<nats/nats.h>`) or names a kvConfig.
	cNatsRE = regexp.MustCompile(`#\s*include\s*[<"](?:nats/)?nats\.h[>"]|\bkvConfig\b`)
	// kvStore_Create, _Update and their String and WithTTL (per-key TTL) variants.
	cCASRE    = regexp.MustCompile(`\bkvStore_(Create|Update)(String)?(WithTTL)?\s*\(`)
	cStringRE = regexp.MustCompile(`"((?:[^"\\]|\\.)*)"`)
)

// The nats.go import paths. A file that imports one is NATS code, so a KV call in it whose bucket or key
// the lint cannot resolve fails closed; methods are matched without types, so in other files only a
// resolved lease or leader name is reported (session's own Store.Create is not KV compare-and-set).
const (
	jetstreamImport = "github.com/nats-io/nats.go/jetstream"
	natsImport      = "github.com/nats-io/nats.go"
)

// natsAPIs reports whether gf imports jetstream (the current API) and nats.go (the legacy KV API).
func natsAPIs(gf *goFile) (js, legacy bool) {
	for _, imp := range gf.imps {
		js = js || imp == jetstreamImport
		legacy = legacy || imp == natsImport
	}
	return js, legacy
}

func checkKV(p *Pass, ttl bool) {
	g := p.Tree.goIndex()
	for _, f := range p.Files {
		if !strings.HasSuffix(f, ".go") {
			checkKVC(p, f, ttl)
			continue
		}
		gf := g.file(f)
		if gf.File == nil {
			p.Report(f, 0, "cannot parse: %v", gf.Err)
			continue
		}
		jsAPI, legacyAPI := natsAPIs(gf)
		exempt := map[*ast.CallExpr]bool{}
		if strings.HasSuffix(f, "_test.go") {
			absenceChecks(gf.File, exempt)
		}
		configs := kvConfigNames(gf.File)
		var vars cfgVars
		if ttl {
			vars = configVars(g, gf)
		}
		seen := map[*ast.CompositeLit]bool{}
		// checkCfgLit checks a KeyValueConfig (kv) or StreamConfig literal.
		checkCfgLit := func(x *ast.CompositeLit, kv bool) {
			b, ok := bucketOf(g, gf, x)
			if !ttl && !seen[x] && kv {
				switch {
				case ok && leaseRE.MatchString(b):
					p.Report(f, g.line(x.Pos()), "KeyValueConfig for bucket %q: lease and leader state lives in "+
						"PostgreSQL, KV holds only read projections (05 §2.3, §1.4)", b)
				case !ok && field(x, "Bucket") != nil && (jsAPI || legacyAPI):
					p.Report(f, g.line(x.Pos()), "KeyValueConfig for a bucket the lint cannot resolve: lease and "+
						"leader state lives in PostgreSQL (05 §2.3, §1.4); if this is a read projection, say so in "+
						"a conformance:allow")
				}
			}
			// A TTL on a KV bucket the lint cannot resolve fails closed too (CONF-01 reports the bucket);
			// a stream's MaxAge is ordinary retention unless the stream is a lease bucket's KV_ stream.
			if ttl && (ok && leaseRE.MatchString(b) || kv && !ok && (jsAPI || legacyAPI)) {
				what := fmt.Sprintf("bucket %q", b)
				if !ok {
					what = "a bucket the lint cannot resolve"
				}
				for _, fl := range kvTTLFields {
					if v := field(x, fl); v != nil {
						p.Report(f, g.line(v.Pos()), "%s on %s: lease expiry in NATS, where the holder "+
							"rule keeps a region until a higher lease_gen (05 §1.4.2)", fl, what)
					}
				}
			}
		}
		ast.Inspect(gf.File, func(n ast.Node) bool {
			switch x := n.(type) {
			case *ast.CallExpr:
				name := calleeName(x)
				if kvBindCalls[name] && !ttl {
					for _, a := range x.Args {
						if cl := kvConfigLit(a); cl != nil {
							seen[cl] = true
							if b, ok := bucketOf(g, gf, cl); ok && leaseRE.MatchString(b) {
								p.Report(f, g.line(x.Pos()), "%s of JetStream KV bucket %q: lease and leader state lives "+
									"in PostgreSQL, KV holds only read projections (05 §2.3, §1.4)", name, b)
							}
						} else if b, ok := g.String(gf, a); ok && leaseRE.MatchString(b) && !exempt[x] {
							p.Report(f, g.line(x.Pos()), "%s of JetStream KV bucket %q: lease and leader state lives "+
								"in PostgreSQL, KV holds only read projections (05 §2.3, §1.4)", name, b)
						}
					}
					// The bucket (KeyValue) or config (the others) is the last argument: jetstream's calls take a
					// context first, the legacy API's do not.
					if (jsAPI && len(x.Args) == 2 || legacyAPI && len(x.Args) == 1) && !exempt[x] &&
						!kvBucketResolved(g, gf, x.Args[len(x.Args)-1], configs) {
						p.Report(f, g.line(x.Pos()), "%s of a KV bucket the lint cannot resolve: lease and leader state "+
							"lives in PostgreSQL (05 §2.3, §1.4); if this is a read projection, say so in a "+
							"conformance:allow", name)
					}
				}
				if ttl {
					checkKVCallTTL(p, g, gf, x, name, jsAPI, legacyAPI)
				}
			case *ast.AssignStmt: // cfg.Bucket = "…" and cfg.TTL = … after the literal
				for i, lhs := range x.Lhs {
					if sel, ok := lhs.(*ast.SelectorExpr); ok && ttl && slices.Contains(kvTTLFields, sel.Sel.Name) {
						checkTTLAssign(p, g, gf, sel, vars, jsAPI || legacyAPI)
					}
					if sel, ok := lhs.(*ast.SelectorExpr); ok && sel.Sel.Name == "Bucket" && !ttl && i < len(x.Rhs) {
						b, ok := g.String(gf, x.Rhs[i])
						switch {
						case ok && leaseRE.MatchString(b):
							p.Report(f, g.line(x.Pos()), "KV bucket %q set on a config: lease and leader state lives "+
								"in PostgreSQL, KV holds only read projections (05 §2.3, §1.4)", b)
						case !ok && (jsAPI || legacyAPI):
							p.Report(f, g.line(x.Pos()), "KV bucket set on a config from a value the lint cannot "+
								"resolve: lease and leader state lives in PostgreSQL (05 §2.3, §1.4); if this is a "+
								"read projection, say so in a conformance:allow")
						}
					}
				}
			case *ast.BasicLit: // a raw `$KV.<bucket>.` subject reaches the bucket without the KV API
				if v, err := strconv.Unquote(x.Value); err == nil && x.Kind == token.STRING && !ttl {
					if m := kvSubjectRE.FindStringSubmatch(v); m != nil && leaseRE.MatchString(m[1]) {
						p.Report(f, g.line(x.Pos()), "raw KV subject of bucket %q: lease and leader state lives in "+
							"PostgreSQL (05 §2.3)", m[1])
					}
				}
			case *ast.CompositeLit:
				// A slice, array or map of configs: its elements may elide their type (`{{Bucket: …}}`).
				if el := configElem(x.Type); el != nil {
					if kv, ok := configKind(el); ok {
						for _, e := range elementLits(x) {
							if e.Type == nil {
								checkCfgLit(e, kv)
							}
						}
					}
					return true
				}
				if kv, ok := configKind(x.Type); ok {
					checkCfgLit(x, kv)
				}
			}
			return true
		})
	}
}

// elementLits returns the composite literals among a collection literal's elements (`{…}`, `&T{…}`, and a
// map's values).
func elementLits(cl *ast.CompositeLit) []*ast.CompositeLit {
	var out []*ast.CompositeLit
	for _, e := range cl.Elts {
		if kv, ok := e.(*ast.KeyValueExpr); ok {
			e = kv.Value
		}
		if u, ok := e.(*ast.UnaryExpr); ok && u.Op == token.AND {
			e = u.X
		}
		if ecl, ok := e.(*ast.CompositeLit); ok {
			out = append(out, ecl)
		}
	}
	return out
}

// kvConfigNames returns the names this file gives a KeyValueConfig whose bucket it sets: one initialised
// from a KeyValueConfig literal that has a Bucket field, or one whose .Bucket it assigns. A bind call that
// passes such a name is checked where the bucket is set; any other config variable fails closed.
func kvConfigNames(file *ast.File) map[string]bool {
	names := map[string]bool{}
	note := func(lhs ast.Expr, rhs ast.Expr) {
		if id, ok := lhs.(*ast.Ident); ok {
			if cl := kvConfigLit(rhs); cl != nil && field(cl, "Bucket") != nil {
				names[id.Name] = true
			}
		}
	}
	ast.Inspect(file, func(n ast.Node) bool {
		switch x := n.(type) {
		case *ast.AssignStmt:
			for i, lhs := range x.Lhs {
				if sel, ok := lhs.(*ast.SelectorExpr); ok && sel.Sel.Name == "Bucket" {
					if id, ok := sel.X.(*ast.Ident); ok {
						names[id.Name] = true
					}
				} else if i < len(x.Rhs) {
					note(lhs, x.Rhs[i])
				}
			}
		case *ast.ValueSpec:
			for i, id := range x.Names {
				if i < len(x.Values) {
					note(id, x.Values[i])
				}
			}
		}
		return true
	})
	return names
}

// cfgVar is a name this file gives a KeyValueConfig (kv) or a StreamConfig, with the buckets (stream names)
// the file sets on it.
type cfgVar struct {
	kv      bool
	buckets []cfgBucket
}

type cfgBucket struct {
	name string
	ok   bool // false: a value the lint cannot resolve
}

// cfgVars are the configs a file names (configVars): vars by name, and the slices, arrays and maps of
// configs (colls), whose elements are read through an index (`cfgs[i].TTL`) or a range variable.
type cfgVars struct {
	vars  map[string]*cfgVar
	colls map[string]*cfgVar
}

// of returns the config a selector's operand names: `cfg`, `s.kv`, `(&cfg)`, `*p` or `cfgs[i]`.
func (c cfgVars) of(e ast.Expr) *cfgVar {
	switch x := e.(type) {
	case *ast.ParenExpr:
		return c.of(x.X)
	case *ast.StarExpr:
		return c.of(x.X)
	case *ast.UnaryExpr:
		if x.Op == token.AND {
			return c.of(x.X)
		}
		return nil
	case *ast.IndexExpr:
		return c.colls[selectorBase(x.X)]
	}
	return c.vars[selectorBase(e)]
}

// configKind returns the config type a type expression names, and whether it names one.
func configKind(typ ast.Expr) (kv, ok bool) {
	kind := typeName(typ)
	return kind == "KeyValueConfig", kind == "KeyValueConfig" || kind == "StreamConfig"
}

// configElem returns the element type of a slice, array or map type expression, or nil.
func configElem(typ ast.Expr) ast.Expr {
	switch t := typ.(type) {
	case *ast.ArrayType:
		return t.Elt
	case *ast.MapType:
		return t.Value
	}
	return nil
}

// configVars returns the names this file gives a KeyValueConfig or a StreamConfig: a variable initialised
// from a literal of the type, from new(T), or declared with it, a parameter or struct field of it, and a
// range variable over a slice, array or map of them or a variable set from one of their elements. Each carries the buckets the file sets on it: the
// literal's Bucket (a stream's Name) and every `.Bucket` (`.Name`) assignment; a collection's elements
// share theirs. Names are matched without scopes, as kvConfigNames does.
func configVars(g *goIndex, gf *goFile) cfgVars {
	c := cfgVars{vars: map[string]*cfgVar{}, colls: map[string]*cfgVar{}}
	declareIn := func(m map[string]*cfgVar, name string, typ ast.Expr) *cfgVar {
		kv, ok := configKind(typ)
		if !ok {
			return nil
		}
		if m[name] == nil {
			m[name] = &cfgVar{kv: kv}
		}
		return m[name]
	}
	declare := func(name string, typ ast.Expr) {
		declareIn(c.vars, name, typ)
		if el := configElem(typ); el != nil {
			declareIn(c.colls, name, el)
		}
	}
	addLit := func(v *cfgVar, cl *ast.CompositeLit) {
		if v != nil && (field(cl, "Bucket") != nil || field(cl, "Name") != nil) {
			b, ok := bucketOf(g, gf, cl)
			v.buckets = append(v.buckets, cfgBucket{b, ok})
		}
	}
	fromLit := func(name string, e ast.Expr) {
		if u, ok := e.(*ast.UnaryExpr); ok && u.Op == token.AND {
			e = u.X
		}
		if call, ok := e.(*ast.CallExpr); ok && len(call.Args) == 1 {
			if id, ok := call.Fun.(*ast.Ident); ok && id.Name == "new" {
				declareIn(c.vars, name, call.Args[0])
			}
		}
		cl, ok := e.(*ast.CompositeLit)
		if !ok {
			return
		}
		addLit(declareIn(c.vars, name, cl.Type), cl)
		if el := configElem(cl.Type); el != nil {
			v := declareIn(c.colls, name, el)
			for _, ecl := range elementLits(cl) {
				addLit(v, ecl)
			}
		}
	}
	ast.Inspect(gf.File, func(n ast.Node) bool {
		switch x := n.(type) {
		case *ast.AssignStmt:
			for i, lhs := range x.Lhs {
				if id, ok := lhs.(*ast.Ident); ok && i < len(x.Rhs) {
					fromLit(id.Name, x.Rhs[i])
				}
			}
		case *ast.ValueSpec:
			for i, id := range x.Names {
				if x.Type != nil {
					declare(id.Name, x.Type)
				}
				if i < len(x.Values) {
					fromLit(id.Name, x.Values[i])
				}
			}
		case *ast.Field: // parameters, results and struct fields
			for _, id := range x.Names {
				declare(id.Name, x.Type)
			}
		}
		return true
	})
	// A range variable over a collection, or a variable set from one of its elements (`c := cfgs[0]`), is
	// an element: it shares the collection's buckets. A collection given as a literal is one too (`range
	// []KeyValueConfig{…}`).
	var merges [][2]*cfgVar
	element := func(id *ast.Ident, coll *cfgVar) {
		switch {
		case coll == nil || id.Name == "_":
		case c.vars[id.Name] == nil:
			c.vars[id.Name] = coll
		case c.vars[id.Name] != coll:
			c.vars[id.Name].kv = c.vars[id.Name].kv || coll.kv
			merges = append(merges, [2]*cfgVar{c.vars[id.Name], coll})
		}
	}
	indexed := func(e ast.Expr) *cfgVar {
		if ix, ok := e.(*ast.IndexExpr); ok {
			return c.colls[selectorBase(ix.X)]
		}
		return nil
	}
	ast.Inspect(gf.File, func(n ast.Node) bool {
		switch x := n.(type) {
		case *ast.RangeStmt:
			id, ok := x.Value.(*ast.Ident)
			if !ok {
				return true
			}
			if cl, ok := x.X.(*ast.CompositeLit); ok {
				const lit = "\x00range" // a name no identifier has
				fromLit(lit, cl)
				element(id, c.colls[lit])
				delete(c.colls, lit)
			} else {
				element(id, c.colls[selectorBase(x.X)])
			}
		case *ast.AssignStmt: // `c := cfgs[i]`, and `c, ok := m[k]`
			for i, lhs := range x.Lhs {
				if id, ok := lhs.(*ast.Ident); ok && (len(x.Lhs) == len(x.Rhs) || len(x.Rhs) == 1 && i == 0) {
					element(id, indexed(x.Rhs[i]))
				}
			}
		case *ast.ValueSpec:
			for i, id := range x.Names {
				if i < len(x.Values) && len(x.Names) == len(x.Values) {
					element(id, indexed(x.Values[i]))
				}
			}
		}
		return true
	})
	ast.Inspect(gf.File, func(n ast.Node) bool {
		if as, ok := n.(*ast.AssignStmt); ok {
			for i, lhs := range as.Lhs {
				if sel, ok := lhs.(*ast.SelectorExpr); ok && (sel.Sel.Name == "Bucket" || sel.Sel.Name == "Name") &&
					i < len(as.Rhs) {
					if v := c.of(sel.X); v != nil {
						b, ok := g.String(gf, as.Rhs[i])
						v.buckets = append(v.buckets, cfgBucket{b, ok})
					}
				}
			}
		}
		return true
	})
	for _, m := range merges {
		m[0].buckets = append(m[0].buckets, m[1].buckets...)
	}
	return c
}

// selectorBase names the value a selector reads: `cfg` for cfg.TTL, `kv` for s.kv.TTL.
func selectorBase(e ast.Expr) string {
	switch x := e.(type) {
	case *ast.Ident:
		return x.Name
	case *ast.SelectorExpr:
		return x.Sel.Name
	case *ast.StarExpr:
		return selectorBase(x.X)
	case *ast.ParenExpr:
		return selectorBase(x.X)
	}
	return ""
}

// checkTTLAssign reports `cfg.TTL = …` (or MaxAge, LimitMarkerTTL) after the literal, where cfg is a config
// this file names (configVars): on a lease-named bucket or stream, and, in NATS code, on a KV config whose
// bucket the lint cannot resolve or this file does not set.
func checkTTLAssign(p *Pass, g *goIndex, gf *goFile, sel *ast.SelectorExpr, vars cfgVars, natsCode bool) {
	v := vars.of(sel.X)
	if v == nil {
		return
	}
	unresolved := v.kv && len(v.buckets) == 0
	for _, b := range v.buckets {
		switch {
		case b.ok && leaseRE.MatchString(b.name):
			p.Report(gf.Path, g.line(sel.Pos()), "%s on bucket %q: lease expiry in NATS, where the holder rule keeps a "+
				"region until a higher lease_gen (05 §1.4.2)", sel.Sel.Name, b.name)
			return
		case !b.ok && v.kv:
			unresolved = true
		}
	}
	if unresolved && natsCode {
		p.Report(gf.Path, g.line(sel.Pos()), "%s on a bucket the lint cannot resolve: lease expiry in NATS, where the "+
			"holder rule keeps a region until a higher lease_gen (05 §1.4.2); if it is not lease state, say so in a "+
			"conformance:allow", sel.Sel.Name)
	}
}

// kvBucketResolved reports whether the bucket argument of a KV bind call is one the lint can check: a
// constant string, a KeyValueConfig literal with a constant Bucket, or a config variable whose bucket this
// file sets (kvConfigNames; checked there).
func kvBucketResolved(g *goIndex, gf *goFile, a ast.Expr, configs map[string]bool) bool {
	if cl := kvConfigLit(a); cl != nil {
		_, ok := bucketOf(g, gf, cl)
		return ok
	}
	if _, ok := g.String(gf, a); ok {
		return true
	}
	if u, ok := a.(*ast.UnaryExpr); ok && u.Op == token.AND {
		a = u.X
	}
	id, ok := a.(*ast.Ident)
	return ok && configs[id.Name]
}

// checkKVCallTTL reports per-key TTLs (jetstream.KeyTTL) on keys that are, or may be, lease state,
// and compare-and-set (KV Create, or Update with a revision) on leader-like keys. In a file that imports
// the API of the call's shape, a compare-and-set key the lint cannot resolve fails closed.
func checkKVCallTTL(p *Pass, g *goIndex, gf *goFile, c *ast.CallExpr, name string, jsAPI, legacyAPI bool) {
	key, keyOK := "", false
	if len(c.Args) >= 2 {
		key, keyOK = g.String(gf, c.Args[1])
	}
	// nats.go's legacy KV API has no context: Create(key, value) and Update(key, value, last).
	legacy := (name == "Create" && len(c.Args) == 2) || (name == "Update" && len(c.Args) == 3)
	if legacy {
		key, keyOK = g.String(gf, c.Args[0])
	}
	for _, a := range c.Args {
		if opt, ok := a.(*ast.CallExpr); ok && calleeName(opt) == "KeyTTL" && (!keyOK || leaseRE.MatchString(key)) {
			what := "a key the lint cannot resolve"
			if keyOK {
				what = "key " + `"` + key + `"`
			}
			p.Report(gf.Path, g.line(opt.Pos()), "per-key TTL on %s: lease state with a NATS expiry (05 §1.4.1–1.4.2); "+
				"if it is not lease state, say so in a conformance:allow", what)
		}
	}
	cas := legacy || (name == "Update" && len(c.Args) == 4) || (name == "Create" && len(c.Args) >= 3)
	switch {
	case cas && keyOK && leaderKeyRE.MatchString(key):
		p.Report(gf.Path, g.line(c.Pos()), "KV compare-and-set (%s) on %q: leadership is a PostgreSQL row with "+
			"term-fenced writes (05 §1.4.1)", name, key)
	case cas && !keyOK && (legacy && legacyAPI || !legacy && jsAPI):
		p.Report(gf.Path, g.line(c.Pos()), "KV compare-and-set (%s) on a key the lint cannot resolve: leadership is "+
			"a PostgreSQL row with term-fenced writes (05 §1.4.1); if the key is not leader state, say so in a "+
			"conformance:allow", name)
	}
}

func kvConfigLit(e ast.Expr) *ast.CompositeLit {
	if u, ok := e.(*ast.UnaryExpr); ok && u.Op == token.AND {
		e = u.X
	}
	if cl, ok := e.(*ast.CompositeLit); ok && typeName(cl.Type) == "KeyValueConfig" {
		return cl
	}
	return nil
}

func bucketOf(g *goIndex, gf *goFile, cl *ast.CompositeLit) (string, bool) {
	for _, name := range []string{"Bucket", "Name"} {
		if v := field(cl, name); v != nil {
			return g.String(gf, v)
		}
	}
	return "", false
}

// absenceChecks marks the KV reads that a test asserts fail: `if _, err := js.KeyValue(…); err == nil
// { t.Fatal… }`, or the same assignment followed by that if (09 §5.10.3: such tests are exempt).
func absenceChecks(file *ast.File, exempt map[*ast.CallExpr]bool) {
	ast.Inspect(file, func(n ast.Node) bool {
		blk, ok := n.(*ast.BlockStmt)
		if !ok {
			return true
		}
		for i, st := range blk.List {
			var as *ast.AssignStmt
			var check *ast.IfStmt
			if is, ok := st.(*ast.IfStmt); ok {
				as, _ = is.Init.(*ast.AssignStmt)
				check = is
			} else if a, ok := st.(*ast.AssignStmt); ok && i+1 < len(blk.List) {
				as = a
				check, _ = blk.List[i+1].(*ast.IfStmt)
			}
			if as == nil || check == nil || len(as.Rhs) != 1 || len(as.Lhs) == 0 {
				continue
			}
			call, ok := as.Rhs[0].(*ast.CallExpr)
			errVar, ok2 := as.Lhs[len(as.Lhs)-1].(*ast.Ident)
			if !ok || !ok2 || calleeName(call) != "KeyValue" || !errIsNil(check.Cond, errVar.Name) || !fails(check.Body) {
				continue
			}
			exempt[call] = true
		}
		return true
	})
}

func errIsNil(cond ast.Expr, name string) bool {
	b, ok := cond.(*ast.BinaryExpr)
	if !ok || b.Op != token.EQL {
		return false
	}
	x, ok1 := b.X.(*ast.Ident)
	y, ok2 := b.Y.(*ast.Ident)
	return ok1 && ok2 && x.Name == name && y.Name == "nil"
}

func fails(body *ast.BlockStmt) bool {
	found := false
	ast.Inspect(body, func(n ast.Node) bool {
		if c, ok := n.(*ast.CallExpr); ok {
			switch calleeName(c) {
			case "Fatal", "Fatalf", "Error", "Errorf", "Fail", "FailNow":
				found = true
			}
		}
		return !found
	})
	return found
}

// checkKVC is the nats.c form of both rules. Calls are read with their arguments joined across lines,
// and a bucket or key is resolved through the scope's string constants (cStrTable). A bucket or key the
// lint cannot resolve fails closed, as the Go side does for KeyTTL: a real DIRECTORY read through a
// variable carries a `conformance:allow` saying so.
func checkKVC(p *Pass, f string, ttl bool) {
	src := newCSource(p.Tree.Lines(f))
	consts := cStrTable(p)
	// The buckets this file configures (kvConfig.Bucket = …), and whether one is, or may be, lease state.
	type bucket struct {
		off    int
		values []string
		ok     bool
	}
	var buckets []bucket
	// A kvConfig whose Bucket this file does not set is one the lint cannot resolve.
	leaseBucket := cNatsRE.MatchString(src.text) && !cBucketRE.MatchString(src.blank)
	for _, m := range cBucketRE.FindAllStringSubmatchIndex(src.blank, -1) {
		vals, ok := cStrValues(src.text[m[2]:m[3]], consts)
		buckets = append(buckets, bucket{m[0], vals, ok})
		leaseBucket = leaseBucket || !ok || anyMatch(vals, leaseRE)
	}
	if !ttl {
		for _, b := range buckets {
			switch {
			case !b.ok:
				p.Report(f, src.line(b.off), "kvConfig.Bucket set from a value the lint cannot resolve: lease and leader "+
					"state lives in PostgreSQL (05 §2.3, §1.4); if this is a read projection, say so in a conformance:allow")
			case anyMatch(b.values, leaseRE):
				p.Report(f, src.line(b.off), "kvConfig.Bucket %q: lease and leader state lives in PostgreSQL (05 §2.3, §1.4)",
					firstMatch(b.values, leaseRE))
			}
		}
		for _, c := range src.calls(cKVBindRE) {
			if c.name != "js_KeyValue" {
				if len(buckets) == 0 { // js_CreateKeyValue/js_UpdateKeyValue take a kvConfig built elsewhere
					p.Report(f, c.line, "%s with a kvConfig whose Bucket this file does not set: the lint cannot "+
						"resolve the bucket (05 §2.3); if it is a read projection, say so in a conformance:allow", c.name)
				}
				continue
			}
			vals, ok := []string(nil), false
			if len(c.args) >= 3 {
				vals, ok = cStrValues(c.args[2], consts)
			}
			switch {
			case !ok:
				p.Report(f, c.line, "js_KeyValue of a bucket the lint cannot resolve: lease and leader state lives in "+
					"PostgreSQL (05 §2.3); if this is a read projection, say so in a conformance:allow")
			case anyMatch(vals, leaseRE):
				p.Report(f, c.line, "js_KeyValue of bucket %q: lease and leader state lives in PostgreSQL, KV holds only "+
					"read projections (05 §2.3)", firstMatch(vals, leaseRE))
			}
		}
		for i, l := range src.logical {
			for _, m := range cStringRE.FindAllStringSubmatch(l, -1) {
				if b := kvSubjectRE.FindStringSubmatch(m[1]); b != nil && leaseRE.MatchString(b[1]) {
					p.Report(f, src.origin[i]+1, "raw KV subject of bucket %q: lease and leader state lives in PostgreSQL "+
						"(05 §2.3)", b[1])
				}
			}
		}
		return
	}
	// A stream named for a lease bucket (`KV_leases`): its MaxAge is the bucket's expiry. A stream name the
	// lint cannot resolve is ordinary retention, as on the Go side; CONF-01 reports the bucket wherever
	// its kvConfig is set.
	leaseStream := ""
	for _, m := range cStreamNameRE.FindAllStringSubmatchIndex(src.blank, -1) {
		if vals, ok := cStrValues(src.text[m[2]:m[3]], consts); ok && leaseStream == "" {
			leaseStream = firstMatch(vals, leaseRE)
		}
	}
	for i, l := range src.blankLines {
		switch {
		case leaseBucket && cTTLRE.MatchString(l):
			p.Report(f, src.origin[i]+1, "TTL on a lease or leader KV bucket (or one the lint cannot resolve): lease "+
				"expiry in NATS (05 §1.4.1–1.4.2)")
		case leaseStream != "" && cMaxAgeRE.MatchString(l):
			p.Report(f, src.origin[i]+1, "MaxAge in a file that configures stream %q: lease expiry in NATS "+
				"(05 §1.4.1–1.4.2)", leaseStream)
		}
	}
	for _, c := range src.calls(cCASRE) {
		vals, ok := []string(nil), false
		if len(c.args) >= 3 {
			vals, ok = cStrValues(c.args[2], consts)
		}
		switch {
		case !ok:
			p.Report(f, c.line, "nats.c KV compare-and-set (%s) on a key the lint cannot resolve: leadership is a "+
				"PostgreSQL row (05 §1.4.1); if the key is not leader state, say so in a conformance:allow", c.name)
		case anyMatch(vals, leaderKeyRE):
			p.Report(f, c.line, "nats.c KV compare-and-set (%s) on %q: leadership is a PostgreSQL row (05 §1.4.1)",
				c.name, firstMatch(vals, leaderKeyRE))
		}
	}
}

func anyMatch(vals []string, re *regexp.Regexp) bool { return firstMatch(vals, re) != "" }

func firstMatch(vals []string, re *regexp.Regexp) string {
	for _, v := range vals {
		if re.MatchString(v) {
			return v
		}
	}
	return ""
}

// cSource is a C-family file prepared for token scans: splices joined, comments blanked. text keeps
// string literals; blank has their contents blanked too, at the same offsets, so structure (calls,
// parentheses, commas) is read from blank and values from text.
type cSource struct {
	logical    []string // logical lines, comments blanked, strings kept
	blankLines []string // the same with string contents blanked
	origin     []int    // the 0-based physical line where each logical line starts
	text       string   // logical lines joined with \n
	blank      string
	starts     []int // offset of each logical line in text
}

func newCSource(lines []string) *cSource {
	logical, origin := spliceLines(lines)
	s := &cSource{logical: codeLines(logical, false), blankLines: codeLines(logical, true), origin: origin}
	s.text, s.blank = strings.Join(s.logical, "\n"), strings.Join(s.blankLines, "\n")
	off := 0
	for _, l := range s.logical {
		s.starts = append(s.starts, off)
		off += len(l) + 1
	}
	return s
}

// index is the logical line that holds offset off.
func (s *cSource) index(off int) int {
	return max(sort.Search(len(s.starts), func(i int) bool { return s.starts[i] > off })-1, 0)
}

// line is the 1-based physical line of offset off.
func (s *cSource) line(off int) int { return s.origin[s.index(off)] + 1 }

type cCall struct {
	name  string
	line  int      // the physical line where the call starts
	index int      // the logical line where it starts
	args  []string // the arguments' source text (strings kept), split at top-level commas
}

// calls finds the calls whose name re matches (re ends with `\(`), with their arguments read up to the
// closing parenthesis, across lines. An unterminated call yields the arguments seen so far.
func (s *cSource) calls(re *regexp.Regexp) []cCall {
	return s.callsAt(re.FindAllStringIndex(s.blank, -1))
}

// callsAt reads the calls that start at the given matches (offsets into text and blank, which agree): the
// name is what precedes the first '(' of the match, and the arguments follow that parenthesis.
func (s *cSource) callsAt(locs [][]int) []cCall {
	var out []cCall
	for _, m := range locs {
		open := strings.IndexByte(s.blank[m[0]:m[1]], '(')
		if open < 0 {
			continue
		}
		open += m[0] + 1
		name := strings.TrimSpace(s.blank[m[0] : open-1])
		c := cCall{name: name, line: s.line(m[0]), index: s.index(m[0])}
		depth, from := 0, open
		for i := open; i < len(s.blank); i++ {
			ch := s.blank[i]
			switch {
			case ch == '(' || ch == '[' || ch == '{':
				depth++
			case (ch == ')' || ch == ']' || ch == '}') && depth > 0:
				depth--
			case ch == ')':
				c.args = append(c.args, strings.TrimSpace(s.text[from:i]))
				i = len(s.blank)
			case ch == ',' && depth == 0:
				c.args = append(c.args, strings.TrimSpace(s.text[from:i]))
				from = i + 1
			}
		}
		out = append(out, c)
	}
	return out
}

var (
	// `#define NAME "…"`.
	cDefineStrRE = regexp.MustCompile(`(?m)^\s*#\s*define\s+([A-Za-z_]\w*)\s+((?:"(?:[^"\\\n]|\\.)*"\s*)+)$`)
	// A declarator initialized with string literals: NAME = "…", NAME = {"…"}, NAME{"…"} or NAME("…"), with an
	// optional array bound, ending the declarator. Whether it declares a constant is decided by what precedes
	// NAME in its statement (cConstDecl).
	cInitStrRE = regexp.MustCompile(`\b([A-Za-z_]\w*)\s*(?:\[\s*\w*\s*\]\s*)?` +
		`(?:=\s*(` + cLits + `)|=?\s*\{\s*(` + cLits + `)\}|\(\s*(` + cLits + `)\))\s*[;,]`)
	// What may precede a declared name in its statement: specifiers and a type (qualified, templated, with
	// `*`, `&` and `const`). A `.`, `->`, `(`, `=` or literal there means an assignment, a call, a parameter
	// or a member access, none of which declares a constant.
	cDeclPrefixRE = regexp.MustCompile(`^(?:\w+|::|[<>,*&]|\s)*$`)
	cAccessRE     = regexp.MustCompile(`^\s*(?:(?:public|private|protected)\s*:\s|\[\[[^\]]*\]\]\s*)+`)
	cConstWordRE  = regexp.MustCompile(`\bconst\b`)
	cConstexprRE  = regexp.MustCompile(`\bconstexpr\b`)
	cStaticRE     = regexp.MustCompile(`\bstatic\b`)
	// An innermost template argument list, stripped repeatedly so that only a top-level const counts.
	cTemplateArgsRE = regexp.MustCompile(`<[^<>]*>`)
	// The head of a class, struct or union body (not an enum, a function returning a struct, or an
	// initializer), once attributes, alignas and __declspec are dropped.
	cClassHeadRE = regexp.MustCompile(`(?:^|[^\w])(?:class|struct|union)\b`)
	cAttrRE      = regexp.MustCompile(`\[\[[^\]]*\]\]|\b(?:alignas|__declspec|__attribute__)\s*\((?:[^()]|\([^()]*\))*\)`)
	cStrLitRE    = regexp.MustCompile(`"((?:[^"\\\n]|\\.)*)"`)
	cWrapRE      = regexp.MustCompile(`^(?:std::)?(?:string|string_view)\s*[({](.*)[)}]$|^static_cast\s*<[^>]*>\s*\((.*)\)$|^\(\s*(?:const\s+)?char\s*(?:const\s*)?\*\s*\)\s*(.*)$`)
	// A name, optionally qualified by namespaces or classes. A member access (a.b, p->b) is not a constant.
	cQualIdentRE = regexp.MustCompile(`^(?:::\s*)?(?:[A-Za-z_]\w*\s*::\s*)*([A-Za-z_]\w*)$`)
	kvSubjectRE  = regexp.MustCompile(`^\$KV\.([^.\s]+)`)
)

// cLits is one or more adjacent string literals.
const cLits = `(?:"(?:[^"\\\n]|\\.)*"\s*)+`

// cConstDecl reports whether prefix, the text of a statement before a declared name, makes that name a
// constant: `constexpr`, or a top-level `const` object (`const std::string k`, `static const char k[]`,
// `const char* const k`). A pointer to const (`const char* k`) is not: the pointer can be re-pointed. Nor
// is a `const` inside template arguments (`std::span<const char> k`).
func cConstDecl(prefix string) bool {
	prefix = cAccessRE.ReplaceAllString(prefix, "")
	if !cDeclPrefixRE.MatchString(prefix) {
		return false
	}
	if cConstexprRE.MatchString(prefix) {
		return true
	}
	for {
		stripped := cTemplateArgsRE.ReplaceAllString(prefix, " ")
		if stripped == prefix {
			break
		}
		prefix = stripped
	}
	if i := strings.LastIndex(prefix, "*"); i >= 0 {
		prefix = prefix[i:]
	}
	return cConstWordRE.MatchString(prefix)
}

// cStrTable collects the string constants of the C-family files in the rule's scope, by bare name
// (cConstStrings). Assignments, members set elsewhere, parameters and calls are not constants, so a bucket
// or key passed through one stays unresolved. A name defined with several values keeps them all, and a
// bucket or key matches if any value does.
func cStrTable(p *Pass) map[string][]string {
	if p.cstr != nil {
		return p.cstr
	}
	t := map[string][]string{}
	for _, f := range p.Files {
		if !cFamily.MatchString(f) {
			continue
		}
		for name, lits := range cConstStrings(newCSource(p.Tree.Lines(f))) {
			for _, l := range lits {
				v := ""
				for _, m := range cStrLitRE.FindAllStringSubmatch(l, -1) {
					v += m[1]
				}
				if !slices.Contains(t[name], v) {
					t[name] = append(t[name], v)
				}
			}
		}
	}
	p.cstr = t
	return t
}

// cConstStrings returns the string-literal initializers of the constants src declares, by bare name: a
// `#define NAME "…"`, and a declarator whose statement makes it a constant (cConstDecl) at namespace or
// block scope, or as a `static` class member. A declarator inside parentheses or brackets is a parameter
// (with its default argument), a call argument or a condition, never a constant, however the statement
// before it reads; a non-static data member's initializer is only a default, which a constructor
// overrides. Groups under `#if 0` are not read.
func cConstStrings(src *cSource) map[string][]string {
	dead := inactiveLines(src.blankLines)
	out := map[string][]string{}
	for _, m := range cDefineStrRE.FindAllStringSubmatchIndex(src.text, -1) {
		if !dead[src.index(m[0])] {
			out[src.text[m[2]:m[3]]] = append(out[src.text[m[2]:m[3]]], src.text[m[4]:m[5]])
		}
	}
	// Statement boundaries: ; { } outside strings and comments, and every preprocessor line.
	bounds := []byte(src.blank)
	for i, l := range src.blankLines {
		if strings.HasPrefix(strings.TrimSpace(l), "#") || dead[i] {
			for k := src.starts[i]; k < src.starts[i]+len(l); k++ {
				bounds[k] = ';'
			}
		}
	}
	scope := cScopes(bounds)
	for _, m := range cInitStrRE.FindAllStringSubmatchIndex(src.text, -1) {
		start := bytes.LastIndexAny(bounds[:m[2]], ";{}") + 1
		prefix := src.blank[start:m[2]]
		switch {
		case bounds[m[2]] == ';', scope[m[2]] == '(', scope[m[2]] == '[', !cConstDecl(prefix),
			scope[m[2]] == 'c' && !cStaticRE.MatchString(prefix):
			continue
		}
		for g := 4; g <= 8; g += 2 {
			if m[g] >= 0 {
				out[src.text[m[2]:m[3]]] = append(out[src.text[m[2]:m[3]]], src.text[m[g]:m[g+1]])
			}
		}
	}
	return out
}

// cScopes gives, for each offset of b (code with strings, comments and preprocessor lines blanked), the
// innermost bracket open there: 0 at namespace scope, '(' or '[', 'c' in a class, struct or union body,
// and '{' in any other brace (a function body, a lambda, a namespace or an initializer).
func cScopes(b []byte) []byte {
	out := make([]byte, len(b))
	var stack []byte
	for i, ch := range b {
		switch ch {
		case '(', '[':
			stack = append(stack, ch)
		case '{':
			kind := byte('{')
			if cClassBody(b[bytes.LastIndexAny(b[:i], ";{}")+1 : i]) {
				kind = 'c'
			}
			stack = append(stack, kind)
		case ')', ']', '}':
			if len(stack) > 0 {
				stack = stack[:len(stack)-1]
			}
		}
		if len(stack) > 0 {
			out[i] = stack[len(stack)-1]
		}
	}
	return out
}

// cClassBody reports whether the brace after head opens a class, struct or union body. The text after the
// last class key, attributes removed and cut at the base clause's ':' (not '::'), is the class name: a
// parenthesis or '=' there makes it a function (`struct S f() {`, `void g(struct S* p) {`) or an initializer
// (`struct S s = {`), while template parameters before the key (`template <class T = int> struct X {`) and
// template arguments in the base clause (`struct B : Base<(N > 1)> {`) do not count.
func cClassBody(head []byte) bool {
	keys := cClassHeadRE.FindAllIndex(head, -1)
	if keys == nil {
		return false
	}
	name := cAttrRE.ReplaceAll(head[keys[len(keys)-1][1]:], nil)
	for k := 0; k < len(name); k++ {
		if name[k] == ':' && (k+1 == len(name) || name[k+1] != ':') && (k == 0 || name[k-1] != ':') {
			name = name[:k]
			break
		}
	}
	return !bytes.ContainsAny(name, "()=")
}

// cStrValues resolves an argument or initializer to its possible string values: literals (adjacent
// ones concatenated), names from the table (namespace and class qualifiers dropped), `.c_str()` and
// `.data()`, and std::string/string_view, static_cast and C-cast wrappers. Anything else, a member
// access (`o.bucket`, `p->bucket`) among it, is unresolved.
func cStrValues(expr string, t map[string][]string) ([]string, bool) {
	for depth := 0; depth < 8; depth++ {
		expr = strings.TrimSpace(expr)
		for strings.HasPrefix(expr, "(") && strings.HasSuffix(expr, ")") && !strings.Contains(expr[1:len(expr)-1], ")") {
			expr = strings.TrimSpace(expr[1 : len(expr)-1])
		}
		expr = strings.TrimSpace(strings.TrimSuffix(strings.TrimSuffix(expr, ".c_str()"), ".data()"))
		if m := cWrapRE.FindStringSubmatch(expr); m != nil {
			expr = m[1] + m[2] + m[3]
			continue
		}
		break
	}
	if strings.HasPrefix(expr, `"`) {
		v, rest := "", expr
		for {
			loc := cStrLitRE.FindStringSubmatchIndex(rest)
			if loc == nil || strings.TrimSpace(rest[:loc[0]]) != "" {
				return nil, false
			}
			v += rest[loc[2]:loc[3]]
			rest = strings.TrimSpace(rest[loc[1]:])
			if rest == "" {
				return []string{v}, true
			}
		}
	}
	if m := cQualIdentRE.FindStringSubmatch(expr); m != nil {
		vals, ok := t[m[1]]
		return vals, ok
	}
	return nil, false
}
