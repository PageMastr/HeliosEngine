package conformance

import (
	"go/ast"
	"go/token"
	"regexp"
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
	cBucketRE = regexp.MustCompile(`(\.|->)\s*Bucket\s*=\s*"([^"]*)"`)
	cTTLRE    = regexp.MustCompile(`(\.|->)\s*(TTL|MaxAge|LimitMarkerTTL)\s*=`)
	cCASRE    = regexp.MustCompile(`\bkvStore_(Create|Update)\s*\(`)
	cStringRE = regexp.MustCompile(`"((?:[^"\\]|\\.)*)"`)
)

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
		exempt := map[*ast.CallExpr]bool{}
		if strings.HasSuffix(f, "_test.go") {
			absenceChecks(gf.File, exempt)
		}
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
				}
				if ttl {
					checkKVCallTTL(p, g, gf, x, name)
				}
			case *ast.CompositeLit:
				if typeName(x.Type) != "KeyValueConfig" && typeName(x.Type) != "StreamConfig" {
					return true
				}
				b, ok := bucketOf(g, gf, x)
				if !ttl && !seen[x] && typeName(x.Type) == "KeyValueConfig" && ok && leaseRE.MatchString(b) {
					p.Report(f, g.line(x.Pos()), "KeyValueConfig for bucket %q: lease and leader state lives in "+
						"PostgreSQL, KV holds only read projections (05 §2.3, §1.4)", b)
				}
				if ttl && ok && leaseRE.MatchString(b) {
					for _, fl := range kvTTLFields {
						if v := field(x, fl); v != nil {
							p.Report(f, g.line(v.Pos()), "%s on bucket %q: lease expiry in NATS, where the holder "+
								"rule keeps a region until a higher lease_gen (05 §1.4.2)", fl, b)
						}
					}
				}
			}
			return true
		})
	}
}

// checkKVCallTTL reports per-key TTLs (jetstream.KeyTTL) on keys that are, or may be, lease state,
// and compare-and-set (KV Create, or Update with a revision) on leader-like keys.
func checkKVCallTTL(p *Pass, g *goIndex, gf *goFile, c *ast.CallExpr, name string) {
	key, keyOK := "", false
	if len(c.Args) >= 2 {
		key, keyOK = g.String(gf, c.Args[1])
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
	cas := (name == "Update" && len(c.Args) == 4) || (name == "Create" && len(c.Args) >= 3)
	if cas && keyOK && leaderKeyRE.MatchString(key) {
		p.Report(gf.Path, g.line(c.Pos()), "KV compare-and-set (%s) on %q: leadership is a PostgreSQL row with "+
			"term-fenced writes (05 §1.4.1)", name, key)
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

// checkKVC is the nats.c form of both rules, on comment-free code lines (strings kept).
func checkKVC(p *Pass, f string, ttl bool) {
	lines := codeLines(p.Tree.Lines(f), false)
	leaseBucket := false
	for _, l := range lines {
		if m := cBucketRE.FindStringSubmatch(l); m != nil && leaseRE.MatchString(m[2]) {
			leaseBucket = true
		}
	}
	for i, l := range lines {
		switch {
		case !ttl && cKVBindRE.MatchString(l) && anyString(l, leaseRE):
			p.Report(f, i+1, "nats.c KV bucket named for leases or leadership: that state lives in PostgreSQL (05 §2.3)")
		case !ttl && cBucketRE.MatchString(l) && leaseRE.MatchString(cBucketRE.FindStringSubmatch(l)[2]):
			p.Report(f, i+1, "kvConfig.Bucket %q: lease and leader state lives in PostgreSQL (05 §2.3, §1.4)",
				cBucketRE.FindStringSubmatch(l)[2])
		case ttl && leaseBucket && cTTLRE.MatchString(l):
			p.Report(f, i+1, "TTL on a lease or leader KV bucket: lease expiry in NATS (05 §1.4.1–1.4.2)")
		case ttl && cCASRE.MatchString(l) && anyString(l, leaderKeyRE):
			p.Report(f, i+1, "nats.c KV compare-and-set on a leader-like key: leadership is a PostgreSQL row (05 §1.4.1)")
		}
	}
}

func anyString(l string, re *regexp.Regexp) bool {
	for _, m := range cStringRE.FindAllStringSubmatch(l, -1) {
		if re.MatchString(m[1]) {
			return true
		}
	}
	return false
}
