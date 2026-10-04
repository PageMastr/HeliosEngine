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
	// Address::ipv4(a, b, c, d, port), ipv4(octets, port), ipv6(groups, port) and ipv6Bytes(bytes, port): the
	// port is the last argument. And the one-argument loopbackV4(port) family.
	cIPv4CallRE = regexp.MustCompile(`\b(?:Address\s*::\s*)?(?:ipv4|ipv6|ipv6Bytes)\s*\(`)
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
	tomlKeyRE     = regexp.MustCompile(`^\s*("[^"]*"|'[^']*'|[A-Za-z0-9_.-]+)\s*=\s*(.*)$`) // bare or quoted key
	tomlIntRE     = regexp.MustCompile(`^[+]?\d[\d_]*$`)
	// A port mapping token in compose or Helm YAML that ends in /udp, and its parts: an optional host IP,
	// an optional published port (or range) and the container port (or range).
	// The protocol in any case: Docker lower-cases it.
	yamlUDPTokenRE = regexp.MustCompile(`(?i)[^\s"',]+/udp\b`)
	yamlUDPRE      = regexp.MustCompile(`(?i)^(?:(?:\d{1,3}(?:\.\d{1,3}){3}|\[[0-9A-Fa-f:]+\]):)?(?:(\d+)(?:-(\d+))?:)?(\d+)(?:-(\d+))?/udp$`)
	ipv6BracketRE  = regexp.MustCompile(`^\[[0-9A-Fa-f:]+\]:`)
	testCodeRE     = regexp.MustCompile(`_test\.go$|(^|/)(tests?|fuzz|testdata)/`)
	tomlStringRE   = regexp.MustCompile(`"((?:[^"\\]|\\.)*)"|'([^']*)'`)
	// A gateway option on a command line held in a TOML array (`args = ["--listen", "127.0.0.1:7777"]`):
	// --listen, --connect or one named for the gateway, with its value after '=' or in the next element.
	tomlOptRE = regexp.MustCompile(`(?i)^(-{1,2}(?:listen|connect|[\w.-]*gateway[\w.-]*))(?:=(.*))?$`)
	// A Go address format: something, a colon, then a verb ("127.0.0.1:%d", "%s:%d", ":%d").
	goAddrFormatRE = regexp.MustCompile(`:%[-+# 0-9]*[dsvq]$`)
	// An address without its port, which code completes by concatenation: "127.0.0.1:", "[::1]:", ":".
	addrPrefixRE = regexp.MustCompile(`^(?:\[[0-9A-Fa-f:.]*\]|[A-Za-z0-9.-]*):$`)
	// C++: what follows such a string when it is concatenated (`"host:" + p`, `std::string("host:") + p`,
	// `std::string{"host:"} + p`, `os << "host:" << p`), and the port operand's std::to_string wrapper.
	cConcatRE   = regexp.MustCompile(`^[\s)}]*(?:\+|<<)\s*`)
	cToStringRE = regexp.MustCompile(`^(?:std\s*::\s*)?to_string\s*\(`)
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
// 7777; an inline table's keys are read the same way. A value whose brackets open across lines (a
// multi-line array) is joined to where they close and reported on its key's line. A port value the lint
// cannot read fails closed.
func checkGatewayPortTOML(p *Pass, f string) {
	table := ""
	lines := p.Tree.Lines(f)
	for i := 0; i < len(lines); i++ {
		l := tomlCode(lines[i])
		if m := tomlTableRE.FindStringSubmatch(l); m != nil {
			table = m[1]
			continue
		}
		m := tomlKeyRE.FindStringSubmatch(l)
		if m == nil {
			continue
		}
		line, val := i+1, strings.TrimSpace(m[2])
		// Joined for every key, so that an element line is never read as a key or a table header.
		for tomlOpen(val) > 0 && i+1 < len(lines) {
			i++
			val += " " + strings.TrimSpace(tomlCode(lines[i]))
		}
		if gatewayNameRE.MatchString(m[1]) || gatewayNameRE.MatchString(table) {
			tomlGatewayValue(p, f, line, strings.Trim(m[1], `"'`), val, 0)
		} else if strings.HasPrefix(val, "[") {
			tomlCommandLine(p, f, line, val)
		}
	}
}

// tomlCommandLine checks a command line held in a TOML array outside a gateway context, such as a
// supervised process's `[[orchestrator.spawn]] args = ["--name", "gw-1", "--listen", "127.0.0.1:7777"]`
// (engine/server/README.md): only the gateway takes --listen and --connect (apps/gateway), so their value
// is its listen or connect address. An option named for a port may take a bare port.
func tomlCommandLine(p *Pass, f string, line int, val string) {
	elems := tomlStringRE.FindAllStringSubmatch(val, -1)
	for k, e := range elems {
		o := tomlOptRE.FindStringSubmatch(e[1] + e[2])
		if o == nil {
			continue
		}
		v := o[2]
		if !strings.Contains(e[1]+e[2], "=") {
			if k+1 == len(elems) {
				continue
			}
			v = elems[k+1][1] + elems[k+1][2]
		}
		if port, ok := portOf(v); ok && port != gatewayPort {
			p.Report(f, line, "gateway address %q (%s on a command line): the default gateway port is UDP 7777 "+
				"(04 §2)", v, o[1])
		} else if !ok && portName(o[1]) && tomlIntRE.MatchString(v) {
			if n, _ := strconv.Atoi(strings.ReplaceAll(strings.TrimPrefix(v, "+"), "_", "")); n != gatewayPort {
				p.Report(f, line, "gateway port %d (%s on a command line): the default gateway port is UDP 7777 "+
					"(04 §2)", n, o[1])
			}
		}
	}
}

// tomlOpen returns how many arrays and inline tables a TOML value leaves open: brackets and braces outside
// strings.
func tomlOpen(s string) int {
	depth, quote := 0, byte(0)
	for i := 0; i < len(s); i++ {
		switch c := s[i]; {
		case quote == '"' && c == '\\':
			i++
		case quote != 0:
			if c == quote {
				quote = 0
			}
		case c == '"' || c == '\'':
			quote = c
		case c == '[' || c == '{':
			depth++
		case c == ']' || c == '}':
			depth--
		}
	}
	return depth
}

// tomlGatewayValue checks one value of a key in a gateway context: address strings anywhere in it, the
// port of a key named for a port, and the keys of an inline table (`listen = { host = "…", port = 7000 }`).
func tomlGatewayValue(p *Pass, f string, line int, key, val string, depth int) {
	for _, s := range tomlStringRE.FindAllStringSubmatch(val, -1) {
		if port, ok := portOf(s[1] + s[2]); ok && port != gatewayPort && depth == 0 {
			p.Report(f, line, "gateway address %q: the default gateway port is UDP 7777 (04 §2)", s[1]+s[2])
		}
	}
	if !portName(key) {
		if strings.HasPrefix(val, "{") && strings.HasSuffix(val, "}") && depth < 8 {
			for _, kv := range tomlSplit(val[1 : len(val)-1]) {
				if m := tomlKeyRE.FindStringSubmatch(kv); m != nil {
					tomlGatewayValue(p, f, line, strings.Trim(m[1], `"'`), strings.TrimSpace(m[2]), depth+1)
				}
			}
		}
		return
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
			p.Report(f, line, unresolvedPort, "TOML "+key+" = "+it)
			continue
		}
		if n, _ := strconv.Atoi(strings.ReplaceAll(strings.TrimPrefix(it, "+"), "_", "")); n != gatewayPort {
			p.Report(f, line, "gateway port %d: the default gateway port is UDP 7777 (04 §2)", n)
		}
	}
}

// tomlSplit splits an inline table's body at the commas outside strings, arrays and inner tables.
func tomlSplit(s string) []string {
	var out []string
	depth, quote, from := 0, byte(0), 0
	for i := 0; i < len(s); i++ {
		switch c := s[i]; {
		case quote == '"' && c == '\\':
			i++
		case quote != 0:
			if c == quote {
				quote = 0
			}
		case c == '"' || c == '\'':
			quote = c
		case c == '[' || c == '{':
			depth++
		case c == ']' || c == '}':
			depth--
		case c == ',' && depth == 0:
			out = append(out, s[from:i])
			from = i + 1
		}
	}
	return append(out, s[from:])
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
	// goPort evaluates the port operand of an address built by concatenation or fmt.Sprint, looking
	// through strconv.Itoa, strconv.FormatInt/FormatUint and fmt.Sprint.
	goPort := func(e ast.Expr) (string, bool) {
		if c, ok := e.(*ast.CallExpr); ok {
			switch fn := calleeName(c); {
			case (fn == "Itoa" || fn == "Sprint") && len(c.Args) == 1, (fn == "FormatInt" || fn == "FormatUint") && len(c.Args) == 2:
				e = c.Args[0]
			}
		}
		return goValue(e)
	}
	// The literals a check has read, so that a gateway-typed literal under a gateway-named value is read once.
	checked := map[*ast.CompositeLit]bool{}
	var check func(name string, values ...ast.Expr)
	check = func(name string, values ...ast.Expr) {
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
				if cl, ok := e.(*ast.CompositeLit); ok {
					checked[cl] = true
				}
				// A Port field inside a gateway value is the gateway port (&net.UDPAddr{Port: 7777}).
				if kv, ok := e.(*ast.KeyValueExpr); ok {
					if k, ok := kv.Key.(*ast.Ident); ok && k.Name == "Port" {
						check(name+".Port", kv.Value)
						return false
					}
				}
				if s, ok := g.String(gf, e); ok {
					addr(e, name, s)
					return false
				}
				// "host:" + port: the port operand evaluated, or the address fails closed. With a host that
				// does not evaluate, `host + ":" + port` parses as (host + ":") + port: the prefix is then the
				// left operand's last string.
				if b, ok := e.(*ast.BinaryExpr); ok && b.Op == token.ADD {
					host, ok := g.String(gf, b.X)
					if bx, add := b.X.(*ast.BinaryExpr); !ok && add && bx.Op == token.ADD {
						host, ok = g.String(gf, bx.Y)
					}
					if ok && addrPrefixRE.MatchString(host) {
						if port, ok := goPort(b.Y); ok {
							addr(b, name, host+port)
						} else {
							p.Report(f, g.line(b.Pos()), unresolvedPort, "address "+strconv.Quote(host)+" + port in "+name)
						}
						return false
					}
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
					case fn == "Sprint" && len(c.Args) == 2: // fmt.Sprint("127.0.0.1:", 7777)
						host, ok := g.String(gf, c.Args[0])
						if !ok || !addrPrefixRE.MatchString(host) {
							break
						}
						if port, ok := goPort(c.Args[1]); ok {
							addr(c, name, host+port)
						} else {
							p.Report(f, g.line(c.Pos()), unresolvedPort, "fmt.Sprint address in "+name)
						}
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
		case *ast.CompositeLit:
			// A literal of a gateway-named type is a gateway value under any name: its Port field and its
			// addresses (GatewayConfig{Port: 7003, Addr: ":7000"}), also as an elided element of a slice,
			// array or map literal ([]GatewayConfig{{Port: 7000}}).
			if checked[x] {
				return true
			}
			if t := typeName(x.Type); gatewayNameRE.MatchString(t) {
				check(t, x)
			} else if el := configElem(x.Type); el != nil && gatewayNameRE.MatchString(typeName(el)) {
				for _, e := range elementLits(x) {
					if e.Type == nil {
						check(typeName(el), e)
					}
				}
			}
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
	// A value is read when its statement starts on, or reaches, a read line: its own line, or the earlier
	// lines of an initializer split across lines (`net::Address listen =` then `Address::ipv4(…);`). A
	// statement starts after `;`, `{` or `}`, or after a preprocessor line. carry[i] says whether the
	// statement still open where line i starts has reached a read line (one pass, so no line is rescanned).
	carry := make([]bool, len(src.logical))
	open := false
	for i, bl := range src.blankLines {
		carry[i] = open
		if strings.HasPrefix(strings.TrimSpace(bl), "#") {
			open = false
			continue
		}
		from := strings.LastIndexAny(bl, ";{}") + 1
		part := src.logical[i][from:]
		reached := gatewayFile && cGatewayLineRE.MatchString(part) || gatewayNameRE.MatchString(part)
		open = reached || open && from == 0
	}
	stmtReads := func(off int) bool {
		i := src.index(off)
		return reads(i) || carry[i] && !strings.ContainsAny(src.blank[src.starts[i]:off], ";{}")
	}
	// The calls a regexp matches, with their offsets.
	calls := func(re *regexp.Regexp, text string) ([]cCall, [][]int) {
		locs := re.FindAllStringIndex(text, -1)
		return src.callsAt(locs), locs // every match ends in '(', so callsAt keeps them all, in order
	}
	// The option calls' argument spans: a default on a continuation line is the call's, reported on its line.
	options, optionLocs := calls(cOptionCallRE, src.text)
	var spans [][2]int
	for _, m := range optionLocs {
		open := m[0] + strings.IndexByte(src.blank[m[0]:m[1]], '(') + 1
		spans = append(spans, [2]int{open, closingParen(src.blank, open)})
	}
	inOption := func(off int) bool {
		return slices.ContainsFunc(spans, func(s [2]int) bool { return s[0] <= off && off < s[1] })
	}
	// String literals on the read lines, and on the continuation lines of a statement that a read line
	// starts (`std::string listen =` then `"127.0.0.1:7000";`).
	for i, l := range src.logical {
		read := reads(i)
		if !read && !carry[i] || slices.ContainsFunc(cPortDeclRE.FindAllStringSubmatch(src.blankLines[i], -1),
			func(m []string) bool { return gatewayPortName(m[1]) }) {
			continue
		}
		for _, s := range cStringRE.FindAllStringSubmatchIndex(l, -1) {
			if off := src.starts[i] + s[0]; !read && (!stmtReads(off) || inOption(off)) {
				continue
			}
			lit := l[s[2]:s[3]]
			if port, ok := portOf(lit); ok {
				report(src.origin[i]+1, int64(port))
			} else if c := cConcatRE.FindString(l[s[1]:]); c != "" && addrPrefixRE.MatchString(lit) {
				// "host:" + port: evaluate the port operand, or fail closed.
				vs := cPortValues(cPortOperand(l[s[1]+len(c):]), ints, 0)
				if vs == nil {
					p.Report(f, src.origin[i]+1, unresolvedPort, "address "+strconv.Quote(lit)+" + port")
				}
				for _, v := range vs {
					report(src.origin[i]+1, v)
				}
			}
		}
	}
	// Calls on those lines, with their arguments read across lines: the port argument of ipv4(…) and
	// loopbackV4(…), and the default (last argument) of a listen, connect or gateway option.
	ipv4s, locs := calls(cIPv4CallRE, src.blank)
	for k, c := range ipv4s {
		if stmtReads(locs[k][0]) && len(c.args) >= 2 {
			portArg(p, f, c, c.args[len(c.args)-1], ints)
		}
	}
	v4s, locs := calls(cV4CallRE, src.blank)
	for k, c := range v4s {
		if stmtReads(locs[k][0]) && len(c.args) == 1 {
			portArg(p, f, c, c.args[0], ints)
		}
	}
	strs := cStrTable(p)
	for k, c := range options {
		if !stmtReads(optionLocs[k][0]) || len(c.args) < 2 {
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

// cPortOperand is the port operand at the start of s, after `"host:" +` or `<<`: the argument of
// std::to_string(…), or the expression up to the first top-level `)`, `,`, `;` or `<<`. A `+` is part of
// it: `os << "host:" << kBase + 1` writes kBase + 1, since + binds tighter than <<. (After a `+`
// concatenation the operand is a string, which cPortValues cannot evaluate either way.)
func cPortOperand(s string) string {
	s = strings.TrimSpace(s)
	if m := cToStringRE.FindString(s); m != "" {
		s = s[len(m)-1:]
		if end := matchingParen(s); end > 0 {
			return s[1:end]
		}
		return ""
	}
	depth := 0
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case '(', '[', '{':
			depth++
		case ')', ']', '}':
			if depth == 0 {
				return s[:i]
			}
			depth--
		case ',', ';':
			if depth == 0 {
				return s[:i]
			}
		case '<':
			if depth == 0 && strings.HasPrefix(s[i:], "<<") {
				return s[:i]
			}
		}
	}
	return s
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
