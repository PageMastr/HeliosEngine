package conformance

import (
	"regexp"
	"sort"
	"strings"
)

var registry = map[string]*Rule{}

func register(r *Rule) *Rule {
	registry[r.ID] = r
	return r
}

// All returns every rule, ordered by ID.
func All() []*Rule {
	out := make([]*Rule, 0, len(registry))
	for _, r := range registry {
		out = append(out, r)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// Lookup returns the rule with the given ID, or nil.
func Lookup(id string) *Rule { return registry[id] }

// codeLines returns C, C++ or Go source lines with comments blanked and, when blankStrings is set,
// the contents of string and character literals blanked too (the quotes stay), so a pattern sees
// only code. Lines and columns are preserved. C++ raw strings and digit separators are understood;
// macros are not expanded.
func codeLines(lines []string, blankStrings bool) []string {
	src := []byte(strings.Join(lines, "\n"))
	out := make([]byte, len(src))
	copy(out, src)
	blank := func(i int) {
		if out[i] != '\n' {
			out[i] = ' '
		}
	}
	for i := 0; i < len(src); i++ {
		c := src[i]
		switch {
		case c == '/' && i+1 < len(src) && src[i+1] == '/':
			for ; i < len(src) && src[i] != '\n'; i++ {
				blank(i)
			}
		case c == '/' && i+1 < len(src) && src[i+1] == '*':
			blank(i)
			blank(i + 1)
			for i += 2; i < len(src) && !(src[i] == '*' && i+1 < len(src) && src[i+1] == '/'); i++ {
				blank(i)
			}
			if i+1 < len(src) {
				blank(i)
				blank(i + 1)
				i++
			}
		case c == 'R' && i+1 < len(src) && src[i+1] == '"' && (i == 0 || !isIdent(src[i-1]) || isPrefix(src, i)):
			open := strings.IndexByte(string(src[i+2:]), '(')
			if open < 0 || open > 16 {
				continue
			}
			body := i + 2 + open + 1
			stop := len(src) // unterminated: the rest of the file is the literal
			if j := strings.Index(string(src[body:]), ")"+string(src[i+2:i+2+open])+`"`); j >= 0 {
				stop = body + j + open + 2
			}
			if blankStrings {
				for k := i + 2; k < stop-1; k++ {
					blank(k)
				}
			}
			i = stop - 1
		case c == '"' || (c == '\'' && !(i > 0 && isHexDigitSep(src, i))):
			for i++; i < len(src) && src[i] != c && src[i] != '\n'; i++ {
				if src[i] == '\\' && i+1 < len(src) {
					if blankStrings {
						blank(i)
					}
					i++
				}
				if blankStrings {
					blank(i)
				}
			}
		}
	}
	return strings.Split(string(out), "\n")
}

func isIdent(c byte) bool {
	return c == '_' || c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z'
}

// isPrefix reports an encoding prefix (u8R, uR, UR, LR) before a raw string at i.
func isPrefix(src []byte, i int) bool {
	j := i
	for j > 0 && isIdent(src[j-1]) {
		j--
	}
	p := string(src[j:i])
	return p == "u8" || p == "u" || p == "U" || p == "L"
}

// isHexDigitSep reports a C++14 digit separator: a quote between two digits of a number.
func isHexDigitSep(src []byte, i int) bool {
	if i+1 >= len(src) || !isIdent(src[i-1]) || !isIdent(src[i+1]) {
		return false
	}
	j := i - 1
	for j > 0 && isIdent(src[j-1]) {
		j--
	}
	return src[j] >= '0' && src[j] <= '9'
}

var cFamily = regexp.MustCompile(`\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp|m|mm)$`)
