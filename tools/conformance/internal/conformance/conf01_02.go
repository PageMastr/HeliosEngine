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
	cTTLRE    = regexp.MustCompile(`(\.|->)\s*(TTL|MaxAge|LimitMarkerTTL)\s*=`)
	cCASRE    = regexp.MustCompile(`\bkvStore_(Create|Update)(String)?\s*\(`)
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
		seen := map[*ast.CompositeLit]bool{}
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
			case *ast.AssignStmt: // cfg.Bucket = "…" after the literal
				for i, lhs := range x.Lhs {
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
				if typeName(x.Type) != "KeyValueConfig" && typeName(x.Type) != "StreamConfig" {
					return true
				}
				b, ok := bucketOf(g, gf, x)
				kv := typeName(x.Type) == "KeyValueConfig"
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
			return true
		})
	}
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
	leaseBucket := false
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
	for i, l := range src.blankLines {
		if leaseBucket && cTTLRE.MatchString(l) {
			p.Report(f, src.origin[i]+1, "TTL on a lease or leader KV bucket (or one the lint cannot resolve): lease "+
				"expiry in NATS (05 §1.4.1–1.4.2)")
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
			head := b[bytes.LastIndexAny(b[:i], ";{}")+1 : i]
			if cClassHeadRE.Match(head) && !bytes.ContainsAny(cAttrRE.ReplaceAll(head, nil), "(=") {
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
