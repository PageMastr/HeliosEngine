package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/patchtrust"
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
	state := filepath.Join(dir, "state.bin")
	code, out, errOut = runCmd(t, now.Add(time.Minute), append([]string{"verify", "--state", state}, target...)...)
	if code != 0 || !strings.Contains(out, "verified sample-game/dev/win64") {
		t.Fatalf("verify: %d %s %s", code, out, errOut)
	}
	if st, err := (patchtrust.FileStateStore{Path: state}).Load(); err != nil || st.PointerSequence != 2 {
		t.Fatalf("state: %+v %v", st, err)
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

	// publish checks the keyset against the signing directory's roots.json: one signed by another root
	// (here with a higher version, so only the root check refuses it) is not published.
	keysDir := filepath.Join(data, "keys", "patch", "sample-game")
	ksb, err := os.ReadFile(filepath.Join(keysDir, "keyset.json"))
	if err != nil {
		t.Fatal(err)
	}
	ks, err := patchtrust.ParseKeyset(ksb)
	if err != nil {
		t.Fatal(err)
	}
	_, other, _ := ed25519.GenerateKey(nil)
	ks.Version++
	if err := ks.Sign(other); err != nil {
		t.Fatal(err)
	}
	forged, _ := ks.Marshal()
	if err := os.WriteFile(filepath.Join(keysDir, "keyset.json"), forged, 0o644); err != nil {
		t.Fatal(err)
	}
	code, _, errOut = runCmd(t, now, append([]string{"publish", "--build", build}, target...)...)
	if code != 1 || !strings.Contains(errOut, "keyset-signature") {
		t.Fatalf("a keyset of another root: %d %s", code, errOut)
	}
}

// The default data directory, as services/README.md runs publish (from services/), is git-ignored: dev key
// directories also ignore themselves (pkg/patchcdn's TestDevKeysAreGitIgnored).
func TestDefaultDataDirIsGitIgnored(t *testing.T) {
	git, err := exec.LookPath("git")
	if err != nil {
		t.Skip("git is not installed")
	}
	services := filepath.Join("..", "..")
	if exec.Command(git, "-C", services, "rev-parse", "--is-inside-work-tree").Run() != nil {
		t.Skip("not in a git work tree")
	}
	for _, f := range []string{"helios-data/keys/patch/sample-game/manifest-key.json",
		"helios-data/keys/patch/sample-game/root-keys.json", "helios-data/cdn/keys/sample-game/keyset.json"} {
		if err := exec.Command(git, "-C", services, "check-ignore", "-q", f).Run(); err != nil {
			t.Errorf("git does not ignore services/%s: %v", f, err)
		}
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
		{"verify", "--product", "../evil", "--channel", "dev", "--platform", "win64"},
		{"publish", "--build", "x", "--product", "p-p", "--channel", "../dev", "--platform", "win64"},
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
