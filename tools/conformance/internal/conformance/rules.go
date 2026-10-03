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

// cFamily matches C, C++ and Objective-C sources and headers, C++20 module units (.cppm, .ccm, .cxxm,
// .ixx) and template-body includes (.tpp).
var cFamily = regexp.MustCompile(`\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp|tpp|m|mm|cppm|ccm|cxxm|ixx)$`)

// spliceLines joins each line that ends in a backslash with the next one, as translation phase 2
// does, so a name split across lines (`SDL_Create\` + `Renderer`) reads whole. origin[i] is the
// 0-based physical line where logical line i starts. Whitespace after the backslash is tolerated,
// as GCC and Clang do.
func spliceLines(lines []string) (logical []string, origin []int) {
	for i := 0; i < len(lines); i++ {
		start, cur := i, lines[i]
		for {
			t := strings.TrimRight(cur, " \t")
			if !strings.HasSuffix(t, "\\") || i+1 >= len(lines) {
				break
			}
			i++
			cur = t[:len(t)-1] + lines[i]
		}
		logical = append(logical, cur)
		origin = append(origin, start)
	}
	return logical, origin
}

var (
	ppCondRE  = regexp.MustCompile(`^\s*#\s*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b(.*)$`)
	ppParenRE = regexp.MustCompile(`[\s()]`)
)

// ppValue evaluates a conditional's expression when it is a literal: -1 for 0 or false, 1 for 1 or
// true, 0 for anything else (unknown, so the group counts as compiled).
func ppValue(expr string) int {
	switch ppParenRE.ReplaceAllString(expr, "") {
	case "0", "false":
		return -1
	case "1", "true":
		return 1
	}
	return 0
}

// inactiveLines marks the lines of preprocessor groups that are never compiled: `#if 0` (or false)
// groups, and the `#elif`/`#else` groups after an `#if 1`. Every other condition is unknown, so its
// groups count as compiled. code must have comments blanked (codeLines), so `#if 0 // off` reads as
// `#if 0`.
func inactiveLines(code []string) []bool {
	type frame struct {
		parent bool // the enclosing group is compiled
		taken  int  // the best branch so far: -1 none, 0 maybe, 1 certainly
		active bool
	}
	var stack []frame
	active := true
	out := make([]bool, len(code))
	for i, l := range code {
		m := ppCondRE.FindStringSubmatch(l)
		if m == nil {
			out[i] = !active
			continue
		}
		switch m[1] {
		case "if", "ifdef", "ifndef":
			v := 0
			if m[1] == "if" {
				v = ppValue(m[2])
			}
			stack = append(stack, frame{parent: active, taken: v, active: active && v >= 0})
		case "elif", "elifdef", "elifndef":
			if len(stack) == 0 {
				break
			}
			f := &stack[len(stack)-1]
			v := 0
			if m[1] == "elif" {
				v = ppValue(m[2])
			}
			f.active = f.parent && f.taken < 1 && v >= 0
			f.taken = max(f.taken, v)
		case "else":
			if len(stack) == 0 {
				break
			}
			f := &stack[len(stack)-1]
			f.active = f.parent && f.taken < 1
			f.taken = 1
		case "endif":
			if len(stack) > 0 {
				stack = stack[:len(stack)-1]
			}
		}
		if len(stack) > 0 {
			active = stack[len(stack)-1].active
		} else {
			active = true
		}
		out[i] = false // a directive line itself is read
	}
	return out
}
