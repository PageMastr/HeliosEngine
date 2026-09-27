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
	avxFlagRE    = regexp.MustCompile(`(?i)(/arch:AVX\w*|-mavx\w*|-mbmi\w*|-mf16c|-mlzcnt|-mfma\b)`)
	marchRE      = regexp.MustCompile(`-march=([A-Za-z0-9_.-]+)`)
	varRefRE     = regexp.MustCompile(`\$\{([A-Za-z0-9_]+)\}`)
	langFlagsRE  = regexp.MustCompile(`\bCMAKE_[A-Z]+_FLAGS\w*`)
	grantPropRE  = regexp.MustCompile(`\b(COMPILE_OPTIONS|COMPILE_FLAGS)\b`)
)

// avxFlags returns the AVX-class flags literally in s (a -march above x86-64 counts).
func avxFlags(s string) []string {
	out := avxFlagRE.FindAllString(s, -1)
	for _, m := range marchRE.FindAllStringSubmatch(s, -1) {
		if m[1] != "x86-64" {
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

// cmakeCommands splits a CMake file into commands, with `#` comments removed (quoted `#` kept).
func cmakeCommands(lines []string) []cmakeCmd {
	var out []cmakeCmd
	var cur *cmakeCmd
	depth := 0
	for i, raw := range lines {
		l, inQ := "", false
		for j := 0; j < len(raw); j++ {
			c := raw[j]
			if c == '\\' && j+1 < len(raw) {
				l += raw[j : j+2]
				j++
				continue
			}
			if c == '"' {
				inQ = !inQ
			}
			if c == '#' && !inQ {
				break
			}
			l += string(c)
		}
		if cur == nil {
			m := cmakeCmdRE.FindStringSubmatchIndex(l)
			if m == nil {
				continue
			}
			cur = &cmakeCmd{name: strings.ToLower(l[m[2]:m[3]]), line: i + 1}
			l = l[m[1]:]
			depth = 1
		}
		for j := 0; j < len(l) && cur != nil; j++ {
			switch l[j] {
			case '(':
				depth++
			case ')':
				if depth--; depth == 0 {
					cur.args += " " + l[:j]
					out = append(out, *cur)
					cur = nil
					if rest := l[j+1:]; cmakeCmdRE.MatchString(rest) { // two commands on one line
						sub := cmakeCommands([]string{rest})
						for k := range sub {
							sub[k].line = i + 1
						}
						out = append(out, sub...)
					}
				}
			}
		}
		if cur != nil {
			cur.args += " " + l
		}
	}
	return out
}

func checkISAGrants(p *Pass) {
	for _, f := range p.Files {
		levelSets := f == isaLevelSets
		for i, l := range p.Tree.Lines(f) {
			code := strings.SplitN(l, "#", 2)[0]
			if m := avxListRE.FindString(code); m != "" {
				p.Report(f, i+1, "%s is a per-target or per-file AVX2 list: images are built at one level "+
					"(ADR-011 amendment, 02 §1.1)", m)
			}
		}
		avxVars := map[string]bool{} // variables holding AVX-class flags, per function
		for _, c := range cmakeCommands(p.Tree.Lines(f)) {
			fields := strings.Fields(c.args)
			first := ""
			if len(fields) > 0 {
				first = strings.Trim(fields[0], `"`)
			}
			flags := avxFlags(c.args)
			for _, v := range varRefRE.FindAllStringSubmatch(c.args, -1) {
				if avxVars[v[1]] {
					flags = append(flags, "${"+v[1]+"}")
				}
			}
			switch c.name {
			case "function", "macro", "endfunction", "endmacro":
				avxVars = map[string]bool{}
				if first == "helios_avx2_sources" {
					p.Report(f, c.line, "helios_avx2_sources() grants AVX2 to single files: images are built at one "+
						"level (ADR-011 amendment, 02 §1.1)")
				}
				continue
			case "helios_avx2_sources":
				p.Report(f, c.line, "helios_avx2_sources() grants AVX2 to single files: images are built at one level "+
					"(ADR-011 amendment, 02 §1.1)")
				continue
			case "helios_isa_avx2_flags":
				avxVars[first] = true
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
				if langFlagsRE.MatchString(name) && len(flags) > 0 && !levelSets {
					p.Report(f, c.line, "%s carries %s: ISA flags come only from the image level (02 §1.1)", name,
						strings.Join(flags, " "))
				}
				continue
			}
			if len(flags) == 0 {
				continue
			}
			perFile := c.name == "set_source_files_properties" ||
				c.name == "set_property" && strings.EqualFold(first, "SOURCE")
			perTarget := c.name == "target_compile_options" || c.name == "add_compile_options" ||
				c.name == "set_target_properties" || c.name == "set_property" && strings.EqualFold(first, "TARGET")
			switch {
			case perFile && (grantPropRE.MatchString(c.args) || c.name == "set_source_files_properties"):
				p.Report(f, c.line, "%s gives single files %s: images are built at one level (02 §1.1)", c.name,
					strings.Join(flags, " "))
			case perTarget && !levelSets && (c.name != "set_target_properties" && c.name != "set_property" ||
				grantPropRE.MatchString(c.args)):
				p.Report(f, c.line, "%s gives %s %s below the image level: only %s's level sets carry ISA flags "+
					"(02 §1.1)", c.name, first, strings.Join(flags, " "), isaLevelSets)
			}
		}
	}
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
			for _, c := range cmakeCommands(p.Tree.Lines(f)) {
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
		for _, c := range cmakeCommands(raw) {
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

func gateDefinition(p *Pass, f string, line int, stmt string, body bool) {
	s := strings.TrimSpace(stripAttrGroups(stmt))
	if s == "" || regexp.MustCompile(`^(static|typedef|struct|enum|union|extern)\b`).MatchString(s) {
		return
	}
	decl := s
	if i := strings.Index(decl, "="); i >= 0 {
		decl = decl[:i]
	} else if !body && strings.Contains(decl, "(") && !strings.Contains(decl, "(*") {
		return // a prototype: a reference, not a definition
	}
	for trailing := regexp.MustCompile(`\([^()]*\)\s*$`); trailing.MatchString(decl); {
		decl = trailing.ReplaceAllString(decl, "")
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
	if name != "" && !gateExports[name] {
		p.Report(f, line, "external symbol %s in a gate TU: only helios_cpu_gate_run, helios_cpu_gate_verdict and "+
			"helios_cpu_gate_tls_entry may be external (make it static) (02 §1.1)", name)
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
