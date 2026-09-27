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
	setupGoRE = regexp.MustCompile(`^(\s*)(-\s+)?uses:\s*["']?actions/setup-go@`)
	yamlKeyRE = regexp.MustCompile(`^\s*(-\s+)?([A-Za-z_-]+):\s*(.*?)\s*$`)
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
			for i, l := range lines {
				if m := setupGoRE.FindStringSubmatch(l); m != nil {
					checkSetupGo(p, f, lines, i, m)
				}
			}
		}
	}
}

// checkSetupGo checks the step whose `uses: actions/setup-go` is on line i: it must read the
// version from services/go.mod (`go-version-file`) and must not pin one (`go-version`).
func checkSetupGo(p *Pass, f string, lines []string, i int, m []string) {
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
	versionFile, pinned := "", false
	for j := start; j < len(lines); j++ {
		t := strings.TrimLeft(lines[j], " ")
		if j > start && t != "" && !strings.HasPrefix(t, "#") && len(lines[j])-len(t) <= indent {
			break
		}
		if k := yamlKeyRE.FindStringSubmatch(strings.SplitN(lines[j], " #", 2)[0]); k != nil {
			switch k[2] {
			case "go-version-file":
				versionFile = strings.Trim(k[3], `"'`)
			case "go-version":
				pinned = true
			}
		}
	}
	switch {
	case pinned:
		p.Report(f, i+1, "setup-go pins `go-version`; read it from services/go.mod with `go-version-file` (ADR-014)")
	case versionFile != "services/go.mod":
		p.Report(f, i+1, "setup-go does not read services/go.mod (`go-version-file: services/go.mod`, ADR-014)")
	}
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
		// Strings stay: a dynamic lookup by name ("SDL_CreateRenderer") counts as a call.
		for i, l := range codeLines(p.Tree.Lines(f), false) {
			if sdlRendererRE.MatchString(l) {
				p.Report(f, i+1, "%s outside the launcher and engine/ui's SDL_Renderer backend (08 §1.16)",
					sdlRendererRE.FindString(l))
			}
		}
	}
}
