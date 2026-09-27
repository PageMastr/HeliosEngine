package conformance

import (
	"go/ast"
	"path"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// The schema rules (05 §1.4, §3, §6.6) read the goose migrations under services/migrations/<service>/
// and evaluate the *net* schema: each service's `-- +goose Up` sections applied in version order, with
// the legacy schema renames that services/migrations/migrations.go declares (WP-0.15r's `Schemas`
// entries: Name, Dir, Legacy, LegacyVersion). Files already applied keep their statements (05 §3.3),
// so a legacy file's `identity.account` is judged as `svc_identity.account`, and a column a later
// migration drops is judged as gone (09 §5.10.4 (a)).
var _ = register(&Rule{
	ID:     "CONF-06",
	Anchor: "05 §1.4, §3",
	Title:  "A PostgreSQL schema not named svc_<service>, or a table created outside its own service's schema",
	Scope:  []string{"services/migrations/**"},
	Types:  regexp.MustCompile(`\.(sql|go)$`),
	Check:  func(p *Pass) { evalSchemas(p).reportSchemas(p) },
})

var _ = register(&Rule{
	ID:     "CONF-07",
	Anchor: "05 §3, §6.6 (Phase 0 rule)",
	Title:  "A direct-PII column that is not *_ct or *_bidx, or lives outside svc_identity; an @pii field outside Identity",
	Scope:  []string{"services/migrations/**", "schemas/**"},
	Types:  regexp.MustCompile(`\.(sql|go|hschema)$`),
	Check: func(p *Pass) {
		evalSchemas(p).reportPII(p)
		checkPIIAttributes(p)
	},
})

var (
	svcSchemaRE = regexp.MustCompile(`^svc_[a-z][a-z0-9_]*$`)
	// Direct PII (05 §6.6): e-mail, date of birth, IP address, real name. Matched on the column name's
	// words; *_ct ciphertext and *_bidx blind indexes are the allowed forms.
	piiWordRE      = regexp.MustCompile(`(^|_)(e_?mail|email_norm|dob|date_of_birth|birth_?date|birthday|ip|ip_?addr(ess)?|ipv[46]|remote_addr|real_name|full_name|first_name|last_name|legal_name|given_name|family_name|surname)(_|$)`)
	piiSafeRE      = regexp.MustCompile(`_(ct|bidx)$`)
	piiTypesRE     = regexp.MustCompile(`(?i)^(inet|cidr)\b`)
	identRE        = `("[^"]+"|[A-Za-z_][A-Za-z0-9_$]*)`
	createSchemaRE = regexp.MustCompile(`(?is)^create\s+schema\s+(?:if\s+not\s+exists\s+)?` + identRE)
	renameSchemaRE = regexp.MustCompile(`(?is)^alter\s+schema\s+` + identRE + `\s+rename\s+to\s+` + identRE)
	createTableRE  = regexp.MustCompile(`(?is)^create\s+(?:(?:global\s+|local\s+)?(?:temp|temporary|unlogged)\s+)?table\s+(?:if\s+not\s+exists\s+)?` + identRE + `(?:\s*\.\s*` + identRE + `)?\s*\(`)
	alterTableRE   = regexp.MustCompile(`(?is)^alter\s+table\s+(?:if\s+exists\s+)?(?:only\s+)?` + identRE + `(?:\s*\.\s*` + identRE + `)?\s+(.*)$`)
	dropTableRE    = regexp.MustCompile(`(?is)^drop\s+table\s+(?:if\s+exists\s+)?(.*?)(?:\s+(?:cascade|restrict))?$`)
	addColumnRE    = regexp.MustCompile(`(?is)^add\s+(?:column\s+)?(?:if\s+not\s+exists\s+)?` + identRE + `\s+(.*)$`)
	dropColumnRE   = regexp.MustCompile(`(?is)^drop\s+(?:column\s+)?(?:if\s+exists\s+)?` + identRE + `(?:\s+(?:cascade|restrict))?$`)
	renameColRE    = regexp.MustCompile(`(?is)^rename\s+(?:column\s+)?` + identRE + `\s+to\s+` + identRE + `$`)
	renameTableRE  = regexp.MustCompile(`(?is)^rename\s+to\s+` + identRE + `$`)
	setSchemaRE    = regexp.MustCompile(`(?is)^set\s+schema\s+` + identRE + `$`)
	constraintRE   = regexp.MustCompile(`(?i)^(constraint|primary|unique|check|foreign|exclude|like)\b`)
	versionRE      = regexp.MustCompile(`^(\d+)_`)
)

type column struct {
	name, typ, file string
	line            int
}

type table struct {
	schema, name, file string
	line               int
	cols               map[string]*column
}

// netSchema is the evaluated state of every service's migrations.
type netSchema struct {
	tables    map[string]*table // schema.table
	schemas   []schemaDecl      // CREATE SCHEMA / RENAME / Go-declared names, for CONF-06
	misplaced []placement       // tables created or moved outside their service's schema
	errors    []placement       // statements the evaluator cannot follow
}

type schemaDecl struct {
	name, file string
	line       int
	how        string
}

type placement struct {
	file string
	line int
	msg  string
}

type legacyRename struct {
	name, legacy string
	version      int64
}

func unquote(id string) string {
	if strings.HasPrefix(id, `"`) {
		return strings.Trim(id, `"`)
	}
	return strings.ToLower(id)
}

// evalSchemas evaluates the migrations once per tree.
func evalSchemas(p *Pass) *netSchema {
	if p.Tree.schema != nil {
		return p.Tree.schema
	}
	ns := &netSchema{tables: map[string]*table{}}
	p.Tree.schema = ns
	renames := map[string]legacyRename{} // migration directory -> rename
	g := p.Tree.goIndex()
	if gf := g.file("services/migrations/migrations.go"); gf.File != nil && p.Tree.Lines("services/migrations/migrations.go") != nil {
		var lits []*ast.CompositeLit // Schema{…}, and the elided elements of []Schema{{…}, …}
		ast.Inspect(gf.File, func(n ast.Node) bool {
			if cl, ok := n.(*ast.CompositeLit); ok {
				if typeName(cl.Type) == "Schema" {
					lits = append(lits, cl)
				} else if at, ok := cl.Type.(*ast.ArrayType); ok && typeName(at.Elt) == "Schema" {
					for _, el := range cl.Elts {
						if e, ok := el.(*ast.CompositeLit); ok && e.Type == nil {
							lits = append(lits, e)
						}
					}
				}
			}
			return true
		})
		for _, cl := range lits {
			var r legacyRename
			var dir string
			if v := field(cl, "Name"); v != nil {
				r.name, _ = g.String(gf, v)
				ns.schemas = append(ns.schemas, schemaDecl{r.name, gf.Path, g.line(v.Pos()), "declared in migrations.Schemas"})
			}
			if v := field(cl, "Dir"); v != nil {
				dir, _ = g.String(gf, v)
			}
			if v := field(cl, "Legacy"); v != nil {
				r.legacy, _ = g.String(gf, v)
			}
			if v := field(cl, "LegacyVersion"); v != nil {
				r.version, _ = g.Int(gf, v)
			}
			if dir != "" {
				renames[dir] = r
			}
		}
	}
	byDir := map[string][]string{}
	for _, f := range p.Tree.Files {
		if strings.HasPrefix(f, "services/migrations/") && strings.HasSuffix(f, ".sql") {
			rel := strings.TrimPrefix(f, "services/migrations/")
			if strings.Count(rel, "/") == 1 {
				byDir[path.Dir(rel)] = append(byDir[path.Dir(rel)], f)
			}
		}
	}
	dirs := make([]string, 0, len(byDir))
	for d := range byDir {
		dirs = append(dirs, d)
	}
	sort.Strings(dirs)
	for _, dir := range dirs {
		files := byDir[dir]
		sort.Slice(files, func(i, j int) bool { return fileVersion(files[i]) < fileVersion(files[j]) })
		r := renames[dir]
		own := r.name
		if own == "" {
			own = "svc_" + dir
		}
		for _, f := range files {
			v := fileVersion(f)
			netName := func(s string) string {
				if r.legacy != "" && s == r.legacy && v <= r.version {
					return r.name
				}
				return s
			}
			for _, st := range sqlStatements(p.Tree.Lines(f)) {
				ns.apply(f, st, own, netName)
			}
		}
	}
	return ns
}

func fileVersion(f string) int64 {
	m := versionRE.FindStringSubmatch(path.Base(f))
	if m == nil {
		return 1 << 62
	}
	v, _ := strconv.ParseInt(m[1], 10, 64)
	return v
}

func (ns *netSchema) apply(f string, st sqlStmt, own string, netName func(string) string) {
	s := st.text
	switch {
	case createSchemaRE.MatchString(s):
		ns.schemas = append(ns.schemas, schemaDecl{netName(unquote(createSchemaRE.FindStringSubmatch(s)[1])), f, st.line, "CREATE SCHEMA"})
	case renameSchemaRE.MatchString(s):
		m := renameSchemaRE.FindStringSubmatch(s)
		ns.schemas = append(ns.schemas, schemaDecl{unquote(m[2]), f, st.line, "ALTER SCHEMA … RENAME TO"})
		for _, t := range ns.tables {
			if t.schema == unquote(m[1]) {
				t.schema = unquote(m[2])
			}
		}
	case createTableRE.MatchString(s):
		m := createTableRE.FindStringSubmatch(s)
		schema, name := "", unquote(m[1])
		if m[2] != "" {
			schema, name = netName(unquote(m[1])), unquote(m[2])
		}
		if schema == "" {
			ns.misplaced = append(ns.misplaced, placement{f, st.line, "table " + name + " is created without a schema, " +
				"so it lands in the search_path's; qualify it with " + own})
			schema = own
		} else if schema != own {
			ns.misplaced = append(ns.misplaced, placement{f, st.line, "table " + schema + "." + name +
				" is created outside its service's schema " + own})
		}
		t := &table{schema: schema, name: name, file: f, line: st.line, cols: map[string]*column{}}
		for _, el := range splitTop(parenBody(s[len(m[0])-1:])) {
			el = strings.TrimSpace(el)
			if el == "" || constraintRE.MatchString(el) {
				continue
			}
			if cm := regexp.MustCompile(`^` + identRE + `\s+(.*)$`).FindStringSubmatch(el); cm != nil {
				t.cols[unquote(cm[1])] = &column{unquote(cm[1]), cm[2], f, st.lineOf(cm[1])}
			}
		}
		ns.tables[schema+"."+name] = t
	case alterTableRE.MatchString(s):
		m := alterTableRE.FindStringSubmatch(s)
		schema, name, actions := own, unquote(m[1]), m[3]
		if m[2] != "" {
			schema, name = netName(unquote(m[1])), unquote(m[2])
		}
		t := ns.tables[schema+"."+name]
		if t == nil {
			ns.errors = append(ns.errors, placement{f, st.line, "ALTER TABLE of " + schema + "." + name +
				", which no earlier migration of this service creates"})
			return
		}
		for _, a := range splitTop(actions) {
			a = strings.TrimSpace(a)
			switch {
			case addColumnRE.MatchString(a) && !constraintRE.MatchString(strings.TrimSpace(a[3:])):
				c := addColumnRE.FindStringSubmatch(a)
				t.cols[unquote(c[1])] = &column{unquote(c[1]), c[2], f, st.lineOf(c[1])}
			case dropColumnRE.MatchString(a) && !regexp.MustCompile(`(?i)^drop\s+(constraint|default|not\s+null)`).MatchString(a):
				delete(t.cols, unquote(dropColumnRE.FindStringSubmatch(a)[1]))
			case renameColRE.MatchString(a):
				c := renameColRE.FindStringSubmatch(a)
				if col := t.cols[unquote(c[1])]; col != nil {
					delete(t.cols, col.name)
					col.name, col.file, col.line = unquote(c[2]), f, st.lineOf(c[2])
					t.cols[col.name] = col
				}
			case renameTableRE.MatchString(a):
				delete(ns.tables, schema+"."+name)
				t.name = unquote(renameTableRE.FindStringSubmatch(a)[1])
				ns.tables[t.schema+"."+t.name] = t
			case setSchemaRE.MatchString(a):
				to := unquote(setSchemaRE.FindStringSubmatch(a)[1])
				if to != own {
					ns.misplaced = append(ns.misplaced, placement{f, st.line, "table " + name + " is moved to schema " +
						to + ", outside its service's schema " + own})
				}
				delete(ns.tables, schema+"."+name)
				t.schema = to
				ns.tables[t.schema+"."+t.name] = t
			}
		}
	case dropTableRE.MatchString(s):
		for _, n := range strings.Split(dropTableRE.FindStringSubmatch(s)[1], ",") {
			parts := strings.SplitN(strings.TrimSpace(n), ".", 2)
			key := own + "." + unquote(parts[0])
			if len(parts) == 2 {
				key = netName(unquote(parts[0])) + "." + unquote(parts[1])
			}
			delete(ns.tables, key)
		}
	}
}

func (ns *netSchema) reportSchemas(p *Pass) {
	for _, d := range ns.schemas {
		if !svcSchemaRE.MatchString(d.name) {
			p.Report(d.file, d.line, "schema %q (%s) is not named svc_<service> (05 §1.4, §3)", d.name, d.how)
		}
	}
	for _, m := range ns.misplaced {
		p.Report(m.file, m.line, "%s (05 §3)", m.msg)
	}
	for _, e := range ns.errors {
		p.Report(e.file, e.line, "%s: the net schema cannot be evaluated", e.msg)
	}
}

func (ns *netSchema) reportPII(p *Pass) {
	keys := make([]string, 0, len(ns.tables))
	for k := range ns.tables {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		t := ns.tables[k]
		for _, c := range t.cols {
			pii := piiWordRE.MatchString(c.name) || piiTypesRE.MatchString(c.typ)
			switch {
			case pii && !piiSafeRE.MatchString(c.name):
				p.Report(c.file, c.line, "column %s.%s holds direct PII in plain text: store *_ct ciphertext or a "+
					"*_bidx blind index (05 §3, §6.6)", k, c.name)
			case pii && t.schema != "svc_identity":
				p.Report(c.file, c.line, "PII column %s.%s lives outside svc_identity (05 §6.6)", k, c.name)
			}
		}
	}
}

var (
	hschemaPkgRE = regexp.MustCompile(`^\s*package\s+([A-Za-z0-9_.]+)`)
	piiAttrRE    = regexp.MustCompile(`@pii\b`)
)

// checkPIIAttributes reports `@pii` fields in schemas outside the Identity package (05 §6.6).
func checkPIIAttributes(p *Pass) {
	for _, f := range p.Files {
		if !strings.HasSuffix(f, ".hschema") {
			continue
		}
		pkg := ""
		lines := codeLines(p.Tree.Lines(f), true)
		for i, l := range lines {
			if m := hschemaPkgRE.FindStringSubmatch(l); m != nil && pkg == "" {
				pkg = m[1]
			}
			if piiAttrRE.MatchString(l) && pkg != "identity" && !strings.HasPrefix(pkg, "identity.") {
				p.Report(f, i+1, "@pii field in package %q: only Identity holds direct PII (05 §6.6)", pkg)
			}
		}
	}
}

// sqlStmt is one SQL statement with comments removed, dollar-quoted bodies and string literals blanked,
// and whitespace collapsed; line is where it starts.
type sqlStmt struct {
	text  string
	line  int
	lines []string // the raw lines it spans, for lineOf
}

// lineOf returns the line of the statement where ident first appears as a word, or its first line.
func (s sqlStmt) lineOf(ident string) int {
	re := regexp.MustCompile(`(?i)(^|[^A-Za-z0-9_])` + regexp.QuoteMeta(strings.Trim(ident, `"`)) + `($|[^A-Za-z0-9_])`)
	for i, l := range s.lines {
		if re.MatchString(strings.SplitN(l, "--", 2)[0]) {
			return s.line + i
		}
	}
	return s.line
}

// sqlStatements splits the `-- +goose Up` part of a migration (all of it without goose annotations).
func sqlStatements(lines []string) []sqlStmt {
	annotated := false
	for _, l := range lines {
		if strings.HasPrefix(strings.TrimSpace(l), "-- +goose Up") {
			annotated = true
		}
	}
	var out []sqlStmt
	var cur strings.Builder
	start, up, inBlock, dollar := 0, !annotated, false, ""
	flush := func(end int) {
		if t := strings.Join(strings.Fields(cur.String()), " "); t != "" && up {
			out = append(out, sqlStmt{text: t, line: start + 1, lines: lines[start : end+1]})
		}
		cur.Reset()
	}
	for i, l := range lines {
		if dollar == "" && !inBlock {
			switch t := strings.TrimSpace(l); {
			case strings.HasPrefix(t, "-- +goose Up"):
				flush(i)
				up = true
				continue
			case strings.HasPrefix(t, "-- +goose Down"):
				flush(i)
				up = false
				continue
			}
		}
		if strings.TrimSpace(cur.String()) == "" {
			start = i
		}
		for j := 0; j < len(l); j++ {
			c := l[j]
			switch {
			case inBlock:
				if c == '*' && j+1 < len(l) && l[j+1] == '/' {
					inBlock = false
					j++
				}
			case dollar != "":
				if strings.HasPrefix(l[j:], dollar) {
					cur.WriteString(dollar)
					j += len(dollar) - 1
					dollar = ""
				}
			case c == '-' && j+1 < len(l) && l[j+1] == '-':
				j = len(l)
			case c == '/' && j+1 < len(l) && l[j+1] == '*':
				inBlock = true
				j++
			case c == '$':
				if m := regexp.MustCompile(`^\$[A-Za-z_]*\$`).FindString(l[j:]); m != "" {
					dollar = m
					cur.WriteString(m)
					j += len(m) - 1
				} else {
					cur.WriteByte(c)
				}
			case c == '\'':
				cur.WriteString("''")
				for j++; j < len(l) && !(l[j] == '\'' && (j+1 >= len(l) || l[j+1] != '\'')); j++ {
					if l[j] == '\'' {
						j++
					}
				}
			case c == ';':
				flush(i)
				start = i
			default:
				cur.WriteByte(c)
			}
		}
		cur.WriteByte('\n')
	}
	flush(len(lines) - 1)
	return out
}

// parenBody returns what the parenthesis that opens s encloses (the rest of s if it never closes), so a
// table's column list stops before `PARTITION BY (…)` or `WITH (…)`.
func parenBody(s string) string {
	depth := 0
	for i, c := range s {
		switch c {
		case '(':
			depth++
		case ')':
			if depth--; depth == 0 {
				return s[1:i]
			}
		}
	}
	return strings.TrimPrefix(s, "(")
}

// splitTop splits at commas outside parentheses.
func splitTop(s string) []string {
	var out []string
	depth, last := 0, 0
	for i, c := range s {
		switch c {
		case '(':
			depth++
		case ')':
			depth--
		case ',':
			if depth == 0 {
				out = append(out, s[last:i])
				last = i + 1
			}
		}
	}
	return append(out, s[last:])
}
