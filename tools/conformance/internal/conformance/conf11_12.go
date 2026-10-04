package conformance

import (
	"maps"
	"path"
	"regexp"
	"slices"
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
	// An ISA level CONF-11 cannot tell: a variable or generator expression in the value of -march=, /arch:
	// or -m (-march=${level}, /arch:${v}, -march=$<IF:…>, -march=x86-64${suffix}, -m${ext}). It fails closed.
	computedISARE = regexp.MustCompile(`(?i)(?:-march=|[/-]arch:)[A-Za-z0-9_.-]*\$[{<][^\s"]*|-m\$[{<][^\s"]*`)
	// $ENV{X} in a grant: its value comes from outside the build files, so it fails closed.
	envRefRE = regexp.MustCompile(`\$ENV\{[^}]*\}`)
	// Variables that reach the compiler: CMake's own (CMAKE_<LANG>_FLAGS*, CMAKE_<LANG>_COMPILE_OBJECT, …)
	// and the environment (ENV{CXXFLAGS} seeds CMAKE_CXX_FLAGS when a language is enabled).
	compilerVarRE = regexp.MustCompile(`^(?:CMAKE_\w+|ENV\{\w+\})$`)
	// CMAKE_REQUIRED_* only feed the check_* try-compile probes; no target is built with them.
	probeFlagsRE = regexp.MustCompile(`^CMAKE_REQUIRED_\w+$`)
	grantPropRE  = regexp.MustCompile(`\b(?:INTERFACE_)?(COMPILE_OPTIONS|COMPILE_FLAGS)\b`)
	// A nested reference, ${${name}}: the variable it reads is computed, so CONF-11 cannot tell its value.
	nestedRefRE = regexp.MustCompile(`\$\{[^{}]*\$\{[^{}]*\}[^{}]*\}`)
	// Commands that may name a flag without passing it to a compiler: a message, a condition, a compiler
	// probe, argument parsing and the end of a definition or loop.
	isaInertCmds = map[string]bool{"message": true, "if": true, "elseif": true, "else": true, "endif": true,
		"while": true, "endwhile": true, "check_c_compiler_flag": true, "check_cxx_compiler_flag": true,
		"check_compiler_flag": true, "cmake_parse_arguments": true, "return": true}
)

// avxFlags returns the AVX-class flags literally in s (a -march above x86-64, which x86-64-v1 names
// too, counts), and the ISA levels whose value it cannot tell (computedISARE).
func avxFlags(s string) []string {
	out := avxFlagRE.FindAllString(s, -1)
	for _, m := range marchRE.FindAllStringSubmatchIndex(s, -1) {
		if v := s[m[2]:m[3]]; v != "x86-64" && v != "x86-64-v1" && !strings.HasPrefix(s[m[1]:], "$") {
			out = append(out, s[m[0]:m[1]])
		}
	}
	return append(out, computedISARE.FindAllString(s, -1)...)
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
// variables it sets from them outside a function (a macro body counts as outside: it writes its caller's
// scope), or into the parent scope or the cache (02 §1.1's HELIOS_ISA_AVX2), and the functions that return them through output arguments (helios_isa_avx2_flags),
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
		fn, params, macro := "", []string(nil), false
		for _, c := range cmds {
			fields := strings.Fields(c.args)
			first := ""
			if len(fields) > 0 {
				first = strings.Trim(fields[0], `"`)
			}
			switch c.name {
			case "function", "macro":
				fn, local, params = strings.ToLower(first), map[string]bool{}, fields[min(1, len(fields)):]
				macro = c.name == "macro"
				continue
			case "endfunction", "endmacro":
				fn, params, local, macro = "", nil, map[string]bool{}, false
				continue
			case "foreach": // foreach(v IN LISTS <level set>) or foreach(v -mavx2 …): v holds flags in the loop
				_, carriers := loopVars(c.args, fields, func(v string) bool { return local[v] || vars[v] })
				for _, v := range carriers {
					local[v] = true
				}
				continue
			case "set", "list", "string":
				names, read := writtenVar(c.name, cmakeArgs(c.args))
				carries := len(avxFlags(read)) > 0
				for _, v := range cmakeRefs(read) {
					carries = carries || local[v] || vars[v]
				}
				for _, name := range names {
					if carries && name != "" {
						changed = setCarrier(name, c.args, fn, macro, params, local, vars, producers) || changed
					}
				}
			default:
				for i := range producers[c.name] {
					if i < len(fields) {
						out := strings.Trim(fields[i], `"`)
						changed = setCarrier(out, c.args, fn, macro, params, local, vars, producers) || changed
					}
				}
			}
		}
	}
	return vars, producers
}

// setCarrier records that name (set by a command with args, inside function or macro fn, or at file scope)
// holds AVX-class flags, and reports whether that is new to vars or producers. A macro body writes its
// caller's scope, which the scan takes to be the file's: a macro called at file scope defines a level set.
func setCarrier(name, args, fn string, macro bool, params []string, local, vars map[string]bool,
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
	case fn == "" || macro || strings.Contains(args, "PARENT_SCOPE") || strings.Contains(args, "CACHE"):
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

// loopCarries reports whether a foreach's loop variable (the first of args, split into fields) takes
// AVX-class flags: an item that is an option names one (foreach(f -mavx2 -mfma), foreach(f IN ITEMS
// -mavx2); a quoted sentence that names a flag, such as a test's expected message, is not one), or its
// lists or items reference a variable for which carries is true.
func loopCarries(args string, fields []string, carries func(string) bool) bool {
	items := cmakeArgs(args)
	return len(fields) > 1 && len(items) > 1 &&
		(len(optionFlags(strings.Join(items[1:], " "), nil)) > 0 || listsCarry(fields[1:], carries))
}

// loopVars returns the variables a foreach sets (all) and those of them that hold AVX-class flags
// (carriers). The loop variable is the first field; with IN ZIP_LISTS, each list sets its own: the i-th of
// several loop variables, or <v>_<i> for a single one (CMake leaves <v> itself unset), and it carries
// flags when its list does.
func loopVars(args string, fields []string, carries func(string) bool) (all, carriers []string) {
	if len(fields) == 0 {
		return nil, nil
	}
	in := slices.IndexFunc(fields, func(f string) bool { return strings.EqualFold(strings.Trim(f, `"`), "IN") })
	if in < 1 || in+1 >= len(fields) || !strings.EqualFold(strings.Trim(fields[in+1], `"`), "ZIP_LISTS") {
		first := strings.Trim(fields[0], `"`)
		if loopCarries(args, fields, carries) {
			carriers = []string{first}
		}
		return []string{first}, carriers
	}
	var names []string
	for _, f := range fields[:in] {
		names = append(names, strings.Trim(f, `"`))
	}
	lists := fields[in+2:]
	for i, l := range lists {
		v := ""
		switch {
		case len(names) == 1:
			v = names[0] + "_" + strconv.Itoa(i)
		case i < len(names):
			v = names[i]
		}
		if v != "" {
			all = append(all, v)
		}
		if listsCarry([]string{"LISTS", l}, carries) {
			if v == "" || len(names) > 1 && len(names) != len(lists) {
				// CMake rejects a count mismatch; which variable takes the list is unknown, so all do.
				carriers = append(carriers, names...)
			} else {
				carriers = append(carriers, v)
			}
		}
	}
	if len(names) > 1 {
		all = names
	}
	return all, carriers
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

// isaFuncs is what CONF-11 learns about the functions and macros of every scanned file (CMake functions are
// global): the output arguments a body fills with flags, the variables it hands its caller (PARENT_SCOPE,
// return(PROPAGATE), or any set() in a macro), and the flag variables in scope where it is called, which
// its body reads (CMake's dynamic scope).
type isaFuncs struct {
	producers  map[string]map[int]bool
	exports    map[string]map[string]bool
	callerVars map[string]map[string]bool
}

// add records v under name in m and reports whether it is new.
func addTo[K comparable](m map[string]map[K]bool, name string, v K) bool {
	if m[name][v] {
		return false
	}
	if m[name] == nil {
		m[name] = map[K]bool{}
	}
	m[name][v] = true
	return true
}

func checkISAGrants(p *Pass) {
	levelVars, producers := isaLevels(p.Tree)
	funcs := &isaFuncs{producers: producers, exports: map[string]map[string]bool{},
		callerVars: map[string]map[string]bool{}}
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
	// A function body runs when it is called, so it reads every variable its file sets, also one set after
	// the definition, and its callers' variables; and a call receives what the body hands back. Passes over
	// every file collect these until nothing new is learnt (the sets only grow, so this ends); the last
	// pass reports.
	// The root CMakeLists.txt and the cmake/*.cmake modules it includes run before every subdirectory, so
	// the flag variables their file scope ends with are seen everywhere, like the level sets. A
	// subdirectory's CMakeLists.txt starts from its ancestors' variables too (add_subdirectory copies the
	// parent's scope; the ancestors' file scope at its end stands in for the point of the call).
	scans := map[string]*isaScan{}
	fileVars := map[string]map[string]bool{}
	globals := maps.Clone(levelVars)
	start := func(f string) map[string]bool {
		vars := maps.Clone(globals)
		if path.Base(f) != "CMakeLists.txt" || f == "CMakeLists.txt" {
			return vars
		}
		for d := path.Dir(path.Dir(f)); ; d = path.Dir(d) {
			for v := range fileVars[path.Join(d, "CMakeLists.txt")] {
				vars[v] = true
			}
			if d == "." {
				return vars
			}
		}
	}
	for _, f := range p.Files {
		scans[f] = &isaScan{p: p, f: f, cmds: cmds[f], funcs: funcs, wrappers: wrappers}
	}
	for changed := true; changed; {
		changed = false
		for _, f := range p.Files {
			scans[f].levelVars = start(f)
			vars := scans[f].run(fileVars[f], false)
			changed = scans[f].changed || len(vars) != len(fileVars[f]) || changed
			fileVars[f] = vars
			if f == "CMakeLists.txt" || path.Dir(f) == "cmake" && strings.HasSuffix(f, ".cmake") {
				for v := range vars {
					if !globals[v] {
						globals[v], changed = true, true
					}
				}
			}
		}
	}
	for _, f := range p.Files {
		for i, l := range p.Tree.Lines(f) {
			code := strings.SplitN(l, "#", 2)[0]
			if m := avxListRE.FindString(code); m != "" {
				p.Report(f, i+1, "%s is a per-target or per-file AVX2 list: images are built at one level "+
					"(ADR-011 amendment, 02 §1.1)", m)
			}
		}
		scans[f].levelVars = start(f)
		scans[f].run(fileVars[f], true)
	}
}

// isaScan reads one CMake file for CONF-11, tracking the variables that hold AVX-class flags per scope.
type isaScan struct {
	p         *Pass
	f         string
	cmds      []cmakeCmd
	levelVars map[string]bool
	funcs     *isaFuncs
	wrappers  map[string]bool
	changed   bool // the last run learnt something new about a function
}

// isaFrame is a scope: the file, or the body of a function or macro being read.
type isaFrame struct {
	vars   map[string]bool // the variables that hold AVX-class flags
	fn     string          // the function or macro, lower case; "" at file scope
	macro  bool
	params []string // the definition's parameters
}

// run reads the file once and returns the variables its file scope ends with. fileVars (from a first
// pass) are visible in every function body; with report false, nothing is reported.
func (s *isaScan) run(fileVars map[string]bool, report bool) map[string]bool {
	p, f := s.p, s.f
	reportf := func(line int, format string, args ...any) {
		if report {
			p.Report(f, line, format, args...)
		}
	}
	levelFile := f == isaLevelSets
	s.changed = false
	fs := s.funcs
	cur := isaFrame{vars: maps.Clone(s.levelVars)}
	var outer []isaFrame // the scopes around the definition being read, the file's first
	// The open foreach loops: each loop's variables, and whether each held flags before the loop (CMake
	// restores their values when the loop ends, CMP0124).
	type loopVar struct {
		name string
		was  bool
	}
	var loops [][]loopVar
	// handBack records that the body being read gives its caller name: its callers then hold the flags
	// there, and so does the scope around the definition, which stands in for them in this file.
	handBack := func(name string) {
		if n := len(outer); n > 0 {
			outer[n-1].vars[name] = true
			s.changed = addTo(fs.exports, cur.fn, name) || s.changed
		}
	}
	// mark records that c writes the flags to name. A set() of ${param} in a body fills its caller's
	// variable (an output argument); a set() in a macro body writes its caller's scope, and so does
	// PARENT_SCOPE in a function; a CACHE entry is seen by the file too. Any other name built at run time
	// (${ARG_OUT}, ${prefix}_FLAGS, ${ARGV0}) is one no reference can be matched to, so it fails closed.
	mark := func(name, args string, c cmakeCmd) {
		if strings.Contains(name, "${") {
			param := false
			for i, prm := range cur.params {
				if cur.fn != "" && name == "${"+prm+"}" {
					s.changed = addTo(fs.producers, cur.fn, i) || s.changed
					param = true
				}
			}
			if !param && !(levelFile && cur.fn == levelFunction) {
				reportf(c.line, "%s() writes AVX-class flags to %s, a name CONF-11 cannot tell: only %s's "+
					"level sets carry ISA flags (02 §1.1)", c.name, name, isaLevelSets)
			}
			return // the caller's variable is marked where the call names it
		}
		cur.vars[name] = true
		if n := len(outer); n > 0 {
			if cur.macro || strings.Contains(args, "PARENT_SCOPE") {
				handBack(name)
			}
			if strings.Contains(args, "CACHE") {
				outer[0].vars[name] = true
			}
		}
	}
	for _, c := range s.cmds {
		fields := strings.Fields(c.args)
		first := ""
		if len(fields) > 0 {
			first = strings.Trim(fields[0], `"`)
		}
		flags := carriedFlags(c.args, cur.vars)
		// in HeliosIsa.cmake, only the image-level function applies a level
		levelSets := levelFile && cur.fn == levelFunction
		switch c.name {
		case "function", "macro":
			// A body reads its caller's variables: the scope around the definition stands in for them, with
			// the file's and those of every call to it found so far.
			body := isaFrame{vars: maps.Clone(cur.vars), fn: strings.ToLower(first), macro: c.name == "macro"}
			for _, prm := range fields[min(1, len(fields)):] {
				body.params = append(body.params, strings.Trim(prm, `"`))
			}
			for v := range fileVars {
				body.vars[v] = true
			}
			for v := range fs.callerVars[body.fn] {
				body.vars[v] = true
			}
			outer, cur = append(outer, cur), body
			if first == "helios_avx2_sources" {
				reportf(c.line, "helios_avx2_sources() grants AVX2 to single files: images are built at one "+
					"level (ADR-011 amendment, 02 §1.1)")
			}
			continue
		case "endfunction", "endmacro":
			if n := len(outer); n > 0 {
				outer, cur = outer[:n-1], outer[n-1]
			}
			continue
		case "helios_avx2_sources":
			reportf(c.line, "helios_avx2_sources() grants AVX2 to single files: images are built at one level "+
				"(ADR-011 amendment, 02 §1.1)")
			continue
		case "foreach": // foreach(v IN LISTS <flags>) or foreach(v -mavx2 …): v holds them in the loop
			all, carriers := loopVars(c.args, fields, func(v string) bool { return cur.vars[v] })
			var saved []loopVar
			for _, v := range all {
				saved = append(saved, loopVar{v, cur.vars[v]})
			}
			loops = append(loops, saved)
			for _, v := range carriers {
				cur.vars[v] = true
			}
			continue
		case "endforeach":
			if n := len(loops); n > 0 {
				for _, lv := range loops[n-1] {
					if !lv.was {
						delete(cur.vars, lv.name)
					}
				}
				loops = loops[:n-1]
			}
			continue
		case "return": // return(PROPAGATE v…) hands v to the caller
			propagate := false
			for _, fl := range fields {
				fl = strings.Trim(fl, `"`)
				if propagate && cur.vars[fl] {
					handBack(fl)
				}
				propagate = propagate || fl == "PROPAGATE"
			}
			continue
		case "set", "list", "string", "separate_arguments":
			names, read := writtenVar(c.name, cmakeArgs(c.args))
			flags = carriedFlags(read, cur.vars)
			for _, name := range names {
				if len(flags) > 0 && name != "" {
					mark(name, c.args, c)
				}
				if avxListVarRE.MatchString(name) && !avxListRE.MatchString(name) {
					reportf(c.line, "%s selects targets or sources for AVX-class flags: images are built at one "+
						"level (ADR-011 amendment, 02 §1.1)", name)
				}
				if compilerVarRE.MatchString(name) && !probeFlagsRE.MatchString(name) && len(flags) > 0 && !levelSets {
					reportf(c.line, "%s carries %s: ISA flags come only from the image level (02 §1.1)", name,
						strings.Join(flags, " "))
				}
			}
			continue
		}
		if s.wrappers[c.name] { // a call: the body reads this scope's flag variables and hands some back
			for v := range cur.vars {
				s.changed = addTo(fs.callerVars, c.name, v) || s.changed
			}
			for v := range fs.exports[c.name] {
				mark(v, "", c)
			}
		}
		if outs, ok := fs.producers[c.name]; ok { // helios_isa_avx2_flags(out): out holds the flags
			for i := range outs {
				if i < len(fields) {
					mark(strings.Trim(fields[i], `"`), "", c)
				}
			}
			continue
		}
		if m := nestedRefRE.FindString(c.args); m != "" && !isaInertCmds[c.name] {
			flags = append(flags, m) // ${${n}}: a value CONF-11 cannot tell, so it fails closed
		}
		// $ENV{X} in a grant: a value from outside the build files, which CONF-11 cannot tell.
		if m := envRefRE.FindString(c.args); m != "" && (c.name == "target_compile_options" ||
			c.name == "add_compile_options" || c.name == "add_definitions" || grantPropRE.MatchString(c.args) &&
			(c.name == "set_property" || c.name == "set_target_properties" || c.name == "set_source_files_properties" ||
				c.name == "set_directory_properties")) {
			flags = append(flags, m)
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
			reportf(c.line, "%s gives single files %s: images are built at one level (02 §1.1)", c.name,
				strings.Join(flags, " "))
		case perTarget && !levelSets && (c.name != "set_target_properties" && c.name != "set_property" ||
			grantPropRE.MatchString(c.args)):
			who := first
			if c.name == "add_compile_options" || c.name == "add_definitions" {
				who = "a directory's targets"
			}
			reportf(c.line, "%s gives %s %s below the image level: only %s's level sets carry ISA flags "+
				"(02 §1.1)", c.name, who, strings.Join(flags, " "), isaLevelSets)
		case perDir && !levelSets && grantPropRE.MatchString(c.args):
			reportf(c.line, "%s gives a directory's targets %s below the image level: only %s's level sets "+
				"carry ISA flags (02 §1.1)", c.name, strings.Join(flags, " "), isaLevelSets)
		case s.wrappers[c.name] && !levelSets:
			// Only arguments that are options: a flag named in a message is not passed on.
			if opts := optionFlags(c.args, cur.vars); len(opts) > 0 {
				reportf(c.line, "%s() is passed %s: a wrapper can grant them below the image level, and only "+
					"%s's level sets carry ISA flags (02 §1.1)", c.name, strings.Join(opts, " "), isaLevelSets)
			}
		case !s.wrappers[c.name] && !isaInertCmds[c.name] && !levelSets:
			// A command CONF-11 does not know (cmake_language(CALL …), a function defined outside the
			// scanned files, a property it does not read such as LINK_OPTIONS, which LTO compiles with) fails
			// closed when it carries flags.
			reportf(c.line, "%s() carries %s: CONF-11 cannot tell whether it grants them below the image "+
				"level; only %s's level sets carry ISA flags (02 §1.1)", c.name, strings.Join(flags, " "),
				isaLevelSets)
		}
	}
	if len(outer) > 0 { // a definition still open at the end of the file
		return outer[0].vars
	}
	return cur.vars
}

// carriedFlags returns the AVX-class flags in args: literal, or through a variable in vars.
func carriedFlags(args string, vars map[string]bool) []string {
	flags := avxFlags(args)
	for _, v := range cmakeRefs(args) {
		if vars[v] {
			flags = append(flags, "${"+v+"}")
		}
	}
	return flags
}

// writtenVar returns the variables that a set(), list() or string() writes, and the text of the arguments
// their value comes from: for string(REPLACE), string(REGEX …) the match pattern is left out (a pattern
// that names a flag removes it), and the output is the argument CMake writes (string(REPLACE <match>
// <replace> <out> <input>) writes its 4th, string(JOIN <glue> <out> …) its 3rd). A list() sub-command
// reads its list by name, so the list's value (${<list>}) is read too; list(POP_FRONT|POP_BACK <list>
// <out>…) writes every <out>.
func writtenVar(cmd string, args []string) (names []string, read string) {
	name, read := writtenVar1(cmd, args)
	if cmd == "list" && len(args) > 0 {
		switch strings.ToUpper(strings.Trim(args[0], `"`)) {
		case "POP_FRONT", "POP_BACK":
			for _, a := range args[min(2, len(args)):] {
				names = append(names, strings.Trim(a, `"`))
			}
		}
	}
	return append(names, name), read
}

// writtenVar1 is writtenVar for the one output every sub-command has.
func writtenVar1(cmd string, args []string) (name, read string) {
	arg := func(i int) string {
		if i >= 0 && i < len(args) {
			return strings.Trim(args[i], `"`)
		}
		return ""
	}
	without := func(skip int) string {
		var keep []string
		for i, a := range args {
			if i != skip {
				keep = append(keep, a)
			}
		}
		return strings.Join(keep, " ")
	}
	all := strings.Join(args, " ")
	switch cmd {
	case "set":
		return arg(0), all
	case "separate_arguments": // separate_arguments(<out> <mode> "<args>")
		return arg(0), without(0)
	case "list":
		if arg(1) != "" {
			all += " ${" + arg(1) + "}"
		}
		switch strings.ToUpper(arg(0)) {
		case "LENGTH", "GET", "JOIN", "SUBLIST", "FIND":
			return arg(len(args) - 1), all
		case "TRANSFORM":
			for i := range args {
				if strings.EqualFold(arg(i), "OUTPUT_VARIABLE") {
					return arg(i + 1), all
				}
			}
		}
		return arg(1), all
	case "string":
		switch strings.ToUpper(arg(0)) {
		case "REPLACE":
			return arg(3), without(1)
		case "REGEX":
			switch strings.ToUpper(arg(1)) {
			case "REPLACE":
				return arg(4), without(2)
			case "MATCH", "MATCHALL":
				return arg(3), without(2)
			}
			return "", all
		case "JOIN", "TOLOWER", "TOUPPER", "STRIP", "GENEX_STRIP", "CONFIGURE", "MAKE_C_IDENTIFIER", "LENGTH", "HEX":
			return arg(2), all
		case "SUBSTRING":
			return arg(4), all
		case "REPEAT", "FIND":
			return arg(3), all
		}
		return arg(1), all
	}
	return "", all
}

// optionFlags returns the AVX-class flags, literal or through a variable in avxVars, among a command's
// arguments that are options: an unquoted argument, or a quoted one whose every word is an option
// (-x, /x), a variable or a generator expression. A quoted sentence that names a flag is not one.
func optionFlags(args string, avxVars map[string]bool) []string {
	var out []string
	for _, a := range cmakeArgs(args) {
		if strings.HasPrefix(a, `"`) {
			// A quoted list ("sse4.2;-mavx2") is several items: each is an option or a sentence on its own.
			var kept []string
			for _, item := range strings.Split(strings.Trim(a, `"`), ";") {
				if !slices.ContainsFunc(strings.Fields(item), func(w string) bool {
					return !strings.HasPrefix(w, "-") && !strings.HasPrefix(w, "/") && !strings.HasPrefix(w, "$")
				}) {
					kept = append(kept, item)
				}
			}
			a = strings.Join(kept, " ")
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
	aggregateRE   = regexp.MustCompile(`^(?:(?:static|extern|const|volatile)\s+)*(typedef\b|(struct|enum|union)\b)`)
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
// lines skipped. An aggregate's body, a `typedef struct {…} Name;` and function bodies are not definitions
// of their own; a variable declared after an aggregate's body (`struct {…} s;`) is.
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
				case aggregateRE.MatchString(head) && !strings.Contains(head, "("): // not `struct s f(void) {`
					kind = "agg"
					stmt += "{}" // the body is skipped; what follows it may declare variables
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
				if kind == "" || kind == "init" || kind == "agg" {
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
	gateSkipRE   = regexp.MustCompile(`^(static|typedef)\b`)
	gateExternRE = regexp.MustCompile(`^extern\s*(?:"C(?:\+\+)?"\s*)?`)
	// An elaborated type that starts a statement: `struct tag`, `enum tag {…}`, `union {…}` (the body is
	// left as "{}" by checkGateSymbols).
	gateAggHeadRE = regexp.MustCompile(`^(?:const\s+|volatile\s+)*(?:struct|enum|union)\b\s*(?:[A-Za-z_]\w*)?\s*(?:\{\})?`)
	trailingRE    = regexp.MustCompile(`\([^()]*\)\s*$`)
	// An array bound or an alignment specifier: parentheses inside them (sizeof(int), _Alignas(16)) are
	// not a parameter list, and identifiers inside them are not the declared name.
	declNoiseRE = regexp.MustCompile(`\[[^\[\]]*\]|\b(?:_Alignas|alignas)\s*\((?:[^()]|\([^()]*\))*\)`)
)

// gateDefinition reports the external names that a file-scope statement defines: every declarator of a
// variable definition (`int a = 1, b = 2;`, `struct state s;`, `enum e {A} v;`), or a function with its
// body. `extern` makes a statement a mere declaration only without a body or an initializer (`extern int
// x = 5;` defines x). A struct, enum or union statement that declares no variable (`struct state;`,
// `struct state {…};`) defines nothing external.
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
	if m := gateAggHeadRE.FindString(s); m != "" {
		if s = strings.TrimSpace(s[len(m):]); s == "" {
			return // a type, no variable
		}
	}
	decls := []string{s}
	if !body {
		decls = splitTop(s) // the declarators of one statement
	}
	for k, decl := range decls {
		i := strings.Index(decl, "=")
		if i >= 0 {
			decl = decl[:i]
		}
		for declNoiseRE.MatchString(decl) {
			decl = declNoiseRE.ReplaceAllString(decl, " ")
		}
		if i < 0 && !body && strings.Contains(decl, "(") && !strings.Contains(decl, "(*") {
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
