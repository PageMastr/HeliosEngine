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
	piiWordRE  = regexp.MustCompile(`(^|_)(e_?mail|email_norm|dob|date_of_birth|birth_?date|birthday|ip|ip_?addr(ess)?|ipv[46]|remote_addr|real_name|full_name|first_name|last_name|legal_name|given_name|family_name|surname)(_|$)`)
	piiSafeRE  = regexp.MustCompile(`_(ct|bidx)$`)
	piiTypesRE = regexp.MustCompile(`(?i)^(inet|cidr)\b`)
	identRE    = `("[^"]+"|[A-Za-z_][A-Za-z0-9_$]*)`
	// CREATE SCHEMA name [AUTHORIZATION role], and nothing after it: a schema with elements (`CREATE SCHEMA s
	// CREATE TABLE t (…)`) creates tables too, so it fails closed below.
	createSchemaRE = regexp.MustCompile(`(?is)^create\s+schema\s+(?:if\s+not\s+exists\s+)?` + identRE +
		`(?:\s+authorization\s+` + identRE + `)?\s*$`)
	renameSchemaRE = regexp.MustCompile(`(?is)^alter\s+schema\s+` + identRE + `\s+rename\s+to\s+` + identRE)
	createTableRE  = regexp.MustCompile(`(?is)^create\s+(?:(?:global\s+|local\s+)?(?:temp|temporary|unlogged)\s+)?table\s+(if\s+not\s+exists\s+)?` + identRE + `(?:\s*\.\s*` + identRE + `)?\s*\(`)
	// ALTER TABLE [IF EXISTS] [ONLY] name [*] actions: `*` names the descendants too (the default).
	alterTableRE = regexp.MustCompile(`(?is)^alter\s+table\s+(?:if\s+exists\s+)?(?:only\s+)?` + identRE + `(?:\s*\.\s*` + identRE + `)?(?:\s*\*)?\s+(.*)$`)
	dropTableRE  = regexp.MustCompile(`(?is)^drop\s+table\s+(?:if\s+exists\s+)?(.*?)(?:\s+(?:cascade|restrict))?$`)
	addColumnRE  = regexp.MustCompile(`(?is)^add\s+(?:column\s+)?(?:if\s+not\s+exists\s+)?` + identRE + `\s*(.*)$`)
	// ADD COLUMN IF NOT EXISTS: PostgreSQL skips the action when the column exists, which keeps its type.
	addIfNotExistsRE = regexp.MustCompile(`(?is)^add\s+(?:column\s+)?if\s+not\s+exists\b`)
	dropColumnRE     = regexp.MustCompile(`(?is)^drop\s+(?:column\s+)?(?:if\s+exists\s+)?` + identRE + `(?:\s+(?:cascade|restrict))?$`)
	renameColRE      = regexp.MustCompile(`(?is)^rename\s+(?:column\s+)?` + identRE + `\s+to\s+` + identRE + `$`)
	renameTableRE    = regexp.MustCompile(`(?is)^rename\s+to\s+` + identRE + `$`)
	setSchemaRE      = regexp.MustCompile(`(?is)^set\s+schema\s+` + identRE + `$`)
	constraintRE     = regexp.MustCompile(`(?i)^(constraint|primary|unique|check|foreign|exclude)\b`)
	alterTypeRE      = regexp.MustCompile(`(?is)^alter\s+(?:column\s+)?` + identRE + `\s+(?:set\s+data\s+)?type\s+(.*)$`)
	// ALTER TABLE actions that change no column: constraints, column defaults, statistics and identity,
	// ownership, triggers and row security, replica identity, clustering, storage parameters, the table's
	// persistence, access method and tablespace, and partitions attached or detached.
	alterKeepRE = regexp.MustCompile(`(?is)^(?:add\s+(?:constraint|primary|unique|check|foreign|exclude)\b|` +
		`(?:drop|validate|alter|rename)\s+constraint\b|alter\s+(?:column\s+)?` + identRE + `\s+(?:set|drop|reset|add)\b|` +
		`owner\s+to\b|(?:enable|disable)\b|replica\s+identity\b|cluster\s+on\b|` +
		`set\s+(?:logged|unlogged|without\s+cluster|access\s+method|tablespace)\b|(?:set|reset)\s*\(|` +
		`(?:no\s+)?force\s+row\s+level\s+security\b|(?:attach|detach)\s+partition\b)`)
	likeRE     = regexp.MustCompile(`(?is)^like\s+` + identRE + `(?:\s*\.\s*` + identRE + `)?`)
	inheritsRE = regexp.MustCompile(`(?is)^\s*inherits\s*\(([^()]*)\)`)
	inheritRE  = regexp.MustCompile(`(?is)^(?:no\s+)?inherit\s+`)
	qualNameRE = regexp.MustCompile(`(?is)^\s*` + identRE + `(?:\s*\.\s*` + identRE + `)?\s*$`)
	// Statements that create or change tables in a form the evaluator does not follow: CREATE TABLE … AS,
	// PARTITION OF, OF type, or a name goose substitutes (${…}); CREATE SCHEMA with such a name; a
	// materialized view (it stores rows); and a DO block, whose body runs with the migration.
	createTableAnyRE  = regexp.MustCompile(`(?is)^create\s+(?:(?:global|local)\s+)?(?:(?:temp|temporary|unlogged|foreign)\s+)?table\b`)
	createSchemaAnyRE = regexp.MustCompile(`(?is)^create\s+schema\b`)
	createMatViewRE   = regexp.MustCompile(`(?is)^create\s+(?:or\s+replace\s+)?materialized\s+view\b`)
	importSchemaRE    = regexp.MustCompile(`(?is)^import\s+foreign\s+schema\b`)
	doRE              = regexp.MustCompile(`(?is)^do\b`)
	callRE            = regexp.MustCompile(`(?is)^call\b`)
	selectLeadRE      = regexp.MustCompile(`(?is)^(?:\(\s*)*(?:select|with)\b`)
	// INTO and the word before it ("email"INTO needs no space: the quote ends the identifier).
	intoRE       = regexp.MustCompile(`(?i)(\w*)\W*\binto\b`)
	versionRE    = regexp.MustCompile(`^(\d+)_`)
	columnDefRE  = regexp.MustCompile(`^` + identRE + `\s*(.*)$`) // "email"TEXT needs no space either
	dropNotColRE = regexp.MustCompile(`(?i)^drop\s+(constraint|default|not\s+null)`)
	// A dollar-quote tag follows the rules of an unquoted identifier: a letter, '_' or a non-ASCII
	// character, then those or digits (PostgreSQL, "Dollar-Quoted String Constants").
	dollarTagRE = regexp.MustCompile(`^\$(?:[A-Za-z_\x{80}-\x{10FFFF}][A-Za-z0-9_\x{80}-\x{10FFFF}]*)?\$`)
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
	order := map[string]int{}            // migration directory -> its index in Schemas (apply order)
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
				if _, ok := order[dir]; !ok {
					order[dir] = len(order)
				}
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
	// migrations.Up applies the services in Schemas order; a directory Schemas does not declare comes after
	// them, in name order.
	sort.Slice(dirs, func(i, j int) bool {
		oi, iok := order[dirs[i]]
		oj, jok := order[dirs[j]]
		if iok != jok {
			return iok
		}
		if iok {
			return oi < oj
		}
		return dirs[i] < dirs[j]
	})
	for _, dir := range dirs {
		files := byDir[dir]
		sort.Slice(files, func(i, j int) bool { return fileVersion(files[i]) < fileVersion(files[j]) })
		r := renames[dir]
		own := r.name
		if own == "" {
			own = "svc_" + dir
		}
		for _, f := range files {
			for i, l := range p.Tree.Lines(f) {
				if cmd, ok := gooseCommand(l); ok && strings.HasPrefix(cmd, "envsub") && !strings.HasSuffix(cmd, "off") {
					ns.errors = append(ns.errors, placement{f, i + 1, "-- +goose ENVSUB ON substitutes environment " +
						"variables into the SQL, so the statements are not what the file says"})
				}
			}
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
		ifNotExists, first, second := m[1] != "", m[2], m[3]
		schema, name := "", unquote(first)
		if second != "" {
			schema, name = netName(unquote(first)), unquote(second)
		}
		if schema == "" {
			ns.misplaced = append(ns.misplaced, placement{f, st.line, "table " + name + " is created without a schema, " +
				"so it lands in the search_path's; qualify it with " + own})
			schema = own
		} else if schema != own {
			ns.misplaced = append(ns.misplaced, placement{f, st.line, "table " + schema + "." + name +
				" is created outside its service's schema " + own})
		}
		if ifNotExists && ns.tables[schema+"."+name] != nil {
			return // PostgreSQL skips the statement: the table keeps the columns it has
		}
		t := &table{schema: schema, name: name, file: f, line: st.line, cols: map[string]*column{}}
		body, rest := parenSplit(s[len(m[0])-1:])
		// LIKE src and INHERITS (parents) give the table the columns their sources have now.
		copyFrom := func(src string) {
			src = strings.TrimSpace(src)
			from := ns.lookup(src, own, netName)
			if from == nil {
				ns.errors = append(ns.errors, placement{f, st.line, "table " + schema + "." + name + " copies the " +
					"columns of " + src + ", which no earlier migration of this service creates"})
				return
			}
			for _, c := range from.cols {
				t.cols[c.name] = &column{c.name, c.typ, f, st.lineOf(src)}
			}
		}
		for _, el := range splitTop(body) {
			el = strings.TrimSpace(el)
			switch {
			case el == "" || constraintRE.MatchString(el):
			case likeRE.MatchString(el):
				copyFrom(likeRE.FindString(el)[len("like"):])
			default:
				if cm := columnDefRE.FindStringSubmatch(el); cm != nil {
					t.cols[unquote(cm[1])] = &column{unquote(cm[1]), cm[2], f, st.lineOf(cm[1])}
				}
			}
		}
		if im := inheritsRE.FindStringSubmatch(rest); im != nil {
			for _, parent := range strings.Split(im[1], ",") {
				copyFrom(parent)
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
			case a == "":
			case addColumnRE.MatchString(a) && !constraintRE.MatchString(strings.TrimSpace(a[3:])):
				c := addColumnRE.FindStringSubmatch(a)
				if t.cols[unquote(c[1])] != nil && addIfNotExistsRE.MatchString(a) {
					continue
				}
				t.cols[unquote(c[1])] = &column{unquote(c[1]), c[2], f, st.lineOf(c[1])}
			case dropColumnRE.MatchString(a) && !dropNotColRE.MatchString(a):
				delete(t.cols, unquote(dropColumnRE.FindStringSubmatch(a)[1]))
			case renameColRE.MatchString(a):
				c := renameColRE.FindStringSubmatch(a)
				col := t.cols[unquote(c[1])]
				if col == nil {
					ns.errors = append(ns.errors, placement{f, st.line, "RENAME COLUMN " + unquote(c[1]) + " of " +
						schema + "." + name + ", which has no such column"})
					continue
				}
				delete(t.cols, col.name)
				col.name, col.file, col.line = unquote(c[2]), f, st.lineOf(c[2])
				t.cols[col.name] = col
			case alterTypeRE.MatchString(a):
				c := alterTypeRE.FindStringSubmatch(a)
				col := t.cols[unquote(c[1])]
				if col == nil {
					ns.errors = append(ns.errors, placement{f, st.line, "ALTER COLUMN " + unquote(c[1]) + " TYPE of " +
						schema + "." + name + ", which has no such column"})
					continue
				}
				col.typ, col.file, col.line = c[2], f, st.lineOf(c[1])
			case inheritRE.MatchString(a):
				ns.errors = append(ns.errors, placement{f, st.line, "ALTER TABLE " + schema + "." + name +
					" changes its parents (INHERIT), and with them its columns"})
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
			case !alterKeepRE.MatchString(a):
				ns.errors = append(ns.errors, placement{f, st.line, "ALTER TABLE " + schema + "." + name + " " +
					firstWords(a, 3) + ": an ALTER TABLE action the lint does not read"})
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
	case createTableAnyRE.MatchString(s), createSchemaAnyRE.MatchString(s), createMatViewRE.MatchString(s),
		importSchemaRE.MatchString(s):
		ns.errors = append(ns.errors, placement{f, st.line, "a table or schema created in a form the lint does not " +
			"read (CREATE TABLE … AS, PARTITION OF or OF type, CREATE SCHEMA with elements, IMPORT FOREIGN SCHEMA, a " +
			"materialized view, or a substituted name)"})
	case selectInto(s):
		ns.errors = append(ns.errors, placement{f, st.line, "SELECT … INTO creates a table the lint does not read"})
	case doRE.MatchString(s):
		ns.errors = append(ns.errors, placement{f, st.line, "a DO block runs with the migration, and the lint does " +
			"not read its body"})
	case callRE.MatchString(s):
		ns.errors = append(ns.errors, placement{f, st.line, "CALL runs a procedure with the migration, and the lint " +
			"does not read its body"})
	}
}

// firstWords is the start of an SQL fragment for a message: its first n words, whitespace collapsed.
func firstWords(s string, n int) string {
	w := strings.Fields(s)
	if len(w) > n {
		return strings.Join(w[:n], " ") + " …"
	}
	return strings.Join(w, " ")
}

// lookup finds the table a (possibly schema-qualified) name refers to, as of the statements applied so far.
func (ns *netSchema) lookup(qname, own string, netName func(string) string) *table {
	m := qualNameRE.FindStringSubmatch(qname)
	if m == nil {
		return nil
	}
	if m[2] == "" {
		return ns.tables[own+"."+unquote(m[1])]
	}
	return ns.tables[netName(unquote(m[1]))+"."+unquote(m[2])]
}

// selectInto reports a SELECT … INTO (or WITH … SELECT … INTO) statement, which creates a table. An
// INTO after INSERT or MERGE is not one.
func selectInto(s string) bool {
	if !selectLeadRE.MatchString(s) {
		return false
	}
	for _, m := range intoRE.FindAllStringSubmatch(s, -1) {
		if w := strings.ToLower(m[1]); w != "insert" && w != "merge" {
			return true
		}
	}
	return false
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
	// A statement the evaluator cannot follow may create or keep a PII column, so it fails this rule too: a
	// CONF-06 suppression on its line does not hide it from the PII check.
	for _, e := range ns.errors {
		p.Report(e.file, e.line, "%s: the net schema cannot be evaluated, so its PII columns cannot be checked", e.msg)
	}
	keys := make([]string, 0, len(ns.tables))
	for k := range ns.tables {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		t := ns.tables[k]
		names := make([]string, 0, len(t.cols))
		for n := range t.cols {
			names = append(names, n)
		}
		sort.Strings(names) // a stable report order: several columns can be reported on one line
		for _, n := range names {
			c := t.cols[n]
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

// lineOf returns the line of the statement where ident first appears as a word (any case, outside a
// `--` comment), or its first line.
func (s sqlStmt) lineOf(ident string) int {
	w := strings.ToLower(strings.Trim(ident, `"`))
	if w == "" {
		return s.line
	}
	for i, l := range s.lines {
		code := strings.ToLower(strings.SplitN(l, "--", 2)[0])
		for k := 0; ; {
			at := strings.Index(code[k:], w)
			if at < 0 {
				break
			}
			at += k
			end := at + len(w)
			if (at == 0 || !wordByte(code[at-1])) && (end == len(code) || !wordByte(code[end])) {
				return s.line + i
			}
			k = at + 1
		}
	}
	return s.line
}

func wordByte(b byte) bool {
	return b == '_' || b >= '0' && b <= '9' || b|0x20 >= 'a' && b|0x20 <= 'z'
}

// identByte reports a byte that continues an unquoted PostgreSQL identifier (letters, digits, '_',
// '$' and non-ASCII), so that the '$' of `a$b$` or the E of `name'…'` belongs to the identifier.
func identByte(b byte) bool {
	return wordByte(b) || b == '$' || b >= 0x80
}

// gooseAnnotation reads a line as goose v3 (the version services/go.mod pins) does in
// internal/sqlparser: a line that starts with "--" and contains "+goose" is an annotation; every "--"
// and the first "+goose" are removed and the rest is compared without case, so `-- +goose down` and
// `--+goose Up` count. dir is "up", "down", or "" for the other annotations (StatementBegin, …),
// which are plain comments to SQL. goose rejects an annotation with leading whitespace; here it counts.
func gooseAnnotation(line string) (dir string, ok bool) {
	cmd, ok := gooseCommand(line)
	if cmd == "up" || cmd == "down" {
		return cmd, true
	}
	return "", ok
}

// gooseCommand returns a goose annotation's command in lower case with its words single-spaced ("up",
// "statementbegin", "envsub on"), read as gooseAnnotation reads it.
func gooseCommand(line string) (string, bool) {
	t := strings.TrimSpace(line)
	if !strings.HasPrefix(t, "--") || !strings.Contains(t, "+goose") {
		return "", false
	}
	cmd := strings.Replace(strings.ReplaceAll(t, "--", ""), "+goose", "", 1)
	return strings.ToLower(strings.Join(strings.Fields(cmd), " ")), true
}

// sqlStatements splits the `-- +goose Up` part of a migration (all of it without goose annotations)
// the way PostgreSQL's lexer reads it: `--` comments; nested `/* */` comments; '…' strings, where a
// doubled quote is an escape and which may span lines, and E'…' strings, where a backslash escapes too;
// "…" identifiers; and $tag$ … $tag$ bodies. Strings and bodies are blanked, so a ';' or a statement
// inside one is never applied.
func sqlStatements(lines []string) []sqlStmt {
	annotated := false
	for _, l := range lines {
		if d, _ := gooseAnnotation(l); d == "up" {
			annotated = true
		}
	}
	var out []sqlStmt
	var cur strings.Builder
	start, up := 0, !annotated
	depth := 0       // nesting depth of /* */ comments
	dollar := ""     // the tag of an open dollar-quoted body
	quote := byte(0) // ' or " while a string or a quoted identifier is open
	esc := false     // the open string is an E'…' string
	flush := func(end int) {
		if t := strings.Join(strings.Fields(cur.String()), " "); t != "" && up {
			out = append(out, sqlStmt{text: t, line: start + 1, lines: lines[start : end+1]})
		}
		cur.Reset()
	}
	for i, l := range lines {
		if depth == 0 && dollar == "" && quote == 0 {
			if d, _ := gooseAnnotation(l); d != "" {
				flush(i)
				up = d == "up"
				continue
			}
		}
		if strings.TrimSpace(cur.String()) == "" {
			start = i
		}
		for j := 0; j < len(l); j++ {
			c := l[j]
			next := byte(0)
			if j+1 < len(l) {
				next = l[j+1]
			}
			switch {
			case depth > 0:
				if c == '*' && next == '/' {
					depth--
					j++
				} else if c == '/' && next == '*' {
					depth++
					j++
				}
			case dollar != "":
				if strings.HasPrefix(l[j:], dollar) {
					cur.WriteString(dollar)
					j += len(dollar) - 1
					dollar = ""
				}
			case quote == '\'':
				switch {
				case esc && c == '\\':
					j++
				case c == '\'' && next == '\'':
					j++
				case c == '\'':
					quote = 0
				}
			case quote == '"':
				cur.WriteByte(c)
				if c == '"' && next == '"' {
					cur.WriteByte(next)
					j++
				} else if c == '"' {
					quote = 0
				}
			case c == '-' && next == '-':
				j = len(l)
			case c == '/' && next == '*':
				depth = 1
				cur.WriteByte(' ') // a comment separates tokens
				j++
			case c == '$' && (j == 0 || !identByte(l[j-1])):
				if m := dollarTagRE.FindString(l[j:]); m != "" {
					dollar = m
					cur.WriteString(m)
					j += len(m) - 1
				} else {
					cur.WriteByte(c)
				}
			case c == '\'':
				// E'…' (any case) when the E does not end an identifier.
				esc = j > 0 && l[j-1]|0x20 == 'e' && (j < 2 || !identByte(l[j-2]))
				quote = '\''
				cur.WriteString("''")
			case c == '"':
				quote = '"'
				cur.WriteByte(c)
			case c == ';':
				flush(i)
				start = i
			default:
				cur.WriteByte(c)
			}
		}
		if quote == 0 && dollar == "" {
			cur.WriteByte('\n')
		}
	}
	flush(len(lines) - 1)
	return out
}

// parenSplit returns what the parenthesis that opens s encloses (the rest of s if it never closes), so a
// table's column list stops before `PARTITION BY (…)`, `INHERITS (…)` or `WITH (…)`, and what follows it.
func parenSplit(s string) (body, rest string) {
	depth := 0
	for i, c := range s {
		switch c {
		case '(':
			depth++
		case ')':
			if depth--; depth == 0 {
				return s[1:i], s[i+1:]
			}
		}
	}
	return strings.TrimPrefix(s, "("), ""
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
