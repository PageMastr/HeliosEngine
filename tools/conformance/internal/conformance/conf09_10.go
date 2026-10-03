package conformance

import (
	"regexp"
	"strings"
)

// CONF-09 (ADR-014): the backend's Go toolchain is 1.27.x everywhere, and CI takes it from services/go.mod.
var _ = register(&Rule{
	ID:     "CONF-09",
	Anchor: "ADR-014",
	Title:  "A go.mod go or toolchain directive other than 1.27.x; a CI setup-go step that does not read services/go.mod",
	Scope:  []string{"services/go.mod", ".github/**"},
	Types:  regexp.MustCompile(`(^|/)go\.mod$|\.ya?ml$`),
	Check:  checkGoToolchain,
})

var (
	goDirectiveRE = regexp.MustCompile(`^\s*(go|toolchain)\s+(\S+)`)
	goVersionOK   = map[string]*regexp.Regexp{
		"go":        regexp.MustCompile(`^1\.27(\.\d+)?$`),
		"toolchain": regexp.MustCompile(`^go1\.27(\.\d+)?$`),
	}
	setupGoRE = regexp.MustCompile(`\buses\s*:\s*["']?actions/setup-go@`)
	stepRE    = regexp.MustCompile(`^(\s*)(-\s+)?`)
	// The step's `with:` keys, in block (`go-version: x`) or flow (`{go-version: x}`) style.
	versionFileRE = regexp.MustCompile(`(?:^|[\s{,])go-version-file\s*:\s*["']?([^\s,"'}]*)`)
	versionKeyRE  = regexp.MustCompile(`(?:^|[\s{,])go-version\s*:`)
	// GOTOOLCHAIN set in an env map (`GOTOOLCHAIN: v`) or a script (`GOTOOLCHAIN=v`, `go env -w …`,
	// `echo "GOTOOLCHAIN=v" >> $GITHUB_ENV`) overrides the go.mod toolchain that ADR-014 pins.
	goToolchainEnvRE = regexp.MustCompile(`\bGOTOOLCHAIN\s*[:=][ \t]*["']?([^\s"',}]*)`)
	goToolchainOK    = regexp.MustCompile(`^(|auto|local|path|local\+(auto|path)|go1\.27(\.\d+)?(\+(auto|path))?)$`)
)

func checkGoToolchain(p *Pass) {
	for _, f := range p.Files {
		lines := p.Tree.Lines(f)
		switch {
		case strings.HasSuffix(f, "go.mod"):
			sawGo := false
			for i, l := range lines {
				m := goDirectiveRE.FindStringSubmatch(strings.SplitN(l, "//", 2)[0])
				if m == nil {
					continue
				}
				sawGo = sawGo || m[1] == "go"
				if !goVersionOK[m[1]].MatchString(m[2]) {
					p.Report(f, i+1, "`%s %s`: ADR-014 pins Go 1.27.x", m[1], m[2])
				}
			}
			if !sawGo {
				p.Report(f, 0, "no `go` directive: ADR-014 pins Go 1.27.x")
			}
		case strings.HasSuffix(f, ".yml") || strings.HasSuffix(f, ".yaml"):
			code := make([]string, len(lines))
			for i, l := range lines {
				code[i] = yamlCode(l)
			}
			for i, l := range code {
				for _, loc := range setupGoRE.FindAllStringIndex(l, -1) { // a flow sequence holds several steps
					checkSetupGo(p, f, code, i, loc[0])
				}
				for _, m := range goToolchainEnvRE.FindAllStringSubmatch(l, -1) {
					if !goToolchainOK.MatchString(m[1]) {
						p.Report(f, i+1, "GOTOOLCHAIN=%s overrides services/go.mod's toolchain: use auto, local or "+
							"go1.27.x (ADR-014)", m[1])
					}
				}
			}
		}
	}
}

// yamlCode returns a YAML line without its comment: a `#` at the start or after whitespace, outside
// quotes.
func yamlCode(l string) string {
	var quote byte
	for i := 0; i < len(l); i++ {
		c := l[i]
		switch {
		case quote != 0:
			if c == quote {
				quote = 0
			}
		case c == '"' || c == '\'':
			quote = c
		case c == '#' && (i == 0 || l[i-1] == ' ' || l[i-1] == '\t'):
			return l[:i]
		}
	}
	return l
}

// checkSetupGo checks the step whose `uses: actions/setup-go` starts at column col of line i: it must
// read the version from services/go.mod (`go-version-file`) and must not pin one (`go-version`). The
// step is a flow mapping (`- {uses: …, with: {…}}`) when an unclosed `{` precedes `uses` on its line,
// and otherwise the block of lines indented below the step's `- `.
func checkSetupGo(p *Pass, f string, lines []string, i, col int) {
	var step string
	if before := lines[i][:col]; strings.Count(before, "{") > strings.Count(before, "}") {
		step = flowMapping(lines, i, strings.LastIndex(before, "{"))
	} else {
		m := stepRE.FindStringSubmatch(lines[i])
		start, indent := i, len(m[1])
		if m[2] == "" { // `uses:` is not the step's first key: find the `- ` that opens the step
			for start = i - 1; start >= 0; start-- {
				if t := strings.TrimLeft(lines[start], " "); strings.HasPrefix(t, "- ") && len(lines[start])-len(t) < indent {
					indent = len(lines[start]) - len(t)
					break
				}
			}
			if start < 0 {
				start = i
			}
		}
		var b strings.Builder
		for j := start; j < len(lines); j++ {
			t := strings.TrimLeft(lines[j], " ")
			if j > start && strings.TrimSpace(t) != "" && len(lines[j])-len(t) <= indent {
				break
			}
			b.WriteString(lines[j])
			b.WriteByte('\n')
		}
		step = b.String()
	}
	versionFile := ""
	if m := versionFileRE.FindStringSubmatch(step); m != nil {
		versionFile = m[1]
	}
	switch {
	case versionKeyRE.MatchString(step):
		p.Report(f, i+1, "setup-go pins `go-version`; read it from services/go.mod with `go-version-file` (ADR-014)")
	case versionFile != "services/go.mod":
		p.Report(f, i+1, "setup-go does not read services/go.mod (`go-version-file: services/go.mod`, ADR-014)")
	}
}

// flowMapping returns the text of the flow mapping that opens at column col of line i, up to its
// closing brace (or the end of the file), across lines.
func flowMapping(lines []string, i, col int) string {
	var b strings.Builder
	depth := 0
	for j := i; j < len(lines); j++ {
		l := lines[j]
		if j == i {
			l = l[col:]
		}
		for k := 0; k < len(l); k++ {
			switch l[k] {
			case '{':
				depth++
			case '}':
				if depth--; depth == 0 {
					b.WriteString(l[:k+1])
					return b.String()
				}
			}
		}
		b.WriteString(l)
		b.WriteByte('\n')
	}
	return b.String()
}

// CONF-10 (08 §1.16; reconciliation #19): only the launcher and engine/ui's SDL_Renderer backend create
// an SDL renderer; the client and editor draw through the RHI. It absorbs WP-0.17's symbol lint.
var _ = register(&Rule{
	ID:     "CONF-10",
	Anchor: "08 §1.16; reconciliation #19",
	Title:  "SDL_CreateRenderer outside the launcher and engine/ui's SDL_Renderer backend",
	Scope:  []string{"**"},
	// 08 §1.16's two users. engine/ui does not exist yet: WP-0.17 names the backend's files, and a path
	// with "sdl_renderer" in it is the backend.
	Allow: []string{"apps/launcher/**", "engine/ui/**/*sdl_renderer*", "engine/ui/**/*sdl_renderer*/**"},
	Types: cFamily,
	Check: checkSDLRenderer,
})

// SDL3's other renderer constructors create the same SDL_Renderer, so they count too.
var sdlRendererRE = regexp.MustCompile(`\bSDL_Create(Renderer\w*|WindowAndRenderer|SoftwareRenderer|GPURenderer)\b`)

func checkSDLRenderer(p *Pass) {
	for _, f := range p.Files {
		// Splices are joined first (a name split by `\`-newline is still the name), and a finding is
		// reported on the physical line where its logical line starts. Strings stay: a dynamic lookup
		// by name ("SDL_CreateRenderer") counts as a call. `#if 0` groups are never compiled.
		logical, origin := spliceLines(p.Tree.Lines(f))
		code := codeLines(logical, false)
		dead := inactiveLines(code)
		for i, l := range code {
			if !dead[i] && sdlRendererRE.MatchString(l) {
				p.Report(f, origin[i]+1, "%s outside the launcher and engine/ui's SDL_Renderer backend (08 §1.16)",
					sdlRendererRE.FindString(l))
			}
		}
	}
}
