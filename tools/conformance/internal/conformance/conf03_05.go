package conformance

import (
	"go/ast"
	"go/token"
	"path"
	"regexp"
	"strconv"
	"strings"
)

// CONF-03 (05 §1.4.2, the holder rule) is a behavioural rule: the lint checks that the required
// conformance test exists in each language of its scope, and CI runs it.
var _ = register(&Rule{
	ID:     "CONF-03",
	Anchor: "05 §1.4.2 (holder rule)",
	Title:  "The required test conformance/holder_rule is missing from the Go Agent or the C++ cell host",
	// "The cell host" of the table is engine/server (WP-0.14's ZoneHost, 04 §11.1).
	Scope: []string{"services/internal/orchestrator/**", "engine/server/**"},
	Types: regexp.MustCompile(`\.go$|` + cFamily.String()),
	Check: checkRequiredTests,
})

// holderRuleTests are CONF-03's own required tests; the map can require more (D2 `tests`).
var holderRuleTests = []string{"conformance/holder_rule"}

var cTestCaseRE = regexp.MustCompile(`\b(?:DOCTEST_)?TEST_CASE\s*\(\s*"conformance/([A-Za-z0-9_]+)`)

func checkRequiredTests(p *Pass) {
	g := p.Tree.goIndex()
	goCases, cCases := map[string]bool{}, map[string]bool{}
	hasGo, hasC := "", ""
	for _, f := range p.Files {
		own := MatchAny(p.Rule.Scope, f) // report a missing test under the table's scope when it has files
		if strings.HasSuffix(f, ".go") {
			if hasGo == "" || own && !MatchAny(p.Rule.Scope, hasGo) {
				hasGo = f
			}
			if !strings.HasSuffix(f, "_test.go") {
				continue
			}
			gf := g.file(f)
			if gf.File == nil {
				p.Report(f, 0, "cannot parse: %v", gf.Err)
				continue
			}
			for _, d := range gf.File.Decls {
				fd, ok := d.(*ast.FuncDecl)
				if !ok || fd.Name.Name != "TestConformance" || fd.Body == nil {
					continue
				}
				ast.Inspect(fd.Body, func(n ast.Node) bool {
					if c, ok := n.(*ast.CallExpr); ok && calleeName(c) == "Run" && len(c.Args) == 2 {
						if s, ok := g.String(gf, c.Args[0]); ok {
							goCases[s] = true
						}
					}
					return true
				})
			}
			continue
		}
		if hasC == "" || own && !MatchAny(p.Rule.Scope, hasC) {
			hasC = f
		}
		for _, l := range codeLines(p.Tree.Lines(f), false) {
			for _, m := range cTestCaseRE.FindAllStringSubmatch(l, -1) {
				cCases[m[1]] = true
			}
		}
	}
	for _, t := range dedupe(append(append([]string(nil), holderRuleTests...), p.Tests...)) {
		name := strings.TrimPrefix(t, "conformance/")
		if hasGo != "" && !goCases[name] {
			p.Report(scopeDir(p, hasGo), 0, "required test %s is missing: no t.Run(%q, …) in a func TestConformance "+
				"(05 §1.4.2; 09 §5.10.3)", t, name)
		}
		if hasC != "" && !cCases[name] {
			p.Report(scopeDir(p, hasC), 0, "required test %s is missing: no TEST_CASE(\"%s…\") in the C++ scope "+
				"(05 §1.4.2; 09 §5.10.3)", t, t)
		}
	}
}

// scopeDir is the directory of the first scope glob that matches f ("engine/server/**" -> "engine/server"),
// where a missing test is reported.
func scopeDir(p *Pass, f string) string {
	for _, g := range p.Scope {
		if Match(g, f) {
			return strings.TrimSuffix(strings.TrimSuffix(g, "**"), "/")
		}
	}
	return path.Dir(f)
}

// CONF-04 (ADR-004, 05 §1.4.5): IDs are time-prefixed blocks, 41-bit prefix | 5-bit shard | 17-bit offset,
// composed only by pkg/idgen and the C++ EntityRegistry minter. There are no node IDs.
var _ = register(&Rule{
	ID:     "CONF-04",
	Anchor: "ADR-004, 05 §1.4.5",
	Title:  "Node-ID minters: node/worker/machine/datacenter IDs in ID code, Snowflake layouts, IDs composed outside the minters",
	Scope:  []string{"services/**", "engine/ecs/**", "engine/net/**"},
	Types:  regexp.MustCompile(`\.go$|\.toml$|` + cFamily.String()),
	Check:  checkNodeIDs,
})

var (
	nodeIDWordRE = regexp.MustCompile(`(^|_)(node|worker|machine|datacenter)_?ids?(_|$)`)
	nodeIDKeyRE  = regexp.MustCompile(`(?i)\b(node|worker|machine|datacenter)[_-]?id\b`)
	camelRE      = regexp.MustCompile(`([a-z0-9])([A-Z])`)
	// Files that compose, parse or allocate 64-bit IDs (09 §5.10.3's "ID code").
	idCodeRE   = regexp.MustCompile(`idgen|AllocateIdBlocks|allocateBlockPrefixes|IdMinter|composeBlockId|BlockIdLayout|(?i:snowflake)`)
	idCodeName = regexp.MustCompile(`(?i)(^|_)(ids?|idgen|entity_?id|snowflake|minter)(_|\.)`)
	// The minters: the only places that may compose a time-prefixed ID (05 §1.4.5 "Implementation").
	idMinters   = []string{"services/pkg/idgen/**", "engine/ecs/**/entity_id.*", "engine/ecs/**/registry.*"}
	cIdentRE    = regexp.MustCompile(`[A-Za-z_][A-Za-z0-9_]*`)
	cShiftRE    = regexp.MustCompile(`([A-Za-z0-9_)\]]+)\s*<<=?\s*\(?\s*((?:[A-Za-z_][A-Za-z0-9_]*::)*[A-Za-z_][A-Za-z0-9_]*|\d+)`)
	cConstRE    = regexp.MustCompile(`\bconstexpr\s+[A-Za-z0-9_:<> ]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;{}]+);`)
	cIntTokenRE = regexp.MustCompile(`^(\d+)[uUlL]*$`)
)

func isNodeID(ident string) bool {
	return nodeIDWordRE.MatchString(strings.ToLower(camelRE.ReplaceAllString(ident, "${1}_${2}")))
}

type shift struct {
	amount int64
	line   int
}

func checkNodeIDs(p *Pass) {
	g := p.Tree.goIndex()
	cConsts := cConstTable(p)
	for _, f := range p.Files {
		text := p.Tree.Text(f)
		idCode := idCodeRE.MatchString(text) || idCodeName.MatchString(path.Base(f))
		var shifts []shift
		switch {
		case strings.HasSuffix(f, ".toml"):
			for i, l := range p.Tree.Lines(f) {
				if m := regexp.MustCompile(`^\s*([A-Za-z0-9_-]+)\s*=`).FindStringSubmatch(l); m != nil && nodeIDKeyRE.MatchString(m[1]) {
					p.Report(f, i+1, "config key %q configures a node-ID minter; IDs come from time-prefixed blocks (05 §1.4.5)", m[1])
				}
			}
			continue
		case strings.HasSuffix(f, ".go"):
			gf := g.file(f)
			if gf.File == nil {
				p.Report(f, 0, "cannot parse: %v", gf.Err)
				continue
			}
			ast.Inspect(gf.File, func(n ast.Node) bool {
				switch x := n.(type) {
				case *ast.Ident:
					if idCode && isNodeID(x.Name) {
						p.Report(f, g.line(x.Pos()), "identifier %s names a node-ID minter; IDs come from time-prefixed "+
							"blocks (ADR-004, 05 §1.4.5)", x.Name)
					}
				case *ast.BasicLit:
					if idCode && x.Kind == token.STRING && nodeIDKeyRE.MatchString(x.Value) {
						p.Report(f, g.line(x.Pos()), "config key %s names a node-ID minter (05 §1.4.5)", x.Value)
					}
				case *ast.BinaryExpr:
					if x.Op == token.SHL {
						if _, lit := x.X.(*ast.BasicLit); !lit {
							if v, ok := g.Int(gf, x.Y); ok {
								shifts = append(shifts, shift{v, g.line(x.Pos())})
							}
						}
					}
				}
				return true
			})
		default:
			code := codeLines(p.Tree.Lines(f), true)
			withStrings := codeLines(p.Tree.Lines(f), false)
			for i, l := range code {
				if idCode {
					for _, id := range cIdentRE.FindAllString(l, -1) {
						if isNodeID(id) {
							p.Report(f, i+1, "identifier %s names a node-ID minter; IDs come from time-prefixed blocks "+
								"(ADR-004, 05 §1.4.5)", id)
						}
					}
					for _, m := range cStringRE.FindAllStringSubmatch(withStrings[i], -1) {
						if nodeIDKeyRE.MatchString(m[1]) {
							p.Report(f, i+1, "config key %q names a node-ID minter (05 §1.4.5)", m[1])
						}
					}
				}
				for _, m := range cShiftRE.FindAllStringSubmatch(l, -1) {
					if cIntTokenRE.MatchString(m[1]) { // 1 << 12 is a size, not a field
						continue
					}
					if v, ok := cEval(m[2], cConsts, 0); ok {
						shifts = append(shifts, shift{v, i + 1})
					}
				}
			}
		}
		checkLayouts(p, f, shifts)
	}
}

// checkLayouts reports the retired layouts and a time-prefixed composition outside the minters.
func checkLayouts(p *Pass, f string, shifts []shift) {
	has := map[int64]int{}
	for _, s := range shifts {
		if has[s.amount] == 0 {
			has[s.amount] = s.line
		}
	}
	switch {
	case has[22] > 0 && has[12] > 0:
		p.Report(f, has[22], "the Snowflake 41/10/12 layout (<< 22 with << 12): IDs are 41/5/17 time-prefixed blocks "+
			"(ADR-004, 05 §1.4.5)")
	case has[22] > 0 && has[17] > 0 && has[9] > 0:
		p.Report(f, has[22], "the retired 41/5/8/9 layout (<< 22, << 17, << 9): IDs are 41/5/17 blocks (05 §1.4.5)")
	case has[22] > 0 && !MatchAny(idMinters, f):
		p.Report(f, has[22], "a time-prefixed ID composed (<< 22) outside pkg/idgen and the EntityRegistry minter "+
			"(05 §1.4.5 \"Implementation\")")
	}
}

// cConstTable collects `constexpr … NAME = expr;` from the C-family files in scope, by bare name.
func cConstTable(p *Pass) map[string]string {
	t := map[string]string{}
	for _, f := range p.Files {
		if !cFamily.MatchString(f) {
			continue
		}
		for _, m := range cConstRE.FindAllStringSubmatch(strings.Join(codeLines(p.Tree.Lines(f), true), "\n"), -1) {
			if _, dup := t[m[1]]; !dup {
				t[m[1]] = m[2]
			}
		}
	}
	return t
}

// cEval evaluates a C++ constant: integer literals, names from the table (qualifiers dropped), + and
// parentheses — enough for `kOffsetBits + kShardBits`.
func cEval(expr string, t map[string]string, depth int) (int64, bool) {
	expr = strings.TrimSpace(expr)
	for strings.HasPrefix(expr, "(") && strings.HasSuffix(expr, ")") {
		expr = strings.TrimSpace(expr[1 : len(expr)-1])
	}
	if depth > 8 || expr == "" {
		return 0, false
	}
	if parts := strings.Split(expr, "+"); len(parts) > 1 {
		var sum int64
		for _, part := range parts {
			v, ok := cEval(part, t, depth+1)
			if !ok {
				return 0, false
			}
			sum += v
		}
		return sum, true
	}
	if m := cIntTokenRE.FindStringSubmatch(expr); m != nil {
		v, err := strconv.ParseInt(m[1], 10, 64)
		return v, err == nil
	}
	if i := strings.LastIndex(expr, "::"); i >= 0 {
		expr = expr[i+2:]
	}
	if v, ok := t[expr]; ok {
		return cEval(v, t, depth+1)
	}
	return 0, false
}

// CONF-05 (05 §1.4.5 "Who mints"): only minting services import pkg/idgen or allocate ID blocks.
var _ = register(&Rule{
	ID:     "CONF-05",
	Anchor: "05 §1.4.5 (who mints)",
	Title:  "pkg/idgen imported, or AllocateIdBlocks called, outside the minter allow-list",
	Scope:  []string{"services/**"},
	Types:  regexp.MustCompile(`\.go$`),
	Check:  checkMinters,
})

// minterPackages is 05 §1.4.5's list as services/internal package directories (world state and
// lifecycle cleanup under their likely names), plus the orchestrator that allocates blocks and
// internal/backend's wiring. Tests are allowed: _test.go files and test-helper packages.
var minterPackages = []string{"identity", "character", "ledger", "market", "industry", "mail", "worldstate",
	"world", "activity", "lifecycle", "orchestrator", "backend"}

func minterAllowed(f string) bool {
	if strings.HasSuffix(f, "_test.go") || strings.Contains(f, "/pkg/idgen/") {
		return true
	}
	parts := strings.Split(f, "/")
	for i, part := range parts[:len(parts)-1] {
		if strings.HasSuffix(part, "test") || part == "testkit" || part == "testdata" {
			return true
		}
		if part == "internal" && i+1 < len(parts)-1 {
			for _, m := range minterPackages {
				if parts[i+1] == m {
					return true
				}
			}
		}
	}
	return false
}

func checkMinters(p *Pass) {
	g := p.Tree.goIndex()
	for _, f := range p.Files {
		if minterAllowed(f) {
			continue
		}
		gf := g.file(f)
		if gf.File == nil {
			p.Report(f, 0, "cannot parse: %v", gf.Err)
			continue
		}
		for _, im := range gf.File.Imports {
			if ip, _ := strconv.Unquote(im.Path.Value); strings.HasSuffix(ip, "/pkg/idgen") {
				p.Report(f, g.line(im.Pos()), "imports pkg/idgen outside the minters of 05 §1.4.5 (session IDs are "+
					"random 63-bit values)")
			}
		}
		ast.Inspect(gf.File, func(n ast.Node) bool {
			if c, ok := n.(*ast.CallExpr); ok && calleeName(c) == "AllocateIdBlocks" {
				p.Report(f, g.line(c.Pos()), "AllocateIdBlocks called outside the minters of 05 §1.4.5")
			}
			return true
		})
	}
}
