package conformance

import (
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strings"
)

// MapEntry is one anchor of tools/conformance/map.jsonc (§5.10.2 D2): the code that implements a
// normative anchor, the CONF rules that check it and the conformance tests it requires. An entry the
// reviewer accepted as not mechanically checkable (D5) has `why` instead of rules.
type MapEntry struct {
	Paths []string `json:"paths"`
	Rules []string `json:"rules"`
	Tests []string `json:"tests"`
	Why   string   `json:"why"`
	line  int
}

// Known is a known-failing record (tools/conformance/known_failing.jsonc): findings of Rule under
// Paths are reported but do not fail the run, because rework WP Owner has them in scope (§5.10.2 D3).
// A record that matches no finding fails the run, so a closed gap cannot stay listed.
type Known struct {
	Rule   string   `json:"rule"`
	Owner  string   `json:"owner"`
	Anchor string   `json:"anchor"`
	Reason string   `json:"reason"`
	Paths  []string `json:"paths"`
	line   int
	hits   int
}

// stripJSONC removes // and /* */ comments and trailing commas outside strings, keeping line breaks
// so decoder offsets still map to source lines.
func stripJSONC(src string) string {
	var b strings.Builder
	inStr := false
	for i := 0; i < len(src); i++ {
		c := src[i]
		switch {
		case inStr:
			b.WriteByte(c)
			if c == '\\' && i+1 < len(src) {
				i++
				b.WriteByte(src[i])
			} else if c == '"' {
				inStr = false
			}
		case c == '"':
			inStr = true
			b.WriteByte(c)
		case c == '/' && i+1 < len(src) && src[i+1] == '/':
			for i < len(src) && src[i] != '\n' {
				i++
			}
			if i < len(src) {
				b.WriteByte('\n')
			}
		case c == '/' && i+1 < len(src) && src[i+1] == '*':
			for i += 2; i+1 < len(src) && !(src[i] == '*' && src[i+1] == '/'); i++ {
				if src[i] == '\n' {
					b.WriteByte('\n')
				}
			}
			i++
		default:
			b.WriteByte(c)
		}
	}
	return regexp.MustCompile(`,(\s*[\]}])`).ReplaceAllString(b.String(), "$1")
}

// decodeJSONC decodes a JSONC file strictly (unknown fields fail).
func decodeJSONC(file string, v any) error {
	data, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	dec := json.NewDecoder(strings.NewReader(stripJSONC(string(data))))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return fmt.Errorf("%s: %v", file, err)
	}
	return nil
}

// lineOf returns the 1-based line of the n-th (0-based) occurrence of needle in file, or 1.
func lineOf(file, needle string, n int) int {
	data, _ := os.ReadFile(file)
	text, off := string(data), 0
	for ; n >= 0; n-- {
		i := strings.Index(text[off:], needle)
		if i < 0 {
			return 1
		}
		off += i + len(needle)
	}
	return strings.Count(text[:off], "\n") + 1
}

var (
	anchorRE = regexp.MustCompile(`^(ADR-\d{3}[a-z]?|\d{2} §\d+(\.\d+)*[a-z]?|[A-Z][A-Z0-9]*(-[A-Z0-9]+)*-\d[\d.]*)`)
	ownerRE  = regexp.MustCompile(`^WP-\d+\.\d+[a-z0-9]*$`)
)

// loadMap reads and validates the anchor map; each of the selected rules must be in an entry.
// Problems are returned as findings of the map file.
func loadMap(file, rel string, selected []*Rule) (map[string]*MapEntry, []Finding) {
	var m map[string]*MapEntry
	if err := decodeJSONC(file, &m); err != nil {
		return nil, []Finding{{Rule: ToolRule, Path: rel, Line: 1, Message: err.Error()}}
	}
	var bad []Finding
	report := func(line int, format string, args ...any) {
		bad = append(bad, Finding{Rule: ToolRule, Path: rel, Line: line, Message: fmt.Sprintf(format, args...)})
	}
	byID := map[string]bool{}
	for _, r := range All() {
		byID[r.ID] = true
	}
	mapped := map[string]bool{}
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, anchor := range keys {
		e := m[anchor]
		e.line = lineOf(file, `"`+anchor+`"`, 0)
		if !anchorRE.MatchString(anchor) {
			report(e.line, "map key %q is not an anchor (an ADR ID, a criterion ID or `<section> §<n>`, §5.10.1)", anchor)
		}
		if len(e.Rules) == 0 && e.Why == "" {
			report(e.line, "anchor %q names no rule and gives no `why` (§5.10.2 D5)", anchor)
		}
		for _, id := range e.Rules {
			if !byID[id] {
				report(e.line, "anchor %q names unknown rule %s", anchor, id)
			}
			mapped[id] = true
		}
		for _, g := range e.Paths {
			if g == "" || strings.HasPrefix(g, "/") || strings.Contains(g, `\`) || strings.Contains(g, "..") {
				report(e.line, "anchor %q: path %q must be a relative slash glob", anchor, g)
			}
		}
	}
	for _, r := range selected {
		if !mapped[r.ID] {
			report(1, "%s (%s) is in no map entry (§5.10.2 D2)", r.ID, r.Anchor)
		}
	}
	return m, bad
}

// loadKnown reads and validates the known-failing records.
func loadKnown(file, rel string) ([]*Known, []Finding) {
	var doc struct {
		KnownFailing []*Known `json:"known_failing"`
	}
	if err := decodeJSONC(file, &doc); err != nil {
		return nil, []Finding{{Rule: ToolRule, Path: rel, Line: 1, Message: err.Error()}}
	}
	var good []*Known
	var bad []Finding
	for i, k := range doc.KnownFailing {
		k.line = lineOf(file, `"rule"`, i)
		switch {
		case Lookup(k.Rule) == nil:
			bad = append(bad, Finding{Rule: ToolRule, Path: rel, Line: k.line,
				Message: fmt.Sprintf("record %d: unknown rule %q", i+1, k.Rule)})
		case !ownerRE.MatchString(k.Owner) || k.Anchor == "" || k.Reason == "" || len(k.Paths) == 0:
			bad = append(bad, Finding{Rule: ToolRule, Path: rel, Line: k.line,
				Message: fmt.Sprintf("record %d (%s): needs an owner WP (WP-<n>.<m>…), an anchor, a reason and paths",
					i+1, k.Rule)})
		default:
			good = append(good, k) // an invalid record covers nothing
		}
	}
	return good, bad
}
