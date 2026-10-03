// Package conformance implements helios-conformance, the plan-conformance lint of
// docs/plan/09-roadmap-and-process.md §5.10.3: rules CONF-01…12, the anchor map (§5.10.2 D2),
// `conformance:allow` suppressions, known-failing records and SARIF output.
//
// Threading: everything here runs on the calling goroutine; a Tree is not safe for concurrent use.
package conformance

import (
	"bytes"
	"io/fs"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"sort"
	"strings"
)

// Tree is the set of files the lint sees under Root: in a git work tree, the files git tracks or
// would track (`ls-files --cached --others --exclude-standard`), so working-tree code awaiting its
// WP counts (§5.10.2 D7) and ignored build output does not; elsewhere, every file.
type Tree struct {
	Root  string
	Files []string // slash-separated paths relative to Root, sorted
	cache map[string][]string
	gidx  *goIndex
}

// ownFixtures are the lint's seeded violations: they fail by design, so a repository run skips them.
const ownFixtures = "tools/conformance/testdata/"

// LoadTree lists the files under root.
func LoadTree(root string) (*Tree, error) {
	abs, err := filepath.Abs(root)
	if err != nil {
		return nil, err
	}
	files, ok := gitFiles(abs)
	if !ok {
		if files, err = walkFiles(abs); err != nil {
			return nil, err
		}
	}
	t := &Tree{Root: abs, cache: map[string][]string{}}
	for _, f := range files {
		if strings.HasPrefix(f, ownFixtures) || vendored(f) {
			continue
		}
		if st, err := os.Stat(filepath.Join(abs, filepath.FromSlash(f))); err == nil && st.Mode().IsRegular() {
			t.Files = append(t.Files, f)
		}
	}
	sort.Strings(t.Files)
	return t, nil
}

// vendored reports third-party code, which no rule covers except the vendored build glue and
// patches that CONF-11 and CONF-12 name.
func vendored(f string) bool {
	if !strings.HasPrefix(f, "third_party/") || f == "third_party/CMakeLists.txt" {
		return false
	}
	parts := strings.Split(f, "/")
	return !(len(parts) > 3 && parts[2] == "patches")
}

func gitFiles(root string) ([]string, bool) {
	cmd := exec.Command("git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
	out, err := cmd.Output()
	if err != nil {
		return nil, false
	}
	var files []string
	for _, f := range bytes.Split(out, []byte{0}) {
		if len(f) > 0 {
			files = append(files, string(f))
		}
	}
	return files, true
}

func walkFiles(root string) ([]string, error) {
	var files []string
	err := filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		// Only the top-level build/ is build output (CLAUDE.md's build/<task> directories); a source
		// directory named build deeper down, such as tools/build/, is code.
		if d.IsDir() && p != root && (d.Name() == ".git" || d.Name() == "node_modules" ||
			(d.Name() == "build" && filepath.Dir(p) == root)) {
			return filepath.SkipDir
		}
		if d.Type().IsRegular() {
			rel, _ := filepath.Rel(root, p)
			files = append(files, filepath.ToSlash(rel))
		}
		return nil
	})
	return files, err
}

// Lines returns a file's lines without line terminators (CRLF checkouts read the same), or nil for
// an unreadable or binary file.
func (t *Tree) Lines(f string) []string {
	if l, ok := t.cache[f]; ok {
		return l
	}
	data, err := os.ReadFile(filepath.Join(t.Root, filepath.FromSlash(f)))
	var lines []string
	if err == nil && !bytes.Contains(data[:min(len(data), 8192)], []byte{0}) {
		lines = strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n")
	}
	t.cache[f] = lines
	return lines
}

// Text returns a file's content with LF line ends.
func (t *Tree) Text(f string) string { return strings.Join(t.Lines(f), "\n") }

// Match reports whether a slash path matches a glob: `**` spans any number of directories, and the
// other segments use path.Match (`*`, `?`, `[…]`).
func Match(glob, p string) bool { return matchSegs(strings.Split(glob, "/"), strings.Split(p, "/")) }

func matchSegs(gs, ps []string) bool {
	for len(gs) > 0 {
		if gs[0] == "**" {
			for i := 0; i <= len(ps); i++ {
				if matchSegs(gs[1:], ps[i:]) {
					return true
				}
			}
			return false
		}
		if len(ps) == 0 {
			return false
		}
		if ok, _ := path.Match(gs[0], ps[0]); !ok {
			return false
		}
		gs, ps = gs[1:], ps[1:]
	}
	return len(ps) == 0
}

// MatchAny reports whether p matches one of the globs.
func MatchAny(globs []string, p string) bool {
	for _, g := range globs {
		if Match(g, p) {
			return true
		}
	}
	return false
}
