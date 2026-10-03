package conformance

import (
	"fmt"
	"go/ast"
	"go/token"
	"regexp"
	"slices"
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

// unresolvedPort is the message for a gateway port the rule reads but cannot evaluate (fail closed).
const unresolvedPort = "%s: a gateway port the lint cannot resolve; the default gateway port is UDP 7777 (04 §2). " +
	"If it is not a gateway default, say so in a conformance:allow"

var (
	gatewayNameRE = regexp.MustCompile(`(?i)gateway`)
	// host:port, [v6]:port or :port.
	addrRE = regexp.MustCompile(`^(?:\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]*):(\d{1,5})$`)
	// C++ in a gateway file: the lines that set its client-side address.
	cGatewayLineRE = regexp.MustCompile(`(?i)\b(listen|connect|gateway\w*)\b`)
	// Address::ipv4(a, b, c, d, port), and the one-argument loopbackV4(port) family.
	cIPv4CallRE = regexp.MustCompile(`\b(?:Address\s*::\s*)?ipv4\s*\(`)
	cV4CallRE   = regexp.MustCompile(`\b(?:loopbackV4|anyV4|loopbackV6|anyV6)\s*\(`)
	// A listen, connect or gateway option read with a default: args.get("connect", "", kDefault).
	cOptionCallRE = regexp.MustCompile(`(?i)\b\w+\s*\(\s*"-{0,2}(?:listen|connect|[\w.-]*gateway)[\w.-]*"\s*,`)
	// The declaration of a k… constant and its initializer, and a #define; gatewayPortName picks those
	// named for the gateway port.
	cPortDeclRE   = regexp.MustCompile(`\b(k[A-Z_]\w*)\s*(?:=\s*([^;=,{}][^;,{}]*)|\{([^{}]*)\}|\(([^()]*)\))\s*[;,]`)
	cDefinePortRE = regexp.MustCompile(`(?m)^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+([^\n]*)$`)
	cIntLitRE     = regexp.MustCompile(`^(0[xX][0-9A-Fa-f']+|\d[\d']*)[uUlL]*$`)
	cCastRE       = regexp.MustCompile(`^(?:static_cast\s*<[^<>]*>|(?:std::)?u?int(?:16|32|64)_t|u16|u32|i32|int|unsigned)\s*[({](.*)[)}]$`)
	tomlTableRE   = regexp.MustCompile(`^\s*\[+\s*([^\]]+?)\s*\]+`)
	optionNameRE  = regexp.MustCompile(`^-{0,2}[\w.-]+$`)
	tomlKeyRE     = regexp.MustCompile(`^\s*([A-Za-z0-9_.-]+)\s*=\s*(.*)$`)
	tomlIntRE     = regexp.MustCompile(`^[+]?\d[\d_]*$`)
	// A port mapping token in compose or Helm YAML that ends in /udp, and its parts: an optional host IP,
	// an optional published port (or range) and the container port (or range).
	yamlUDPTokenRE = regexp.MustCompile(`[^\s"',]+/udp\b`)
	yamlUDPRE      = regexp.MustCompile(`^(?:(?:\d{1,3}(?:\.\d{1,3}){3}|\[[0-9A-Fa-f:]+\]):)?(?:(\d+)(?:-(\d+))?:)?(\d+)(?:-(\d+))?/udp$`)
	ipv6BracketRE  = regexp.MustCompile(`^\[[0-9A-Fa-f:]+\]:`)
	testCodeRE     = regexp.MustCompile(`_test\.go$|(^|/)(tests?|fuzz|testdata)/`)
	tomlStringRE   = regexp.MustCompile(`"((?:[^"\\]|\\.)*)"|'([^']*)'`)
	// A Go address format: something, a colon, then a verb ("127.0.0.1:%d", "%s:%d", ":%d").
	goAddrFormatRE = regexp.MustCompile(`:%[-+# 0-9]*[dsvq]$`)
)

// nameWords splits a name into lower-case words at case changes and non-alphanumerics
// (kDefaultGatewayPort, HELIOS_GATEWAY_PORT and "gateway-port" are all … gateway port).
func nameWords(name string) []string {
	return strings.FieldsFunc(strings.ToLower(camelRE.ReplaceAllString(name, "${1}_${2}")), func(r rune) bool {
		return !(r >= 'a' && r <= 'z' || r >= '0' && r <= '9')
	})
}

// portName reports a name with the word port (listen_port, ports), not one that only contains it
// (transport, report_interval); gatewayPortName one with the words gateway and port.
func portName(name string) bool {
	return slices.ContainsFunc(nameWords(name), func(w string) bool { return w == "port" || w == "ports" })
}

func gatewayPortName(name string) bool {
	return portName(name) && slices.ContainsFunc(nameWords(name), func(w string) bool {
		return w == "gateway" || w == "gateways"
	})
}

func portOf(addr string) (int, bool) {
	m := addrRE.FindStringSubmatch(addr)
	if m == nil {
		return 0, false
	}
	p, err := strconv.Atoi(m[1])
	return p, err == nil && p > 0 && p < 65536
}

func checkGatewayPort(p *Pass) {
	var ints map[string][]string // the scope's C and C++ integer constants (cConstTable), read once on first use
	for _, f := range p.Files {
		if testCodeRE.MatchString(f) {
			continue // tests choose their own ports; the rule is about defaults
		}
		switch {
		case strings.HasSuffix(f, ".go"):
			checkGatewayPortGo(p, f)
		case strings.HasSuffix(f, ".toml"):
			checkGatewayPortTOML(p, f)
		case strings.HasSuffix(f, ".yml") || strings.HasSuffix(f, ".yaml"):
			// Compose and Helm files: a UDP game port is the gateway's (voice rides on it too, ADR-015).
			for i, l := range p.Tree.Lines(f) {
				if strings.HasPrefix(strings.TrimSpace(l), "#") {
					continue
				}
				l = strings.SplitN(l, " #", 2)[0]
				for _, tok := range yamlUDPTokenRE.FindAllString(l, -1) {
					if strings.HasPrefix(tok, "[") && !ipv6BracketRE.MatchString(tok) {
						tok = tok[1:] // a flow sequence: [7000:7777/udp]
					}
					m := yamlUDPRE.FindStringSubmatch(tok)
					if m == nil {
						p.Report(f, i+1, unresolvedPort, "UDP port mapping "+strconv.Quote(tok))
						continue
					}
					pub, ctr := m[1], m[3]
					if pub == "" {
						pub = ctr // "7000/udp" publishes the container port
					}
					if pub != strconv.Itoa(gatewayPort) || ctr != strconv.Itoa(gatewayPort) || m[2] != "" || m[4] != "" {
						p.Report(f, i+1, "UDP port mapping %q publishes %s (container port %s): the gateway's is UDP 7777, "+
							"the only public game port (04 §2)", tok, pub, ctr)
					}
				}
			}
		default:
			if !gatewayNameRE.MatchString(f) && !gatewayNameRE.MatchString(p.Tree.Text(f)) {
				continue // every form below names the gateway, in the file's name or its text
			}
			if ints == nil {
				ints = cConstTable(p)
			}
			checkGatewayPortC(p, f, ints)
		}
	}
}

// tomlCode is a TOML line without its comment: the first '#' outside a string.
func tomlCode(l string) string {
	quote := byte(0)
	for i := 0; i < len(l); i++ {
		switch c := l[i]; {
		case quote == '"' && c == '\\':
			i++
		case quote != 0 && c == quote:
			quote = 0
		case quote == 0 && (c == '"' || c == '\''):
			quote = c
		case quote == 0 && c == '#':
			return l[:i]
		}
	}
	return l
}

// checkGatewayPortTOML reads the keys and tables named for the gateway: address strings anywhere in
// them, and the integer (or array, or quoted integer) values of keys named for a port, which must be
// 7777. A port value the lint cannot read fails closed.
func checkGatewayPortTOML(p *Pass, f string) {
	table := ""
	for i, l := range p.Tree.Lines(f) {
		l = tomlCode(l)
		if m := tomlTableRE.FindStringSubmatch(l); m != nil {
			table = m[1]
			continue
		}
		m := tomlKeyRE.FindStringSubmatch(l)
		if m == nil || !(gatewayNameRE.MatchString(m[1]) || gatewayNameRE.MatchString(table)) {
			continue
		}
		val := strings.TrimSpace(m[2])
		for _, s := range tomlStringRE.FindAllStringSubmatch(val, -1) {
			if port, ok := portOf(s[1] + s[2]); ok && port != gatewayPort {
				p.Report(f, i+1, "gateway address %q: the default gateway port is UDP 7777 (04 §2)", s[1]+s[2])
			}
		}
		if !portName(m[1]) {
			continue
		}
		items := []string{val}
		if strings.HasPrefix(val, "[") && strings.HasSuffix(val, "]") {
			items = strings.Split(strings.Trim(val, "[]"), ",")
		}
		for _, it := range items {
			it = strings.TrimSpace(it)
			if s := tomlStringRE.FindStringSubmatch(it); s != nil && s[0] == it {
				if _, ok := portOf(s[1] + s[2]); ok {
					continue // an address, checked above
				}
				it = s[1] + s[2]
			}
			if it == "" {
				continue
			}
			if !tomlIntRE.MatchString(it) {
				p.Report(f, i+1, unresolvedPort, "TOML "+m[1]+" = "+it)
				continue
			}
			if n, _ := strconv.Atoi(strings.ReplaceAll(strings.TrimPrefix(it, "+"), "_", "")); n != gatewayPort {
				p.Report(f, i+1, "gateway port %d: the default gateway port is UDP 7777 (04 §2)", n)
			}
		}
	}
}

// checkGatewayPortGo reads values named for the gateway: keyed fields, var and const specs and
// assignments whose name contains "gateway", and flag or config calls with a "gateway" argument.
// Inside them it evaluates address strings, net.JoinHostPort and fmt.Sprintf addresses, and, for a
// name that says gateway port, the integer; any of these it cannot evaluate fails closed.
func checkGatewayPortGo(p *Pass, f string) {
	g := p.Tree.goIndex()
	gf := g.file(f)
	if gf.File == nil {
		p.Report(f, 0, "cannot parse: %v", gf.Err)
		return
	}
	addr := func(e ast.Expr, name, s string) {
		if port, ok := portOf(s); ok && port != gatewayPort {
			p.Report(f, g.line(e.Pos()), "gateway address %q (%s): the default gateway port is UDP 7777 (04 §2)", s, name)
		}
	}
	// goValue evaluates a string or integer argument of an address builder.
	goValue := func(e ast.Expr) (string, bool) {
		if s, ok := g.String(gf, e); ok {
			return s, true
		}
		if n, ok := g.Int(gf, e); ok {
			return strconv.FormatInt(n, 10), true
		}
		return "", false
	}
	check := func(name string, values ...ast.Expr) {
		portName := gatewayPortName(name)
		for _, v := range values {
			if portName {
				// A value named for the gateway port is a port: evaluate it whole, or look into the call
				// that produces it (flag.Int("gateway-port", 7777, …)); nothing evaluable fails closed.
				if n, ok := g.Int(gf, v); ok {
					if n != gatewayPort {
						p.Report(f, g.line(v.Pos()), "%s = %d: the default gateway port is UDP 7777 (04 §2)", name, n)
					}
					continue
				}
				if _, ok := g.String(gf, v); !ok {
					if c, ok := v.(*ast.CallExpr); !ok || !slices.ContainsFunc(c.Args, func(a ast.Expr) bool {
						_, ok := g.Int(gf, a)
						return ok
					}) {
						p.Report(f, g.line(v.Pos()), unresolvedPort, name)
						continue
					}
				}
			}
			ast.Inspect(v, func(n ast.Node) bool {
				e, ok := n.(ast.Expr)
				if !ok {
					return true
				}
				if _, closure := e.(*ast.FuncLit); closure {
					return false // t.Run("gateway …", func…): the body is not the value
				}
				if s, ok := g.String(gf, e); ok {
					addr(e, name, s)
					return false
				}
				if c, ok := e.(*ast.CallExpr); ok {
					switch fn := calleeName(c); {
					case fn == "JoinHostPort" && len(c.Args) == 2:
						host, ok1 := g.String(gf, c.Args[0])
						port, ok2 := goValue(c.Args[1])
						if !ok1 || !ok2 {
							p.Report(f, g.line(c.Pos()), unresolvedPort, "net.JoinHostPort in "+name)
						} else {
							addr(c, name, host+":"+port)
						}
						return false
					case fn == "Sprintf" && len(c.Args) >= 1:
						format, ok := g.String(gf, c.Args[0])
						if !ok || !goAddrFormatRE.MatchString(format) {
							break
						}
						var args []any
						for _, a := range c.Args[1:] {
							v, ok := goValue(a)
							if !ok {
								p.Report(f, g.line(c.Pos()), unresolvedPort, "fmt.Sprintf address in "+name)
								return false
							}
							args = append(args, v)
						}
						addr(c, name, fmt.Sprintf(strings.NewReplacer("%d", "%s", "%v", "%s", "%q", "%s").Replace(format), args...))
						return false
					}
				}
				if lit, ok := e.(*ast.BasicLit); ok && lit.Kind == token.INT && portName {
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
			// Any string that mentions the gateway makes the call's addresses gateway addresses; only an
			// option name ("gateway-port", not a sentence) can say that an integer is the gateway port.
			named, option := false, "gateway"
			for _, a := range x.Args {
				if s, ok := g.String(gf, a); ok && gatewayNameRE.MatchString(s) {
					named = true
					if optionNameRE.MatchString(s) && option == "gateway" {
						option = s
					}
				}
			}
			if named {
				check(calleeName(x)+"("+option+")", x)
				return false
			}
		}
		return true
	})
}

// cPortValues evaluates a C or C++ port expression to every value it can take: integer literals
// (digit separators and suffixes allowed), names from the table (qualifiers dropped; every definition
// must evaluate), casts, parentheses and +. Nil means the lint cannot resolve it.
func cPortValues(expr string, t map[string][]string, depth int) []int64 {
	expr = strings.TrimSpace(expr)
	for strings.HasPrefix(expr, "(") && matchingParen(expr) == len(expr)-1 {
		expr = strings.TrimSpace(expr[1 : len(expr)-1])
	}
	if m := cCastRE.FindStringSubmatch(expr); m != nil {
		return cPortValues(m[1], t, depth+1)
	}
	if depth > 8 || expr == "" {
		return nil
	}
	if parts := splitTopLevel(expr, '+'); len(parts) > 1 {
		sums := []int64{0}
		for _, part := range parts {
			vs := cPortValues(part, t, depth+1)
			if vs == nil {
				return nil
			}
			var next []int64
			for _, s := range sums {
				for _, v := range vs {
					if !slices.Contains(next, s+v) {
						next = append(next, s+v)
					}
				}
			}
			sums = next
		}
		return sums
	}
	if m := cIntLitRE.FindStringSubmatch(expr); m != nil {
		v, err := strconv.ParseInt(strings.ReplaceAll(m[1], "'", ""), 0, 64)
		if err != nil {
			return nil
		}
		return []int64{v}
	}
	m := cQualIdentRE.FindStringSubmatch(expr)
	if m == nil || len(t[m[1]]) == 0 {
		return nil
	}
	var out []int64
	for _, def := range t[m[1]] {
		vs := cPortValues(def, t, depth+1)
		if vs == nil {
			return nil
		}
		for _, v := range vs {
			if !slices.Contains(out, v) {
				out = append(out, v)
			}
		}
	}
	return out
}

// checkGatewayPortC reads constants named for the gateway port everywhere; and, in a file named for the
// gateway, the lines that set its listen or connect address (elsewhere, lines that name the gateway):
// address literals, the port of Address::ipv4(…) and loopbackV4(…), and the default of a listen,
// connect or gateway option. Names resolve through the scope's constants; what does not resolve fails
// closed.
func checkGatewayPortC(p *Pass, f string, ints map[string][]string) {
	src := newCSource(p.Tree.Lines(f))
	gatewayFile := gatewayNameRE.MatchString(f)
	report := func(line int, port int64) {
		if port != gatewayPort {
			p.Report(f, line, "gateway address with port %d: the default gateway port is UDP 7777 (04 §2)", port)
		}
	}
	for _, m := range cPortDeclRE.FindAllStringSubmatchIndex(src.blank, -1) {
		name := src.text[m[2]:m[3]]
		if !gatewayPortName(name) {
			continue
		}
		init := ""
		for g := 4; g <= 8; g += 2 {
			if m[g] >= 0 {
				init = src.text[m[g]:m[g+1]]
			}
		}
		vs := cPortValues(init, ints, 0)
		if vs == nil {
			p.Report(f, src.line(m[0]), unresolvedPort, name+" = "+strings.TrimSpace(init))
		}
		for _, v := range vs {
			if v != gatewayPort {
				p.Report(f, src.line(m[0]), "%s = %d: the default gateway port is UDP 7777 (04 §2)", name, v)
			}
		}
	}
	for _, m := range cDefinePortRE.FindAllStringSubmatchIndex(src.text, -1) {
		name, init := src.text[m[2]:m[3]], src.text[m[4]:m[5]]
		if !gatewayPortName(name) {
			continue
		}
		vs := cPortValues(init, ints, 0)
		if vs == nil {
			p.Report(f, src.line(m[0]), unresolvedPort, name+" "+strings.TrimSpace(init))
		}
		for _, v := range vs {
			if v != gatewayPort {
				p.Report(f, src.line(m[0]), "%s = %d: the default gateway port is UDP 7777 (04 §2)", name, v)
			}
		}
	}
	reads := func(i int) bool {
		l := src.logical[i]
		return gatewayFile && cGatewayLineRE.MatchString(l) || gatewayNameRE.MatchString(l)
	}
	for i, l := range src.logical {
		if !reads(i) || slices.ContainsFunc(cPortDeclRE.FindAllStringSubmatch(src.blankLines[i], -1),
			func(m []string) bool { return gatewayPortName(m[1]) }) {
			continue
		}
		for _, s := range cStringRE.FindAllStringSubmatch(l, -1) {
			if port, ok := portOf(s[1]); ok {
				report(src.origin[i]+1, int64(port))
			}
		}
	}
	// Calls on those lines, with their arguments read across lines: the port argument of ipv4(…) and
	// loopbackV4(…), and the default (last argument) of a listen, connect or gateway option.
	for _, c := range src.callsAt(cIPv4CallRE.FindAllStringIndex(src.blank, -1)) {
		if reads(c.index) && len(c.args) == 5 {
			portArg(p, f, c, c.args[4], ints)
		}
	}
	for _, c := range src.callsAt(cV4CallRE.FindAllStringIndex(src.blank, -1)) {
		if reads(c.index) && len(c.args) == 1 {
			portArg(p, f, c, c.args[0], ints)
		}
	}
	strs := cStrTable(p)
	for _, c := range src.callsAt(cOptionCallRE.FindAllStringIndex(src.text, -1)) {
		if !reads(c.index) || len(c.args) < 2 {
			continue
		}
		def := c.args[len(c.args)-1]
		vals, ok := cStrValues(def, strs)
		if !ok {
			// An integer default is the port itself (args.getInt("listen-port", 0, kPort)).
			if vs := cPortValues(def, ints, 0); vs != nil {
				for _, v := range vs {
					report(c.line, v)
				}
				continue
			}
			p.Report(f, c.line, unresolvedPort, "default "+strings.TrimSpace(def)+" of "+c.args[0])
			continue
		}
		for _, v := range vals {
			if port, ok := portOf(v); ok {
				report(c.line, int64(port))
			}
		}
	}
}

// portArg checks the port argument of an address call.
func portArg(p *Pass, f string, c cCall, arg string, ints map[string][]string) {
	vs := cPortValues(arg, ints, 0)
	if vs == nil {
		p.Report(f, c.line, unresolvedPort, "port "+strings.TrimSpace(arg)+" of "+c.name)
	}
	for _, v := range vs {
		if v != gatewayPort {
			p.Report(f, c.line, "gateway address with port %d: the default gateway port is UDP 7777 (04 §2)", v)
		}
	}
}
