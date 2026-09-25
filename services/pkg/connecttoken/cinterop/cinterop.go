// Package cinterop builds and runs the C harness in ../testdata/interop against the vendored
// netcode (third_party/netcode), for tests that cross-check Go connect tokens with the real C
// implementation. It needs a C compiler, so callers gate it behind HELIOS_NETCODE_INTEROP=1.
// Test-only: never import it from production code.
package cinterop

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"sync"
	"testing"
)

// Enabled reports whether the C interop checks were requested.
func Enabled() bool { return os.Getenv("HELIOS_NETCODE_INTEROP") == "1" }

// RepoRoot finds the Helios checkout this package was compiled from.
func RepoRoot(t testing.TB) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("cinterop: cannot locate source")
	}
	dir := filepath.Dir(file)
	for {
		if _, err := os.Stat(filepath.Join(dir, "third_party", "netcode", "netcode.c")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			t.Fatal("cinterop: third_party/netcode not found above " + filepath.Dir(file))
		}
		dir = parent
	}
}

var (
	buildOnce sync.Once
	buildExe  string
	buildErr  string
)

// Build compiles the harness once per test binary and returns its path.
func Build(t testing.TB) string {
	t.Helper()
	root := RepoRoot(t)
	buildOnce.Do(func() {
		cc := os.Getenv("CC")
		if cc == "" {
			for _, c := range []string{"cc", "gcc", "clang"} {
				if _, err := exec.LookPath(c); err == nil {
					cc = c
					break
				}
			}
		}
		if cc == "" {
			buildErr = "no C compiler found (set CC)"
			return
		}
		netcode := filepath.Join(root, "third_party", "netcode")
		src := filepath.Join(root, "services", "pkg", "connecttoken", "testdata", "interop", "netcode_interop.c")
		// Cache the binary by content hash of its sources, so repeated runs skip the ~2 s compile
		// and nothing accumulates in the temp directory.
		h := sha256.New()
		h.Write([]byte(cc + runtime.GOOS + runtime.GOARCH))
		for _, f := range []string{src, filepath.Join(netcode, "netcode.c"), filepath.Join(netcode, "netcode.h"),
			filepath.Join(netcode, "sodium", "sodium.c"), filepath.Join(netcode, "sodium", "sodium.h")} {
			b, err := os.ReadFile(f)
			if err != nil {
				buildErr = err.Error()
				return
			}
			h.Write(b)
		}
		cacheRoot, err := os.UserCacheDir()
		if err != nil {
			cacheRoot = os.TempDir()
		}
		dir := filepath.Join(cacheRoot, "helios", "netcode-interop", hex.EncodeToString(h.Sum(nil))[:16])
		if err := os.MkdirAll(dir, 0o755); err != nil {
			buildErr = err.Error()
			return
		}
		exe := filepath.Join(dir, "netcode_interop")
		if runtime.GOOS == "windows" {
			exe += ".exe"
		}
		if _, err := os.Stat(exe); err == nil {
			buildExe = exe
			return
		}
		// Build under a per-process name (keeping .exe on Windows) and rename, so concurrent test
		// binaries never run a half-written harness.
		tmp := filepath.Join(dir, "build-"+strconv.Itoa(os.Getpid())+"-"+filepath.Base(exe))
		args := []string{"-O1", "-I", netcode, "-I", filepath.Join(netcode, "sodium"), src,
			filepath.Join(netcode, "sodium", "sodium.c"), "-o", tmp}
		if runtime.GOOS == "windows" {
			args = append(args, "-lws2_32", "-liphlpapi", "-ladvapi32")
		} else {
			args = append(args, "-lm")
		}
		if out, err := exec.Command(cc, args...).CombinedOutput(); err != nil {
			buildErr = "building harness with " + cc + " failed: " + err.Error() + "\n" + string(out)
			return
		}
		if err := os.Rename(tmp, exe); err != nil {
			buildErr = err.Error()
			return
		}
		buildExe = exe
	})
	if buildErr != "" {
		t.Fatal(buildErr)
	}
	return buildExe
}

// Run executes the harness and returns stdout, failing the test on a non-zero exit.
func Run(t testing.TB, exe string, args ...string) []byte {
	t.Helper()
	cmd := exec.Command(exe, args...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("%s %v: %v\nstderr: %s\nstdout: %s", filepath.Base(exe), args, err, stderr.String(), out)
	}
	return out
}

// Try executes the harness and returns stdout and whether it exited successfully.
func Try(exe string, args ...string) ([]byte, bool) {
	out, err := exec.Command(exe, args...).Output()
	return out, err == nil
}
