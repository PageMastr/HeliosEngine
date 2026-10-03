package conformance

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// ToolRule is the rule ID of the lint's own findings: an unreadable map, a malformed or unused
// suppression, a stale known-failing record, a source file a rule cannot parse.
const ToolRule = "conformance"

// Rule is one row of 09 §5.10.3's CONF table.
type Rule struct {
	ID     string
	Anchor string         // the table's Anchor column
	Title  string         // what it flags, in one line
	Scope  []string       // the table's Scope column as globs; map entries naming the rule add paths (D2)
	Allow  []string       // paths the rule's own text exempts
	Types  *regexp.Regexp // file names the rule reads (nil: every file in scope)
	Check  func(*Pass)
}

// covers reports whether the rule reads file f, given its scope globs.
func (r *Rule) covers(scope []string, f string) bool {
	if r.Types != nil && !r.Types.MatchString(f) {
		return false
	}
	return MatchAny(scope, f) && !MatchAny(r.Allow, f)
}

// Pass is one rule's run over the tree.
type Pass struct {
	Rule  *Rule
	Tree  *Tree
	Files []string // the files in the rule's scope
	Scope []string // the scope globs: the table's, then the map's
	Tests []string // conformance tests the map requires for the rule's anchors
	out   *[]Finding
	seen  map[string]bool
	cstr  map[string][]string // the scope's C string constants (cStrTable), built on first use
}

// Report records a finding. line 0 means the finding has no line (a missing file or test). Distinct
// messages on one line are distinct findings (a suppression on the line covers them all); the same
// message twice on a line is one.
func (p *Pass) Report(file string, line int, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	key := fmt.Sprintf("%s:%d:%s", file, line, msg)
	if p.seen == nil {
		p.seen = map[string]bool{}
	}
	if p.seen[key] {
		return
	}
	p.seen[key] = true
	*p.out = append(*p.out, Finding{Rule: p.Rule.ID, Path: file, Line: line, Message: msg})
}

// Finding is one diagnostic. Suppressed and Known are set by Run.
type Finding struct {
	Rule, Path  string
	Line        int
	Message     string
	Suppressed  string // the reason of the `conformance:allow` comment on its line
	Known       *Known // the known-failing record that covers it
	Fingerprint string // rule, file, message and trimmed source line, hashed (known-failing records pin it)
}

func (f Finding) where() string {
	if f.Line > 0 {
		return fmt.Sprintf("%s:%d", f.Path, f.Line)
	}
	return f.Path
}

// Failing reports whether the finding fails the run.
func (f Finding) Failing() bool { return f.Suppressed == "" && f.Known == nil }

// Options select what Run checks.
type Options struct {
	Root   string
	Rules  []string // rule IDs to run; empty runs all
	Strict bool     // ignore known-failing records (used by fixtures and to prove a rework closed)
}

// Result is the outcome of Run.
type Result struct {
	Rules    []*Rule
	Files    int
	Findings []Finding // sorted by path, line and rule
	// ShowFingerprints makes WriteText end each finding's line with its fingerprint, which a
	// known-failing record pins (the CLI's -fingerprints flag).
	ShowFingerprints bool
}

// Failed reports whether any finding fails the run.
func (r *Result) Failed() bool {
	for _, f := range r.Findings {
		if f.Failing() {
			return true
		}
	}
	return false
}

var (
	suppressRE     = regexp.MustCompile(`conformance:allow\b(.*)`)
	suppressArgsRE = regexp.MustCompile(`^\s+(CONF-\d{2})(\s.*)?$`)
	commentEndRE   = regexp.MustCompile(`\*/|-->`)
	hasWordRE      = regexp.MustCompile(`[A-Za-z0-9]`)
)

// Run lints the tree under opts.Root. The map (tools/conformance/map.jsonc) and the known-failing
// records (tools/conformance/known_failing.jsonc) are read from the root when present, so a fixture
// tree without them runs each rule over its table scope only.
func Run(opts Options) (*Result, error) {
	tree, err := LoadTree(opts.Root)
	if err != nil {
		return nil, err
	}
	rules, err := selectRules(opts.Rules)
	if err != nil {
		return nil, err
	}
	var findings []Finding
	mapFile := filepath.Join(tree.Root, "tools", "conformance", "map.jsonc")
	var anchors map[string]*MapEntry
	if _, err := os.Stat(mapFile); err == nil {
		var bad []Finding
		anchors, bad = loadMap(mapFile, "tools/conformance/map.jsonc", rules)
		findings = append(findings, bad...)
	}
	var known []*Known
	knownFile := filepath.Join(tree.Root, "tools", "conformance", "known_failing.jsonc")
	if _, err := os.Stat(knownFile); err == nil && !opts.Strict {
		var bad []Finding
		known, bad = loadKnown(knownFile, "tools/conformance/known_failing.jsonc")
		findings = append(findings, bad...)
		// A fixture tree has no scorecard; the repository always does (lint_scorecard).
		if scorecard := filepath.Join(tree.Root, "scorecard.jsonc"); fileExists(scorecard) {
			findings = append(findings, checkScorecard(scorecard, "tools/conformance/known_failing.jsonc", known)...)
		}
	}

	selected := map[string]*Rule{}
	scopes := map[string][]string{}
	for _, r := range rules {
		selected[r.ID] = r
		scope := append([]string(nil), r.Scope...)
		var tests []string
		for _, e := range anchors {
			for _, id := range e.Rules {
				if id == r.ID {
					scope = append(scope, e.Paths...)
					tests = append(tests, e.Tests...)
				}
			}
		}
		scopes[r.ID] = scope
		p := &Pass{Rule: r, Tree: tree, Scope: scope, Tests: dedupe(tests), out: &findings}
		for _, f := range tree.Files {
			if r.covers(scope, f) {
				p.Files = append(p.Files, f)
			}
		}
		r.Check(p)
	}

	// Suppressions: every `conformance:allow CONF-nn <reason>` comment in a selected rule's scope must
	// name a known rule, give a reason, and sit on a line where that rule reports a finding.
	type loc struct {
		rule, path string
		line       int
	}
	used := map[loc]bool{}
	allows := map[loc]string{}
	for _, f := range tree.Files {
		if strings.HasSuffix(f, ".md") {
			continue
		}
		read := false
		for id, scope := range scopes {
			read = read || selected[id].covers(scope, f)
		}
		if !read {
			continue
		}
		for i, line := range tree.Lines(f) {
			m := suppressRE.FindStringSubmatch(line)
			if m == nil {
				continue
			}
			a := suppressArgsRE.FindStringSubmatch(m[1])
			if a != nil { // the reason ends where its comment does
				a[2] = strings.TrimSpace(commentEndRE.Split(a[2], 2)[0])
			}
			if a == nil || !hasWordRE.MatchString(a[2]) {
				findings = append(findings, Finding{Rule: ToolRule, Path: f, Line: i + 1,
					Message: "malformed suppression: write `conformance:allow CONF-nn <reason>` (§5.10.3)"})
				continue
			}
			if r := selected[a[1]]; r != nil && r.covers(scopes[a[1]], f) {
				allows[loc{a[1], f, i + 1}] = a[2]
			}
		}
	}
	for i := range findings {
		f := &findings[i]
		source := ""
		if lines := tree.Lines(f.Path); f.Line > 0 && f.Line <= len(lines) {
			source = lines[f.Line-1]
		}
		f.Fingerprint = fingerprint(f.Rule, f.Path, f.Message, source)
		if reason, ok := allows[loc{f.Rule, f.Path, f.Line}]; ok {
			f.Suppressed = reason
			used[loc{f.Rule, f.Path, f.Line}] = true
			continue
		}
	match:
		for _, k := range known {
			if k.Rule != f.Rule {
				continue
			}
			for _, e := range k.Findings {
				if !e.used && e.Path == f.Path && e.Fingerprint == f.Fingerprint {
					e.used = true
					k.hits++
					f.Known = k
					break match
				}
			}
		}
	}
	for l := range allows {
		if !used[l] {
			findings = append(findings, Finding{Rule: ToolRule, Path: l.path, Line: l.line,
				Message: fmt.Sprintf("unused suppression: %s reports nothing on this line; remove it", l.rule)})
		}
	}
	for _, k := range known {
		if selected[k.Rule] == nil {
			continue
		}
		if k.hits == 0 {
			findings = append(findings, Finding{Rule: ToolRule, Path: "tools/conformance/known_failing.jsonc",
				Line: k.line, Message: fmt.Sprintf("stale record: %s (owner %s) matches no finding; remove it "+
					"and the scorecard gap", k.Rule, k.Owner)})
			continue
		}
		for _, e := range k.Findings {
			if !e.used {
				findings = append(findings, Finding{Rule: ToolRule, Path: "tools/conformance/known_failing.jsonc",
					Line: e.line, Message: fmt.Sprintf("stale finding: %s (owner %s) no longer reports %s in %s; "+
						"remove the entry", k.Rule, k.Owner, e.Fingerprint, e.Path)})
			}
		}
	}
	sort.SliceStable(findings, func(i, j int) bool {
		a, b := findings[i], findings[j]
		if a.Path != b.Path {
			return a.Path < b.Path
		}
		if a.Line != b.Line {
			return a.Line < b.Line
		}
		return a.Rule < b.Rule
	})
	return &Result{Rules: rules, Files: len(tree.Files), Findings: findings}, nil
}

func fileExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.Mode().IsRegular()
}

func selectRules(ids []string) ([]*Rule, error) {
	if len(ids) == 0 {
		return All(), nil
	}
	var out []*Rule
	for _, id := range ids {
		r := Lookup(id)
		if r == nil {
			return nil, fmt.Errorf("unknown rule %q", id)
		}
		out = append(out, r)
	}
	return out, nil
}

func dedupe(s []string) []string {
	seen := map[string]bool{}
	var out []string
	for _, x := range s {
		if !seen[x] {
			seen[x] = true
			out = append(out, x)
		}
	}
	sort.Strings(out)
	return out
}

// WriteText prints one line per finding and a summary.
func (r *Result) WriteText(w io.Writer) {
	failing, suppressed := 0, 0
	known := map[string]int{}
	var owners []string
	for _, f := range r.Findings {
		switch {
		case f.Suppressed != "":
			suppressed++
			fmt.Fprintf(w, "%s: %s: suppressed (%s): %s%s\n", f.where(), f.Rule, f.Suppressed, f.Message, r.fp(f))
		case f.Known != nil:
			key := f.Rule + " (" + f.Known.Owner + ")"
			if known[key] == 0 {
				owners = append(owners, key)
			}
			known[key]++
			fmt.Fprintf(w, "%s: %s: known failing, owned by %s (%s): %s%s\n", f.where(), f.Rule, f.Known.Owner,
				f.Known.Anchor, f.Message, r.fp(f))
		default:
			failing++
			fmt.Fprintf(w, "%s: %s: %s%s\n", f.where(), f.Rule, f.Message, r.fp(f))
		}
	}
	sort.Strings(owners)
	var ks []string
	for _, o := range owners {
		ks = append(ks, fmt.Sprintf("%s x%d", o, known[o]))
	}
	kt := "none"
	if len(ks) > 0 {
		kt = strings.Join(ks, ", ")
	}
	fmt.Fprintf(w, "helios-conformance: %d rules over %d files: %d failing, %d suppressed; known failing: %s\n",
		len(r.Rules), r.Files, failing, suppressed, kt)
}

func (r *Result) fp(f Finding) string {
	if !r.ShowFingerprints {
		return ""
	}
	return " [fingerprint " + f.Fingerprint + "]"
}
