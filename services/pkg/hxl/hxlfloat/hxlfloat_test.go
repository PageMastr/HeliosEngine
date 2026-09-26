package hxlfloat

import (
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

var wantRE = regexp.MustCompile(`// want((?: "[a-z-]+")+)`)

// wants maps "file:line" to the expected rules (with multiplicity) from `// want "rule" ...`.
func wants(t *testing.T, dir string) map[string][]string {
	out := map[string][]string{}
	files, _ := filepath.Glob(filepath.Join(dir, "*.go"))
	for _, f := range files {
		data, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		for i, line := range strings.Split(string(data), "\n") {
			m := wantRE.FindStringSubmatch(line)
			if m == nil {
				continue
			}
			for _, r := range regexp.MustCompile(`"([a-z-]+)"`).FindAllStringSubmatch(m[1], -1) {
				key := filepath.Base(f) + ":" + strconv.Itoa(i+1)
				out[key] = append(out[key], r[1])
			}
		}
	}
	return out
}

// TestPlantedViolations: the analyzer catches 100 % of the planted violations and reports nothing
// else (GP-1: "catches 100% of its planted violations").
func TestPlantedViolations(t *testing.T) {
	dir := filepath.Join("testdata", "planted")
	findings, err := CheckDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	want := wants(t, dir)
	total := 0
	for _, r := range want {
		total += len(r)
	}
	got := map[string][]string{}
	for _, f := range findings {
		key := filepath.Base(f.Pos.Filename) + ":" + strconv.Itoa(f.Pos.Line)
		got[key] = append(got[key], f.Rule)
	}
	caught := 0
	for key, rules := range want {
		remaining := append([]string(nil), got[key]...)
		for _, r := range rules {
			found := false
			for i, g := range remaining {
				if g == r {
					remaining = append(remaining[:i], remaining[i+1:]...)
					found = true
					break
				}
			}
			if found {
				caught++
			} else {
				t.Errorf("%s: planted %s violation not reported", key, r)
			}
		}
		for _, extra := range remaining {
			t.Errorf("%s: unexpected %s finding", key, extra)
		}
	}
	for key, rules := range got {
		if _, ok := want[key]; !ok {
			t.Errorf("%s: unexpected findings %v", key, rules)
		}
	}
	if total < 15 {
		t.Errorf("only %d planted violations", total)
	}
	t.Logf("hxlfloat caught %d of %d planted violations", caught, total)
}

func TestCleanFixture(t *testing.T) {
	findings, err := CheckDir(filepath.Join("testdata", "clean"))
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range findings {
		t.Errorf("false positive: %s", f)
	}
}

// TestHXLPackagesClean: the analyzer reports zero findings on pkg/hxl/... (GP-1).
func TestHXLPackagesClean(t *testing.T) {
	_, file, _, _ := runtime.Caller(0)
	root := filepath.Dir(filepath.Dir(file)) // services/pkg/hxl
	// Every package below pkg/hxl ("pkg/hxl/..."), at any depth, except the fixtures.
	var dirs []string
	err := filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			if d.Name() == "testdata" {
				return filepath.SkipDir
			}
			dirs = append(dirs, path)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	checked := 0
	for _, d := range dirs {
		findings, err := CheckDir(d)
		if err != nil {
			t.Fatalf("%s: %v", d, err)
		}
		checked++
		for _, f := range findings {
			t.Errorf("%s", f)
		}
	}
	if checked < 5 { // hxl, det, gamedef, hxlfloat, hxlfloat/cmd/hxlfloat
		t.Errorf("checked only %d packages", checked)
	}
}
