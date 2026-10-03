package conformance

import (
	"crypto/sha256"
	"encoding/hex"
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

// Known is a known-failing record (tools/conformance/known_failing.jsonc): the findings it pins are
// reported but do not fail the run, because rework WP Owner has them in scope (§5.10.2 D3). A record
// pins each finding by its fingerprint, so a new finding of the same rule in the same file (a new grant
// next to an old one, or an old line that gains a flag) still fails. A pinned finding that is no longer
// reported fails the run too, so a closed gap cannot stay listed.
type Known struct {
	Rule     string        `json:"rule"`
	Owner    string        `json:"owner"`
	Anchor   string        `json:"anchor"`
	Reason   string        `json:"reason"`
	Findings []*KnownEntry `json:"findings"`
	line     int
	hits     int
}

// KnownEntry is one finding a record pins: its file and fingerprint (Finding.Fingerprint; the CLI's
// -fingerprints flag prints them). Each entry covers one finding.
type KnownEntry struct {
	Path        string `json:"path"`
	Fingerprint string `json:"fingerprint"`
	line        int
	used        bool
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
	entries := 0
	for i, k := range doc.KnownFailing {
		k.line = lineOf(file, `"rule"`, i)
		valid := true
		for _, e := range k.Findings {
			e.line = lineOf(file, `"fingerprint"`, entries)
			entries++
			if e.Path == "" || strings.ContainsAny(e.Path, `*?[\`) || strings.HasPrefix(e.Path, "/") ||
				!fingerprintRE.MatchString(e.Fingerprint) {
				valid = false
				bad = append(bad, Finding{Rule: ToolRule, Path: rel, Line: e.line, Message: fmt.Sprintf(
					"record %d (%s): each finding needs a relative file path (no globs) and a %d-hex fingerprint",
					i+1, k.Rule, fingerprintLen)})
			}
		}
		switch {
		case Lookup(k.Rule) == nil:
			bad = append(bad, Finding{Rule: ToolRule, Path: rel, Line: k.line,
				Message: fmt.Sprintf("record %d: unknown rule %q", i+1, k.Rule)})
		case !ownerRE.MatchString(k.Owner) || k.Anchor == "" || k.Reason == "" || len(k.Findings) == 0:
			bad = append(bad, Finding{Rule: ToolRule, Path: rel, Line: k.line,
				Message: fmt.Sprintf("record %d (%s): needs an owner WP (WP-<n>.<m>…), an anchor, a reason and "+
					"the findings it pins", i+1, k.Rule)})
		case valid:
			good = append(good, k) // an invalid record covers nothing
		}
	}
	return good, bad
}

const fingerprintLen = 16

var fingerprintRE = regexp.MustCompile(fmt.Sprintf(`^[0-9a-f]{%d}$`, fingerprintLen))

// fingerprint identifies a finding without its line number, so it survives edits elsewhere in the file:
// the rule, the file, the message and the finding's source line with surrounding whitespace trimmed.
func fingerprint(rule, path, message, source string) string {
	h := sha256.Sum256([]byte(rule + "\x00" + path + "\x00" + message + "\x00" + strings.TrimSpace(source)))
	return hex.EncodeToString(h[:])[:fingerprintLen]
}

// scorecardExit is the scorecard item that carries each known-failing record's gap (README).
const scorecardExit = "EXIT-0.conformance"

// checkScorecard cross-checks the known-failing records against scorecard.jsonc: each record's rule must
// be named in a gap of EXIT-0.conformance whose owner list includes the record's owner, so a gap the
// records hide from the run stays visible in the scorecard. rel is the records file, for findings.
func checkScorecard(file, rel string, known []*Known) []Finding {
	if len(known) == 0 {
		return nil
	}
	data, err := os.ReadFile(file)
	var doc struct {
		Exit []struct {
			ID   string `json:"id"`
			Gaps []struct {
				Clause string `json:"clause"`
				Owner  string `json:"owner"`
			} `json:"gaps"`
		} `json:"exit"`
	}
	if err == nil {
		err = json.Unmarshal([]byte(stripJSONC(string(data))), &doc)
	}
	if err != nil {
		return []Finding{{Rule: ToolRule, Path: "scorecard.jsonc", Line: 1,
			Message: fmt.Sprintf("cannot read the scorecard to check the known-failing records: %v", err)}}
	}
	var bad []Finding
	for _, k := range known {
		linked := false
		for _, e := range doc.Exit {
			if e.ID != scorecardExit {
				continue
			}
			for _, g := range e.Gaps {
				for _, o := range strings.Split(g.Owner, ",") {
					linked = linked || strings.TrimSpace(o) == k.Owner && strings.Contains(g.Clause, k.Rule)
				}
			}
		}
		if !linked {
			bad = append(bad, Finding{Rule: ToolRule, Path: rel, Line: k.line, Message: fmt.Sprintf(
				"record %s (owner %s) has no gap in scorecard.jsonc's %s that names %s with owner %s; add one",
				k.Rule, k.Owner, scorecardExit, k.Rule, k.Owner)})
		}
	}
	return bad
}
