package main

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func runCmd(t *testing.T, now time.Time, args ...string) (int, string, string) {
	t.Helper()
	var out, errOut bytes.Buffer
	code := run(context.Background(), args, &out, &errOut, func() time.Time { return now })
	return code, out.String(), errOut.String()
}

// publish --channel dev creates dev keys, writes the CDN, and verify accepts it with a state file; a
// tampered pointer then fails verify with the check's name.
func TestPublishVerify(t *testing.T) {
	dir := t.TempDir()
	build := filepath.Join(dir, "build")
	if err := os.MkdirAll(filepath.Join(build, "bin"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(build, "bin", "client"), bytes.Repeat([]byte("helios"), 50000), 0o644); err != nil {
		t.Fatal(err)
	}
	data := filepath.Join(dir, "helios-data")
	now := time.Unix(1790000000, 0)
	target := []string{"--product", "sample-game", "--channel", "dev", "--platform", "win64", "--data-dir", data}
	code, out, errOut := runCmd(t, now, append([]string{"publish", "--build", build, "--cdn-host",
		"http://127.0.0.1:7700/cdn"}, target...)...)
	if code != 0 || !strings.Contains(out, "created dev keys") || !strings.Contains(out, "pointer sequence 1") {
		t.Fatalf("publish: %d %s %s", code, out, errOut)
	}
	code, out, _ = runCmd(t, now, append([]string{"publish", "--build", build}, target...)...)
	if code != 0 || strings.Contains(out, "created dev keys") {
		t.Fatalf("second publish: %d %s", code, out)
	}
	state := filepath.Join(dir, "state.json")
	code, out, errOut = runCmd(t, now.Add(time.Minute), append([]string{"verify", "--state", state}, target...)...)
	if code != 0 || !strings.Contains(out, "verified sample-game/dev/win64") {
		t.Fatalf("verify: %d %s %s", code, out, errOut)
	}
	if b, err := os.ReadFile(state); err != nil || !strings.Contains(string(b), `"pointerSequence":2`) {
		t.Fatalf("state: %s %v", b, err)
	}
	ptr := filepath.Join(data, "cdn", "channels", "sample-game", "dev", "win64.json")
	b, err := os.ReadFile(ptr)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(ptr, bytes.Replace(b, []byte(`"rollout_pct":100`), []byte(`"rollout_pct":10`), 1), 0o644); err != nil {
		t.Fatal(err)
	}
	code, _, errOut = runCmd(t, now.Add(time.Minute), append([]string{"verify"}, target...)...)
	if code != 1 || !strings.Contains(errOut, "pointer-signature") {
		t.Fatalf("tampered pointer: %d %s", code, errOut)
	}
	// Expired: a week later.
	if err := os.WriteFile(ptr, b, 0o644); err != nil {
		t.Fatal(err)
	}
	code, _, errOut = runCmd(t, now.Add(8*24*time.Hour), append([]string{"verify"}, target...)...)
	if code != 1 || !strings.Contains(errOut, "pointer-expired") {
		t.Fatalf("expired pointer: %d %s", code, errOut)
	}
}

func TestUsage(t *testing.T) {
	for _, args := range [][]string{
		nil,
		{"frobnicate"},
		{"publish"},
		{"publish", "--product", "p-p", "--channel", "dev", "--platform", "win64"}, // no --build
		{"publish", "--build", "x", "--product", "p-p", "--channel", "dev", "--platform", "win64", "--rollout-pct", "101"},
		{"verify", "--product", "p-p"},
		{"verify", "--product", "p-p", "--channel", "dev", "--platform", "win64", "extra"},
		{"verify", "--bogus"},
	} {
		if code, _, _ := runCmd(t, time.Now(), args...); code != 2 {
			t.Errorf("%q: exit %d, want 2", args, code)
		}
	}
	if code, out, _ := runCmd(t, time.Now(), "version"); code != 0 || !strings.HasPrefix(out, "helios-patch ") {
		t.Errorf("version: %d %q", code, out)
	}
	// Live needs explicit keys.
	code, _, errOut := runCmd(t, time.Now(), "publish", "--build", t.TempDir(), "--product", "sample-game", "--channel",
		"live", "--platform", "win64", "--data-dir", t.TempDir())
	if code != 1 || !strings.Contains(errOut, "--keys is required") {
		t.Errorf("live without keys: %d %s", code, errOut)
	}
}
