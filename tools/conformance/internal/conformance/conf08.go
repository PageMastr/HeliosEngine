package conformance

import (
	"go/ast"
	"go/token"
	"regexp"
	"strconv"
	"strings"
)

// CONF-08 (04 §2; reconciliation #12): the gateway's default is UDP 7777, the only public game port.
var _ = register(&Rule{
	ID:     "CONF-08",
	Anchor: "04 §2; reconciliation #12",
	Title:  "A default gateway port other than UDP 7777",
	Scope:  []string{"services/**", "engine/net/**", "deploy/**"},
	Types:  regexp.MustCompile(`\.(go|toml|ya?ml)$|` + cFamily.String()),
	Check:  checkGatewayPort,
})

const gatewayPort = 7777

var (
	gatewayNameRE = regexp.MustCompile(`(?i)gateway`)
	gatewayPortRE = regexp.MustCompile(`(?i)gateway\w*port|port\w*gateway`)
	// host:port, [v6]:port or :port.
	addrRE = regexp.MustCompile(`^(?:\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]*):(\d{1,5})$`)
	// C++ in a gateway file: the lines that set its client-side address.
	cGatewayLineRE = regexp.MustCompile(`(?i)\b(listen|connect|gateway\w*)\b`)
	cIPv4PortRE    = regexp.MustCompile(`\b(?:ipv4|Address::ipv4)\s*\(\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*(\d+)\s*\)`)
	cV4PortRE      = regexp.MustCompile(`\b(?:loopbackV4|anyV4|loopbackV6|anyV6)\s*\(\s*(\d+)\s*\)`)
	cPortConstRE   = regexp.MustCompile(`\b(k\w*[Gg]ateway\w*[Pp]ort|k\w*[Pp]ort\w*[Gg]ateway\w*)\s*[=({]\s*(\d+)`)
	tomlTableRE    = regexp.MustCompile(`^\s*\[+\s*([^\]]+?)\s*\]+`)
	tomlKeyRE      = regexp.MustCompile(`^\s*([A-Za-z0-9_.-]+)\s*=\s*(.*)$`)
	yamlUDPRE      = regexp.MustCompile(`(?:^|[\s"'\[,-])(?:[0-9.]+:)?(?:\d+:)?(\d+)/udp\b`)
	testCodeRE     = regexp.MustCompile(`_test\.go$|(^|/)(tests?|fuzz|testdata)/`)
	tomlStringRE   = regexp.MustCompile(`"((?:[^"\\]|\\.)*)"|'([^']*)'`)
)

func portOf(addr string) (int, bool) {
	m := addrRE.FindStringSubmatch(addr)
	if m == nil {
		return 0, false
	}
	p, err := strconv.Atoi(m[1])
	return p, err == nil && p > 0 && p < 65536
}

func checkGatewayPort(p *Pass) {
	for _, f := range p.Files {
		if testCodeRE.MatchString(f) {
			continue // tests choose their own ports; the rule is about defaults
		}
		switch {
		case strings.HasSuffix(f, ".go"):
			checkGatewayPortGo(p, f)
		case strings.HasSuffix(f, ".toml"):
			table := ""
			for i, l := range p.Tree.Lines(f) {
				l = strings.SplitN(l, " #", 2)[0]
				if m := tomlTableRE.FindStringSubmatch(l); m != nil {
					table = m[1]
					continue
				}
				m := tomlKeyRE.FindStringSubmatch(l)
				if m == nil || !(gatewayNameRE.MatchString(m[1]) || gatewayNameRE.MatchString(table)) {
					continue
				}
				for _, s := range tomlStringRE.FindAllStringSubmatch(m[2], -1) {
					if port, ok := portOf(s[1] + s[2]); ok && port != gatewayPort {
						p.Report(f, i+1, "gateway address %q: the default gateway port is UDP 7777 (04 §2)", s[1]+s[2])
					}
				}
				if n, err := strconv.Atoi(strings.TrimSpace(m[2])); err == nil && strings.EqualFold(m[1], "port") && n != gatewayPort {
					p.Report(f, i+1, "gateway port %d: the default gateway port is UDP 7777 (04 §2)", n)
				}
			}
		case strings.HasSuffix(f, ".yml") || strings.HasSuffix(f, ".yaml"):
			// Compose and Helm files: a UDP game port is the gateway's (voice rides on it too, ADR-015).
			for i, l := range p.Tree.Lines(f) {
				l = strings.SplitN(l, " #", 2)[0]
				for _, m := range yamlUDPRE.FindAllStringSubmatch(l, -1) {
					if n, _ := strconv.Atoi(m[1]); n != gatewayPort {
						p.Report(f, i+1, "UDP port %d published: the gateway's is UDP 7777, the only public game port (04 §2)", n)
					}
				}
			}
		default:
			checkGatewayPortC(p, f)
		}
	}
}

// checkGatewayPortGo reads values named for the gateway: keyed fields, var and const specs and
// assignments whose name contains "gateway", and flag or config calls with a "gateway" argument.
func checkGatewayPortGo(p *Pass, f string) {
	g := p.Tree.goIndex()
	gf := g.file(f)
	if gf.File == nil {
		p.Report(f, 0, "cannot parse: %v", gf.Err)
		return
	}
	check := func(name string, values ...ast.Expr) {
		for _, v := range values {
			ast.Inspect(v, func(n ast.Node) bool {
				e, ok := n.(ast.Expr)
				if !ok {
					return true
				}
				if _, closure := e.(*ast.FuncLit); closure {
					return false // t.Run("gateway …", func…): the body is not the value
				}
				if s, ok := g.String(gf, e); ok {
					if port, ok := portOf(s); ok && port != gatewayPort {
						p.Report(f, g.line(e.Pos()), "gateway address %q (%s): the default gateway port is UDP 7777 (04 §2)", s, name)
					}
					return false
				}
				if lit, ok := e.(*ast.BasicLit); ok && lit.Kind == token.INT && gatewayPortRE.MatchString(name) {
					if n, _ := strconv.Atoi(lit.Value); n != gatewayPort {
						p.Report(f, g.line(e.Pos()), "%s = %d: the default gateway port is UDP 7777 (04 §2)", name, n)
					}
				}
				return true
			})
		}
	}
	ast.Inspect(gf.File, func(n ast.Node) bool {
		switch x := n.(type) {
		case *ast.KeyValueExpr:
			if k, ok := x.Key.(*ast.Ident); ok && gatewayNameRE.MatchString(k.Name) {
				check(k.Name, x.Value)
			}
		case *ast.ValueSpec:
			for i, name := range x.Names {
				if gatewayNameRE.MatchString(name.Name) && i < len(x.Values) {
					check(name.Name, x.Values[i])
				}
			}
		case *ast.AssignStmt:
			for i, lhs := range x.Lhs {
				name := ""
				switch l := lhs.(type) {
				case *ast.Ident:
					name = l.Name
				case *ast.SelectorExpr:
					name = l.Sel.Name
				}
				if gatewayNameRE.MatchString(name) && i < len(x.Rhs) {
					check(name, x.Rhs[i])
				}
			}
		case *ast.CallExpr: // flag.String("gateway", "127.0.0.1:7000", …) and the like
			named := false
			for _, a := range x.Args {
				if s, ok := g.String(gf, a); ok && gatewayNameRE.MatchString(s) {
					named = true
				}
			}
			if named {
				check(calleeName(x)+"(gateway)", x.Args...)
				return false
			}
		}
		return true
	})
}

// checkGatewayPortC reads, in a file named for the gateway, the lines that set its listen or connect
// address; elsewhere, lines that name the gateway; everywhere, k…GatewayPort constants.
func checkGatewayPortC(p *Pass, f string) {
	gatewayFile := gatewayNameRE.MatchString(f)
	for i, l := range codeLines(p.Tree.Lines(f), false) {
		if m := cPortConstRE.FindStringSubmatch(l); m != nil {
			if n, _ := strconv.Atoi(m[2]); n != gatewayPort {
				p.Report(f, i+1, "%s = %d: the default gateway port is UDP 7777 (04 §2)", m[1], n)
			}
			continue
		}
		if !(gatewayFile && cGatewayLineRE.MatchString(l)) && !gatewayNameRE.MatchString(l) {
			continue
		}
		var ports []int
		for _, s := range cStringRE.FindAllStringSubmatch(l, -1) {
			if port, ok := portOf(s[1]); ok {
				ports = append(ports, port)
			}
		}
		for _, re := range []*regexp.Regexp{cIPv4PortRE, cV4PortRE} {
			for _, m := range re.FindAllStringSubmatch(l, -1) {
				if n, _ := strconv.Atoi(m[1]); n != 0 {
					ports = append(ports, n)
				}
			}
		}
		for _, port := range ports {
			if port != gatewayPort {
				p.Report(f, i+1, "gateway address with port %d: the default gateway port is UDP 7777 (04 §2)", port)
			}
		}
	}
}
