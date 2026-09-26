package hxl

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"testing"
)

// TestNoFusedMultiplyAddInMachineCode checks 06 §1.2 rule 1 where it matters: in the machine code gc
// actually emits for pkg/hxl and pkg/hxl/det on the targets that fuse (arm64 and amd64 at
// GOAMD64=v3; ppc64le, s390x and riscv64 with HXL_FMA_ASM_ALL=1). The hxlfloat lint checks the
// source rule; this checks the compiler honoured it: no float fused multiply-add instruction may
// appear. It needs the go command (skipped without it, and in -short mode).
func TestNoFusedMultiplyAddInMachineCode(t *testing.T) {
	if testing.Short() {
		t.Skip("-short")
	}
	goCmd, err := exec.LookPath("go")
	if err != nil {
		goCmd = filepath.Join(runtime.GOROOT(), "bin", "go")
		if _, err := os.Stat(goCmd); err != nil {
			t.Skip("go command not found")
		}
	}
	targets := [][]string{{"GOARCH=arm64"}, {"GOARCH=amd64", "GOAMD64=v3"}}
	if os.Getenv("HXL_FMA_ASM_ALL") != "" {
		targets = append(targets, []string{"GOARCH=ppc64le"}, []string{"GOARCH=s390x"}, []string{"GOARCH=riscv64"},
			[]string{"GOARCH=loong64"})
	}
	// Float fused multiply-add mnemonics of every gc backend (integer MADD/MSUB have no F prefix).
	fused := regexp.MustCompile(`\s(VF(N)?M(ADD|SUB)[0-9]*S[DS]|F(N)?M(ADD|SUB)[DS]?|VFML[AS])\s`)
	floatMul := regexp.MustCompile(`\s(FMULD|MULSD|VMULSD|FMUL|FMULS|MULD)\s`)
	_, file, _, _ := runtime.Caller(0)
	moduleDir := filepath.Join(filepath.Dir(file), "..", "..")
	const pattern = "github.com/PageMastr/scifi-test/services/pkg/hxl/...=-S"
	listing := func(env []string, pkgs ...string) string {
		cmd := exec.Command(goCmd, append([]string{"build", "-gcflags=" + pattern}, pkgs...)...)
		cmd.Dir = moduleDir
		cmd.Env = append(append(os.Environ(), "CGO_ENABLED=0", "GOAMD64=v1"), env...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("%s: go build %v: %v\n%s", strings.Join(env, " "), pkgs, err, out)
		}
		return string(out)
	}
	for _, env := range targets {
		name := strings.Join(env, " ")
		// Positive control: the planted lint fixture has unconverted a*b + c, which this target fuses.
		if !fused.MatchString(listing(env, "./pkg/hxl/hxlfloat/testdata/planted/")) {
			t.Errorf("%s: the planted fixture shows no fused multiply-add; the instruction pattern is stale", name)
		}
		out := listing(env, "./pkg/hxl/", "./pkg/hxl/det/")
		muls, fmas := 0, 0
		for _, line := range strings.Split(out, "\n") {
			// Only code of the HXL packages (the listing also covers imported packages matched by
			// the pattern, e.g. the generated gamedef codecs, which have no float arithmetic).
			if !strings.Contains(line, "/pkg/hxl/") {
				continue
			}
			if floatMul.MatchString(line) {
				muls++
			}
			if fused.MatchString(line) {
				fmas++
				t.Errorf("%s: fused multiply-add in HXL code: %s", name, strings.TrimSpace(line))
			}
		}
		if muls < 10 {
			t.Errorf("%s: found only %d float multiplications in the listing; is -S output missing?", name, muls)
		}
		t.Logf("%s: %d float multiplications, %d fused", name, muls, fmas)
	}
}
