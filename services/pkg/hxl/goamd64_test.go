package hxl

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestCorpusAtGOAMD64v3 runs GP-1's corpus clause at GOAMD64=v3 (06 §1.2 rule 7), where gc may fuse
// a*b + c into an FMA, from a plain `go test` on amd64: it builds this package's test binary with
// GOAMD64=v3 and runs TestCorpus and TestFMAHazardIsReal in it. The CI job runs `go test ./...` at
// the default v1, so without this the v3 run happened only by hand. It is skipped with -short,
// off amd64, when this binary is already v3 or higher, and on CPUs without x86-64-v3.
func TestCorpusAtGOAMD64v3(t *testing.T) {
	if testing.Short() {
		t.Skip("-short")
	}
	if runtime.GOARCH != "amd64" {
		t.Skip("not amd64")
	}
	if level := GOAMD64(); level >= "v3" {
		t.Skipf("already running at GOAMD64=%s", level)
	}
	goCmd, err := exec.LookPath("go")
	if err != nil {
		goCmd = filepath.Join(runtime.GOROOT(), "bin", "go")
		if _, err := os.Stat(goCmd); err != nil {
			t.Skip("go command not found")
		}
	}
	_, file, _, _ := runtime.Caller(0)
	pkgDir := filepath.Dir(file)
	bin := filepath.Join(t.TempDir(), "hxl_v3.test")
	if runtime.GOOS == "windows" {
		bin += ".exe"
	}
	build := exec.Command(goCmd, "test", "-c", "-o", bin, ".")
	build.Dir = pkgDir
	build.Env = append(os.Environ(), "GOAMD64=v3", "CGO_ENABLED=0")
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("go test -c at GOAMD64=v3: %v\n%s", err, out)
	}
	run := exec.Command(bin, "-test.run", "^(TestCorpus|TestFMAHazardIsReal)$", "-test.v", "-test.count=1")
	run.Dir = pkgDir
	out, err := run.CombinedOutput()
	text := string(out)
	if err != nil {
		if strings.Contains(text, "microarchitecture support") {
			t.Skipf("this CPU cannot run GOAMD64=v3 code: %s", strings.TrimSpace(text))
		}
		t.Fatalf("the corpus fails at GOAMD64=v3: %v\n%s", err, text)
	}
	for _, want := range []string{"--- PASS: TestCorpus ", "--- PASS: TestFMAHazardIsReal ", `GOAMD64="v3"`} {
		if !strings.Contains(text, want) {
			t.Errorf("GOAMD64=v3 run: %q missing from the output:\n%s", want, text)
		}
	}
	for _, line := range strings.Split(text, "\n") {
		if strings.Contains(line, "hxl corpus:") || strings.Contains(line, "differs on") {
			t.Log("GOAMD64=v3: " + strings.TrimSpace(line))
		}
	}
}
