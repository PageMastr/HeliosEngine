package hxl

import (
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

//go:noinline
func unconvertedMulAdd(a, b, c float64) float64 { return a*b + c } // the pattern 06 §1.2 rule 1 bans

// TestFMAHazardIsReal is a negative control for 06 §1.2 rule 1: on targets where the Go compiler
// fuses multiply-adds (arm64, GOAMD64>=v3) an unconverted a*b + c really does change results on the
// FMA-sensitive corpus vectors, while the VM (which converts every product) still matches C++
// (TestCorpus). On other targets it only checks the vectors are well formed.
func TestFMAHazardIsReal(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join(corpusDir(t), "fma_sensitive.jsonc"))
	if err != nil {
		t.Fatal(err)
	}
	var doc struct {
		Cases []struct {
			Fma  string `json:"fma"`
			Rows struct {
				Data [][]string `json:"data"`
			} `json:"rows"`
		} `json:"cases"`
	}
	if err := json.Unmarshal(stripJSONC(raw), &doc); err != nil {
		t.Fatal(err)
	}
	fuses := runtime.GOARCH == "arm64" || (runtime.GOARCH == "amd64" && GOAMD64() >= "v3")
	rows, differ := 0, 0
	for _, c := range doc.Cases {
		if c.Fma != "mul_add" {
			continue
		}
		for _, r := range c.Rows.Data {
			var v [4]float64
			for i := range v {
				x, ok := parseCorpusNumber(r[i])
				if !ok {
					t.Fatalf("bad number %q", r[i])
				}
				v[i] = x
			}
			rows++
			if math.Float64bits(unconvertedMulAdd(v[0], v[1], v[2])) != math.Float64bits(v[3]) {
				differ++
			}
		}
	}
	t.Logf("GOARCH=%s GOAMD64=%q: unconverted a*b+c differs on %d of %d FMA-sensitive rows", runtime.GOARCH, GOAMD64(), differ, rows)
	if fuses && differ == 0 {
		t.Errorf("expected the compiler to fuse a*b+c on this target")
	}
	if !fuses && differ != 0 {
		t.Errorf("unexpected fusion without FMA codegen")
	}
}
