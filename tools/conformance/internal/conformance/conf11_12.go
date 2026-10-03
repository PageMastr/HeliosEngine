package conformance

import (
	"path"
	"regexp"
	"strconv"
	"strings"
)

// CONF-11 (ADR-011 amendment; 02 §1.1; reconciliation #25): whole images are built at one ISA level,
// so AVX-class flags are never granted to a target or a file below the image level.
var _ = register(&Rule{
	ID:     "CONF-11",
	Anchor: "ADR-011 amendment; 02 §1.1; reconciliation #25",
	Title:  "ISA flags granted below the image level (per-target or per-file AVX lists, helios_avx2_sources, AVX options)",
	Scope:  []string{"CMakeLists.txt", "**/CMakeLists.txt", "cmake/**", "tools/**/*.cmake", "third_party/CMakeLists.txt"},
	Types:  regexp.MustCompile(`(^|/)CMakeLists\.txt$|\.cmake$`),
	Check:  checkISAGrants,
})

// isaLevelSets is where 02 §1.1's level sets live; its set() and target_compile_options() apply a level.
const isaLevelSets = "cmake/HeliosIsa.cmake"

var (
	cmakeCmdRE   = regexp.MustCompile(`^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(`)
	avxListRE    = regexp.MustCompile(`\bHELIOS_ISA_AVX2_(TARGETS|SOURCE_PATTERNS)\b`)
	avxListVarRE = regexp.MustCompile(`(?i)^\w*avx\w*_(targets|sources|source_patterns|patterns|files|kernels|allowlist)$`)
	avxFlagRE    = regexp.MustCompile(`(?i)([/-]arch:AVX\w*|-mavx\w*|-mbmi\w*|-mf16c|-mlzcnt|-mfma\b)`)
	marchRE      = regexp.MustCompile(`-march=([A-Za-z0-9_.-]+)`)
	langFlagsRE  = regexp.MustCompile(`\bCMAKE_[A-Z]+_FLAGS\w*`)
	// CMAKE_REQUIRED_FLAGS only feeds the check_* try-compile probes; no target is built with it.
	probeFlagsRE = regexp.MustCompile(`^CMAKE_REQUIRED_FLAGS$`)
	grantPropRE  = regexp.MustCompile(`\b(?:INTERFACE_)?(COMPILE_OPTIONS|COMPILE_FLAGS)\b`)
)

// avxFlags returns the AVX-class flags literally in s (a -march above x86-64, which x86-64-v1 names
// too, counts).
func avxFlags(s string) []string {
	out := avxFlagRE.FindAllString(s, -1)
	for _, m := range marchRE.FindAllStringSubmatch(s, -1) {
		if m[1] != "x86-64" && m[1] != "x86-64-v1" {
			out = append(out, m[0])
		}
	}
	return out
}

type cmakeCmd struct {
	name string // lower case
	args string // comment-free, one line
	line int
}

var (
	cmakeIdentRE   = regexp.MustCompile(`^([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(`)
	cmakeBracketRE = regexp.MustCompile(`^\[(=*)\[`)
)

// cmakeCommands splits a CMake file into commands as CMake's lexer reads it: `#` line comments and
// `#[[…]]` bracket comments are dropped; a quoted argument (with `\` escapes, over several lines), a
// bracket argument (`[=[…]=]`) and an escaped parenthesis (`\(`) do not count toward the parentheses
// that close a command, so a quoted "(" cannot swallow the commands after it. A command still open at
// the end of the file is returned too, and open is its line (0 when every command closes).
func cmakeCommands(lines []string) (out []cmakeCmd, open int) {
	var cur *cmakeCmd
	var args strings.Builder
	depth := 0
	quoted := false  // inside a quoted argument
	closer := ""     // the "]=*]" that ends an open bracket argument or comment
	comment := false // the open bracket is a comment
	argStart := true // the next character starts an argument (a bracket argument can only start one)
	finish := func() {
		cur.args = args.String()
		out = append(out, *cur)
		cur = nil
		args.Reset()
	}
	for i, l := range lines {
		for j := 0; j < len(l); j++ {
			c := l[j]
			switch {
			case closer != "":
				k := strings.Index(l[j:], closer)
				if k < 0 {
					if !comment && cur != nil {
						args.WriteString(l[j:])
					}
					j = len(l)
					continue
				}
				if !comment && cur != nil {
					args.WriteString(l[j : j+k])
				}
				j += k + len(closer) - 1
				closer, argStart = "", false
			case quoted:
				switch {
				case c == '\\' && j+1 < len(l):
					args.WriteString(l[j : j+2])
					j++
				case c == '\\': // a line continuation
				case c == '"':
					args.WriteByte(c)
					quoted = false
				default:
					args.WriteByte(c)
				}
			case c == '#':
				if m := cmakeBracketRE.FindString(l[j+1:]); m != "" {
					closer, comment = "]"+strings.Repeat("=", len(m)-2)+"]", true
					j += len(m)
					continue
				}
				j = len(l)
			case cur == nil:
				if m := cmakeIdentRE.FindStringSubmatch(l[j:]); m != nil && (j == 0 || !wordByte(l[j-1])) {
					cur = &cmakeCmd{name: strings.ToLower(m[1]), line: i + 1}
					depth, argStart = 1, true
					j += len(m[0]) - 1
				}
			case c == '\\' && j+1 < len(l):
				args.WriteString(l[j : j+2])
				j++
				argStart = false
			case c == '"':
				args.WriteByte(c)
				quoted, argStart = true, false
			case c == '[' && argStart:
				if m := cmakeBracketRE.FindString(l[j:]); m != "" {
					closer, comment = "]"+strings.Repeat("=", len(m)-2)+"]", false
					args.WriteByte(' ')
					j += len(m) - 1
					continue
				}
				args.WriteByte(c)
				argStart = false
			case c == '(':
				depth++
				args.WriteByte(c)
				argStart = true
			case c == ')':
				if depth--; depth == 0 {
					finish()
					continue
				}
				args.WriteByte(c)
				argStart = false
			case c == ' ' || c == '\t':
				args.WriteByte(c)
				argStart = true
			default:
				args.WriteByte(c)
				argStart = false
			}
		}
		if cur != nil {
			args.WriteByte(' ')
			if !quoted && closer == "" {
				argStart = true
			}
		}
	}
	if cur != nil {
		open = cur.line
		finish()
	}
	return out, open
}

// cmake returns f's commands; a command still open at the end of the file is a ToolRule finding,
// since what follows it was never read as commands of its own.
func (p *Pass) cmake(f string) []cmakeCmd {
	cmds, open := cmakeCommands(p.Tree.Lines(f))
	if open > 0 {
		p.ReportTool(f, open, "%s cannot read this CMake file: the command that starts here never closes", p.Rule.ID)
	}
	return cmds
}

// isaLevels reads cmake/HeliosIsa.cmake for the names that carry AVX-class flags everywhere: the
// variables it sets from them outside a function, or into the parent scope or the cache (02 §1.1's
// HELIOS_ISA_AVX2), and the functions that return them through output arguments (helios_isa_avx2_flags),
// with those arguments' positions. A call to such a function sets its output like set() does: at file
// scope it defines a level set (helios_isa_avx2_flags(HELIOS_ISA_AVX2)), inside a function a local that a
// later CACHE or PARENT_SCOPE set publishes. The file is read until nothing new is found, so a function
// defined after its use, or one that calls another, is followed. A variable that references the level
// sets, or that such a call fills, holds the flags themselves, in any file.
func isaLevels(t *Tree) (vars map[string]bool, producers map[string]map[int]bool) {
	vars, producers = map[string]bool{}, map[string]map[int]bool{"helios_isa_avx2_flags": {0: true}}
	cmds, _ := cmakeCommands(t.Lines(isaLevelSets))
	for changed := true; changed; {
		changed = false
		local := map[string]bool{}
		fn, params := "", []string(nil)
		for _, c := range cmds {
			fields := strings.Fields(c.args)
			first := ""
			if len(fields) > 0 {
				first = strings.Trim(fields[0], `"`)
			}
			name, carries := "", false
			switch c.name {
			case "function", "macro":
				fn, local, params = first, map[string]bool{}, fields[min(1, len(fields)):]
				continue
			case "endfunction", "endmacro":
				fn, params, local = "", nil, map[string]bool{}
				continue
			case "foreach": // foreach(v IN LISTS <level set>): v holds the flags in the loop
				if len(fields) > 1 && listsCarry(fields[1:], func(v string) bool { return local[v] || vars[v] }) {
					local[first] = true
				}
				continue
			case "set", "list", "string":
				name = first
				if c.name != "set" && len(fields) > 1 {
					name = strings.Trim(fields[1], `"`)
				}
				carries = len(avxFlags(c.args)) > 0
				for _, v := range cmakeRefs(c.args) {
					carries = carries || local[v] || vars[v]
				}
			default:
				for i := range producers[c.name] {
					if i < len(fields) {
						out := strings.Trim(fields[i], `"`)
						changed = setCarrier(out, c.args, fn, params, local, vars, producers) || changed
					}
				}
				continue
			}
			if carries && name != "" {
				changed = setCarrier(name, c.args, fn, params, local, vars, producers) || changed
			}
		}
	}
	return vars, producers
}

// setCarrier records that name (set by a command with args, inside function fn or at file scope) holds
// AVX-class flags, and reports whether that is new to vars or producers.
func setCarrier(name, args, fn string, params []string, local, vars map[string]bool,
	producers map[string]map[int]bool) bool {
	changed := false
	switch {
	case strings.HasPrefix(name, "${") && fn != "": // set(${out} … PARENT_SCOPE): the caller's out
		for i, prm := range params {
			if name == "${"+prm+"}" && !producers[fn][i] {
				if producers[fn] == nil {
					producers[fn] = map[int]bool{}
				}
				producers[fn][i], changed = true, true
			}
		}
	case fn == "" || strings.Contains(args, "PARENT_SCOPE") || strings.Contains(args, "CACHE"):
		if !vars[name] {
			vars[name], changed = true, true
		}
	default:
		local[name] = true
	}
	return changed
}

// cmakeRefsRE matches a variable reference, ${v} or $CACHE{v}.
var cmakeRefsRE = regexp.MustCompile(`\$(?:CACHE)?\{([A-Za-z0-9_]+)\}`)

// cmakeRefs returns the variables that args references.
func cmakeRefs(args string) []string {
	var out []string
	for _, m := range cmakeRefsRE.FindAllStringSubmatch(args, -1) {
		out = append(out, m[1])
	}
	return out
}

// listsCarry reports whether a foreach's lists (the fields after its loop variable: `IN LISTS a b`,
// `IN ITEMS ${a}` or plain items) include a variable for which carries is true.
func listsCarry(fields []string, carries func(string) bool) bool {
	byName := false
	for _, f := range fields {
		f = strings.Trim(f, `"`)
		switch strings.ToUpper(f) {
		case "IN", "ITEMS", "ZIP_LISTS":
			byName = strings.EqualFold(f, "ZIP_LISTS")
			continue
		case "LISTS":
			byName = true
			continue
		}
		if byName && carries(f) {
			return true
		}
		for _, v := range cmakeRefs(f) {
			if carries(v) {
				return true
			}
		}
	}
	return false
}

// levelFunction is the function in cmake/HeliosIsa.cmake that applies an image's level: the one place a
// target receives ISA flags. The exemption is by this exact name; WP-0.2r uses it, or renames it here.
const levelFunction = "helios_apply_isa_level"

func checkISAGrants(p *Pass) {
	levelVars, producers := isaLevels(p.Tree)
	cmds := map[string][]cmakeCmd{}
	wrappers := map[string]bool{} // functions and macros the scanned CMake files define
	for _, f := range p.Files {
		cmds[f] = p.cmake(f)
		for _, c := range cmds[f] {
			if fields := strings.Fields(c.args); (c.name == "function" || c.name == "macro") && len(fields) > 0 {
				wrappers[strings.ToLower(strings.Trim(fields[0], `"`))] = true
			}
		}
	}
	for _, f := range p.Files {
		levelFile := f == isaLevelSets
		for i, l := range p.Tree.Lines(f) {
			code := strings.SplitN(l, "#", 2)[0]
			if m := avxListRE.FindString(code); m != "" {
				p.Report(f, i+1, "%s is a per-target or per-file AVX2 list: images are built at one level "+
					"(ADR-011 amendment, 02 §1.1)", m)
			}
		}
		// Variables holding AVX-class flags: the level sets everywhere, plus each function's own.
		fresh := func() map[string]bool {
			m := map[string]bool{}
			for v := range levelVars {
				m[v] = true
			}
			return m
		}
		avxVars, fn := fresh(), ""
		for _, c := range cmds[f] {
			fields := strings.Fields(c.args)
			first := ""
			if len(fields) > 0 {
				first = strings.Trim(fields[0], `"`)
			}
			flags := avxFlags(c.args)
			for _, v := range cmakeRefs(c.args) {
				if avxVars[v] {
					flags = append(flags, "${"+v+"}")
				}
			}
			// in HeliosIsa.cmake, only the image-level function applies a level
			levelSets := levelFile && fn == levelFunction
			switch c.name {
			case "function", "macro", "endfunction", "endmacro":
				avxVars, fn = fresh(), ""
				if c.name == "function" || c.name == "macro" {
					fn = strings.ToLower(first)
				}
				if first == "helios_avx2_sources" {
					p.Report(f, c.line, "helios_avx2_sources() grants AVX2 to single files: images are built at one "+
						"level (ADR-011 amendment, 02 §1.1)")
				}
				continue
			case "helios_avx2_sources":
				p.Report(f, c.line, "helios_avx2_sources() grants AVX2 to single files: images are built at one level "+
					"(ADR-011 amendment, 02 §1.1)")
				continue
			case "foreach": // foreach(v IN LISTS <flags>): v holds them in the loop
				if len(fields) > 1 && listsCarry(fields[1:], func(v string) bool { return avxVars[v] }) {
					avxVars[first] = true
				}
				continue
			case "set", "list", "string":
				name := first
				if c.name != "set" && len(fields) > 1 {
					name = strings.Trim(fields[1], `"`)
				}
				if len(flags) > 0 {
					avxVars[name] = true
				}
				if avxListVarRE.MatchString(name) && !avxListRE.MatchString(name) {
					p.Report(f, c.line, "%s selects targets or sources for AVX-class flags: images are built at one "+
						"level (ADR-011 amendment, 02 §1.1)", name)
				}
				if langFlagsRE.MatchString(name) && !probeFlagsRE.MatchString(name) && len(flags) > 0 && !levelSets {
					p.Report(f, c.line, "%s carries %s: ISA flags come only from the image level (02 §1.1)", name,
						strings.Join(flags, " "))
				}
				continue
			}
			if outs, ok := producers[c.name]; ok { // helios_isa_avx2_flags(out): out holds the flags
				for i := range outs {
					if i < len(fields) {
						avxVars[strings.Trim(fields[i], `"`)] = true
					}
				}
				continue
			}
			if len(flags) == 0 {
				continue
			}
			perFile := c.name == "set_source_files_properties" ||
				c.name == "set_property" && strings.EqualFold(first, "SOURCE")
			perTarget := c.name == "target_compile_options" || c.name == "add_compile_options" ||
				c.name == "add_definitions" || // a directory-wide grant; CMake passes non -D flags through
				c.name == "set_target_properties" || c.name == "set_property" && strings.EqualFold(first, "TARGET")
			perDir := c.name == "set_directory_properties" || c.name == "set_property" && strings.EqualFold(first, "DIRECTORY")
			switch {
			case perFile && (grantPropRE.MatchString(c.args) || c.name == "set_source_files_properties"):
				p.Report(f, c.line, "%s gives single files %s: images are built at one level (02 §1.1)", c.name,
					strings.Join(flags, " "))
			case perTarget && !levelSets && (c.name != "set_target_properties" && c.name != "set_property" ||
				grantPropRE.MatchString(c.args)):
				who := first
				if c.name == "add_compile_options" || c.name == "add_definitions" {
					who = "a directory's targets"
				}
				p.Report(f, c.line, "%s gives %s %s below the image level: only %s's level sets carry ISA flags "+
					"(02 §1.1)", c.name, who, strings.Join(flags, " "), isaLevelSets)
			case perDir && !levelSets && grantPropRE.MatchString(c.args):
				p.Report(f, c.line, "%s gives a directory's targets %s below the image level: only %s's level sets "+
					"carry ISA flags (02 §1.1)", c.name, strings.Join(flags, " "), isaLevelSets)
			case wrappers[c.name] && !levelSets:
				// Only arguments that are options: a flag named in a message is not passed on.
				if opts := optionFlags(c.args, avxVars); len(opts) > 0 {
					p.Report(f, c.line, "%s() is passed %s: a wrapper can grant them below the image level, and only "+
						"%s's level sets carry ISA flags (02 §1.1)", c.name, strings.Join(opts, " "), isaLevelSets)
				}
			}
		}
	}
}

// optionFlags returns the AVX-class flags, literal or through a variable in avxVars, among a command's
// arguments that are options: an unquoted argument, or a quoted one whose every word is an option
// (-x, /x), a variable or a generator expression. A quoted sentence that names a flag is not one.
func optionFlags(args string, avxVars map[string]bool) []string {
	var out []string
	for _, a := range cmakeArgs(args) {
		if strings.HasPrefix(a, `"`) {
			a = strings.Trim(a, `"`)
			for _, w := range strings.Fields(a) {
				if !strings.HasPrefix(w, "-") && !strings.HasPrefix(w, "/") && !strings.HasPrefix(w, "$") {
					a = ""
					break
				}
			}
		}
		out = append(out, avxFlags(a)...)
		for _, v := range cmakeRefs(a) {
			if avxVars[v] {
				out = append(out, "${"+v+"}")
			}
		}
	}
	return out
}

// cmakeArgs splits a command's arguments at whitespace outside quotes (quoted ones keep their quotes).
func cmakeArgs(args string) []string {
	var out []string
	cur, quoted := strings.Builder{}, false
	for i := 0; i < len(args); i++ {
		c := args[i]
		switch {
		case c == '\\' && i+1 < len(args):
			cur.WriteString(args[i : i+2])
			i++
			continue
		case c == '"':
			quoted = !quoted
		case !quoted && (c == ' ' || c == '\t'):
			if cur.Len() > 0 {
				out = append(out, cur.String())
				cur.Reset()
			}
			continue
		}
		cur.WriteByte(c)
	}
	if cur.Len() > 0 {
		out = append(out, cur.String())
	}
	return out
}

// CONF-12 (02 §1.1 gate placement and gate-TU rules; reconciliation #25).
var _ = register(&Rule{
	ID:     "CONF-12",
	Anchor: "02 §1.1 (gate placement and gate-TU rules); reconciliation #25",
	Title:  "A misplaced CPU gate or gate-TU rule break; a pre-gate hook anywhere else",
	Scope:  []string{"engine/**", "gems/**", "apps/**", "game/**", "third_party/CMakeLists.txt", "third_party/*/patches/**"},
	Types:  regexp.MustCompile(cFamily.String() + `|\.patch$|(^|/)CMakeLists\.txt$`),
	Check:  checkGate,
})

var (
	gateFiles    = []string{"engine/core/src/cpugate/**", "engine/core/src/platform/*/cpu_gate_hook.c"}
	gateIncludes = map[string]bool{"cpu_gate.h": true, "stdint.h": true, "intrin.h": true, "cpuid.h": true,
		"windows.h": true, "signal.h": true, "unistd.h": true}
	gateExports = map[string]bool{"helios_cpu_gate_run": true, "helios_cpu_gate_verdict": true,
		"helios_cpu_gate_tls_entry": true}
	crtSectionRE  = regexp.MustCompile(`\.CRT\$X[A-Z0-9]*`)
	initSegRE     = regexp.MustCompile(`#\s*pragma\s+init_seg\b`)
	includeRE     = regexp.MustCompile(`^\s*#\s*include\s*[<"]([^>"]+)[>"]`)
	exitProcessRE = regexp.MustCompile(`\bExitProcess\s*\(`)
	tlsUsedRE     = regexp.MustCompile(`(?i)/INCLUDE:_?_tls_used\b`)
	tlsEntryRE    = regexp.MustCompile(`(?i)/INCLUDE:_?helios_cpu_gate_tls_entry\b`)
	preinitRE     = regexp.MustCompile(`\.preinit_array\b`)
	ctorRE        = regexp.MustCompile(`\b(?:constructor|init_priority)\s*\(\s*(\d+)\s*\)`)
	dispatchRE    = regexp.MustCompile(`\b(ifunc|target_clones)\s*\(`)
	attrGroupRE   = regexp.MustCompile(`__(?:declspec|attribute__)\s*\(`)
	externCRE     = regexp.MustCompile(`^extern\s*"C"$`)
	aggregateRE   = regexp.MustCompile(`^(typedef\b|(struct|enum|union)\b)`)
	cKeywords     = map[string]bool{"static": true, "const": true, "volatile": true, "extern": true, "inline": true,
		"int": true, "void": true, "char": true, "unsigned": true, "signed": true, "long": true, "short": true,
		"__cdecl": true, "__stdcall": true, "WINAPI": true, "NTAPI": true, "PIMAGE_TLS_CALLBACK": true, "struct": true,
		"uint32_t": true, "uint64_t": true, "int32_t": true, "BOOL": true, "DWORD": true, "LONG": true}
)

func checkGate(p *Pass) {
	var winHook string
	linkerText := ""
	for _, f := range p.Tree.Files { // the /INCLUDE: options may live in the hook or in the build (not in comments)
		if strings.HasPrefix(f, "cmake/") || path.Base(f) == "CMakeLists.txt" && strings.HasPrefix(f, "engine/") {
			cmds, _ := cmakeCommands(p.Tree.Lines(f))
			for _, c := range cmds {
				linkerText += c.args + "\n"
			}
		}
	}
	for _, f := range p.Files {
		lines := gateScanLines(p, f)
		gate := MatchAny(gateFiles, f)
		if gate && strings.Contains(f, "/win32/") {
			winHook = f
			linkerText += strings.Join(lines, "\n")
		}
		for i, l := range lines {
			if gate {
				checkGateLine(p, f, i+1, l)
				continue
			}
			for _, s := range crtSectionRE.FindAllString(l, -1) {
				if strings.HasPrefix(s, ".CRT$XLA") {
					p.Report(f, i+1, "a %s contribution: only the CPU gate runs in the first TLS-callback slot (02 §1.1)", s)
				}
			}
			if preinitRE.MatchString(l) {
				p.Report(f, i+1, "a .preinit_array entry: only the CPU gate runs before C initializers (02 §1.1)")
			}
			for _, m := range ctorRE.FindAllStringSubmatch(l, -1) {
				if n, _ := strconv.Atoi(m[1]); n < 101 {
					p.Report(f, i+1, "a pre-initializer priority %d (< 101) runs with the CPU gate (02 §1.1)", n)
				}
			}
			if m := dispatchRE.FindStringSubmatch(l); m != nil {
				p.Report(f, i+1, "%s: resolvers run before the CPU gate (02 §1.1)", m[1])
			}
		}
		if gate && cFamily.MatchString(f) {
			checkGateSymbols(p, f, lines)
		}
	}
	if winHook != "" {
		if !tlsUsedRE.MatchString(linkerText) {
			p.Report(winHook, 0, "missing /INCLUDE:_tls_used: the gate's TLS callback needs the TLS directory (02 §1.1)")
		}
		if !tlsEntryRE.MatchString(linkerText) {
			p.Report(winHook, 0, "missing /INCLUDE:helios_cpu_gate_tls_entry: the linker would drop the gate's "+
				".CRT$XLA0 entry (02 §1.1)")
		}
	}
}

// gateScanLines returns f's code lines, strings kept: C comments removed; for CMake, `#` comments; for a
// patch, only its added lines (context and removed lines are the vendored code, not Helios's).
func gateScanLines(p *Pass, f string) []string {
	raw := p.Tree.Lines(f)
	switch {
	case strings.HasSuffix(f, ".patch"):
		out := make([]string, len(raw))
		for i, l := range raw {
			if strings.HasPrefix(l, "+") && !strings.HasPrefix(l, "+++") {
				out[i] = l[1:]
			}
		}
		return codeLines(out, false)
	case path.Base(f) == "CMakeLists.txt":
		out := make([]string, len(raw))
		for _, c := range p.cmake(f) {
			out[c.line-1] += " " + c.args
		}
		return out
	}
	return codeLines(raw, false)
}

func checkGateLine(p *Pass, f string, line int, l string) {
	for _, s := range crtSectionRE.FindAllString(l, -1) {
		if s != ".CRT$XLA0" {
			p.Report(f, line, "gate entry in %s: the gate is the first TLS callback, .CRT$XLA0 (02 §1.1)", s)
		}
	}
	if initSegRE.MatchString(l) {
		p.Report(f, line, "#pragma init_seg: the gate is the first TLS callback, .CRT$XLA0 (02 §1.1)")
	}
	if exitProcessRE.MatchString(l) {
		p.Report(f, line, "ExitProcess runs DLL detach hooks built at avx2; the gate ends with TerminateProcess (02 §1.1)")
	}
	if m := includeRE.FindStringSubmatch(l); m != nil && !gateIncludes[path.Base(m[1])] {
		p.Report(f, line, "#include <%s> in a gate TU: only cpu_gate.h, <stdint.h>, the CPUID headers and the hooks' "+
			"<windows.h>, <signal.h> and <unistd.h> (02 §1.1)", m[1])
	}
}

// checkGateSymbols reports file-scope definitions without `static` other than the three gate exports.
// The scan is textual: statements at file scope (inside an `extern "C" {` block too), with preprocessor
// lines skipped; an aggregate's `typedef struct {…} Name;` and function bodies are not definitions of
// their own.
func checkGateSymbols(p *Pass, f string, lines []string) {
	depth, base, stmt, start, kind := 0, 0, "", 0, ""
	cont := false
	for i, l := range lines {
		t := strings.TrimSpace(l)
		if cont || strings.HasPrefix(t, "#") {
			cont = strings.HasSuffix(t, "\\")
			continue
		}
		for j := 0; j < len(l); j++ {
			c := l[j]
			if depth > base { // inside a body, an aggregate or an initializer
				switch c {
				case '{':
					depth++
				case '}':
					if depth--; depth == base && kind == "func" {
						stmt, kind = "", ""
					}
				}
				continue
			}
			switch c {
			case '{':
				head := strings.TrimSpace(stripAttrGroups(stmt))
				switch {
				case externCRE.MatchString(head):
					base++
					stmt = ""
					depth++
					continue
				case kind != "":
				case aggregateRE.MatchString(head):
					kind = "agg"
				case strings.Contains(head, "="):
					kind = "init"
				default:
					gateDefinition(p, f, start, stmt, true)
					kind = "func"
				}
				depth++
			case '}': // closes an extern "C" block
				if base > 0 {
					base--
					depth--
				}
				stmt, kind = "", ""
			case ';':
				if kind == "" || kind == "init" {
					gateDefinition(p, f, start, stmt, false)
				}
				stmt, kind = "", ""
			default:
				if strings.TrimSpace(stmt) == "" && c != ' ' && c != '\t' {
					start = i + 1
				}
				stmt += string(c)
			}
		}
		stmt += " "
	}
}

var (
	gateSkipRE   = regexp.MustCompile(`^(static|typedef|struct|enum|union)\b`)
	gateExternRE = regexp.MustCompile(`^extern\s*(?:"C(?:\+\+)?"\s*)?`)
	trailingRE   = regexp.MustCompile(`\([^()]*\)\s*$`)
)

// gateDefinition reports the external names that a file-scope statement defines: every declarator of a
// variable definition (`int a = 1, b = 2;`), or a function with its body. `extern` makes a statement a
// mere declaration only without a body or an initializer (`extern int x = 5;` defines x).
func gateDefinition(p *Pass, f string, line int, stmt string, body bool) {
	s := strings.TrimSpace(stripAttrGroups(stmt))
	if s == "" || gateSkipRE.MatchString(s) {
		return
	}
	if m := gateExternRE.FindString(s); m != "" {
		if !body && !strings.Contains(s, "=") {
			return // a declaration of something defined elsewhere
		}
		s = s[len(m):]
	}
	decls := []string{s}
	if !body {
		decls = splitTop(s) // the declarators of one statement
	}
	for k, decl := range decls {
		if i := strings.Index(decl, "="); i >= 0 {
			decl = decl[:i]
		} else if !body && strings.Contains(decl, "(") && !strings.Contains(decl, "(*") {
			continue // a prototype: a reference, not a definition
		}
		for trailingRE.MatchString(decl) {
			decl = trailingRE.ReplaceAllString(decl, "")
			if !body || strings.Contains(decl, "(") { // a function's name precedes its (last) parameter list
				break
			}
		}
		name := ""
		for _, id := range cIdentRE.FindAllString(decl, -1) {
			if !cKeywords[id] {
				name = id
			}
		}
		if k > 0 && name == "" {
			continue
		}
		if name != "" && !gateExports[name] {
			p.Report(f, line, "external symbol %s in a gate TU: only helios_cpu_gate_run, helios_cpu_gate_verdict and "+
				"helios_cpu_gate_tls_entry may be external (make it static) (02 §1.1)", name)
		}
	}
}

// stripAttrGroups removes __declspec(…) and __attribute__((…)) groups.
func stripAttrGroups(s string) string {
	for {
		loc := attrGroupRE.FindStringIndex(s)
		if loc == nil {
			return s
		}
		depth, end := 0, len(s)
		for j := loc[1] - 1; j < len(s); j++ {
			if s[j] == '(' {
				depth++
			} else if s[j] == ')' {
				if depth--; depth == 0 {
					end = j + 1
					break
				}
			}
		}
		s = s[:loc[0]] + " " + s[end:]
	}
}
