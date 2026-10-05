package patchcdn_test

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/klauspost/compress/zstd"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/cdc/cdctest"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
	"github.com/PageMastr/scifi-test/services/pkg/patchcdn"
	"github.com/PageMastr/scifi-test/services/pkg/patchtrust"
)

var t0 = time.Unix(1790000000, 0)

func writeFiles(t *testing.T, dir string, files map[string][]byte) {
	t.Helper()
	for p, b := range files {
		full := filepath.Join(dir, filepath.FromSlash(p))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func countFiles(t *testing.T, dir string) int {
	n := 0
	_ = filepath.WalkDir(dir, func(_ string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			n++
		}
		return nil
	})
	return n
}

// devSetup creates dev keys and returns them with a verifier for their roots.
func devSetup(t *testing.T, target patchtrust.Target) (*patchcdn.Keys, *patchtrust.Verifier, string) {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "keys")
	keys, created, err := patchcdn.LoadOrCreateDevKeys(dir, target.ProductID, t0)
	if err != nil || !created || !keys.Dev {
		t.Fatalf("dev keys: %v %v", created, err)
	}
	product, rp, err := patchcdn.LoadRoots(filepath.Join(dir, "roots.json"))
	if err != nil || product != target.ProductID {
		t.Fatalf("roots: %q %v", product, err)
	}
	v, err := patchtrust.NewVerifier(target, rp, patchtrust.Options{})
	if err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" {
		for _, f := range []string{"root-keys.json", "manifest-key.json"} {
			if st, err := os.Stat(filepath.Join(dir, f)); err != nil || st.Mode().Perm() != 0o600 {
				t.Fatalf("%s: %v %v", f, st.Mode(), err)
			}
		}
	}
	return keys, v, dir
}

// Publish then verify, from a directory and over HTTP; an identical republish writes nothing; a changed
// build writes only its new chunks, with the next sequence; an install's ratchet then refuses the old pointer.
func TestPublishVerifyRepublish(t *testing.T) {
	target := patchtrust.Target{ProductID: "sample-game", Channel: "dev", Platform: "linux64"}
	keys, v, _ := devSetup(t, target)
	build, cdn := t.TempDir(), t.TempDir()
	writeFiles(t, build, map[string][]byte{
		"bin/client":         cdctest.Random(1, 300<<10),
		"content/zone1.hpak": cdctest.Random(2, 700<<10),
		"content/zone2.hpak": cdctest.Random(2, 700<<10), // the same bytes: deduplicated
		"empty":              nil,
	})
	if runtime.GOOS != "windows" {
		if err := os.Chmod(filepath.Join(build, "bin", "client"), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	opts := patchcdn.PublishOptions{CDNRoot: cdn, BuildDir: build, Target: target, CompatEpoch: 2, Now: t0,
		CDNHosts: []string{"http://127.0.0.1:7700/cdn"}, RolloutPct: 100}
	ctx := context.Background()
	r1, err := patchcdn.Publish(ctx, opts, keys)
	if err != nil {
		t.Fatal(err)
	}
	if r1.Sequence != 1 || !r1.ManifestWritten || !r1.PointerWritten || r1.Files != 4 || r1.ChunksWritten != r1.Chunks ||
		len(r1.BuildID) != 16 {
		t.Fatalf("first publish: %+v", r1)
	}
	objects := countFiles(t, filepath.Join(cdn, "chunks"))
	if objects != r1.Chunks {
		t.Fatalf("%d chunk objects for %d chunks", objects, r1.Chunks)
	}

	store := &patchcdn.MemoryStateStore{}
	res, err := patchcdn.Verify(ctx, patchcdn.DirSource{Root: cdn}, v, uint64(t0.Unix())+60, store)
	if err != nil {
		t.Fatal(err)
	}
	m := res.Manifest
	ci, zi := m.FindFile("bin/client"), m.FindFile("content/zone1.hpak")
	if ci < 0 || zi < 0 || m.Files[ci].Tier != 0 || m.Files[zi].Tier != 1 ||
		(runtime.GOOS != "windows" && m.Files[ci].Flags&manifest.FlagExecutable == 0) {
		t.Fatalf("tiers or flags: %+v", m.Files)
	}
	if res.Chunks != r1.Chunks || res.RawBytes != (300+700)<<10 || store.State.PointerSequence != 1 {
		t.Fatalf("verify: %d chunks, %d bytes, state %+v", res.Chunks, res.RawBytes, store.State)
	}
	srv := httptest.NewServer(http.FileServer(http.Dir(cdn)))
	defer srv.Close()
	if _, err := patchcdn.Verify(ctx, patchcdn.HTTPSource{Base: srv.URL}, v, uint64(t0.Unix())+60,
		&patchcdn.MemoryStateStore{}); err != nil {
		t.Fatalf("over HTTP: %v", err)
	}

	// Republishing the same build writes nothing.
	before := snapshot(t, cdn)
	r2, err := patchcdn.Publish(ctx, func() patchcdn.PublishOptions { o := opts; o.Now = t0.Add(time.Hour); return o }(), keys)
	if err != nil || r2.ManifestWritten || r2.PointerWritten || r2.ChunksWritten != 0 || r2.Sequence != 1 ||
		r2.BuildID != r1.BuildID {
		t.Fatalf("republish: %+v %v", r2, err)
	}
	if after := snapshot(t, cdn); !equalSnapshots(before, after) {
		t.Fatal("an identical republish changed the CDN")
	}
	// Past half its lifetime the pointer is re-signed with the next sequence.
	r3, err := patchcdn.Publish(ctx, func() patchcdn.PublishOptions { o := opts; o.Now = t0.Add(4 * 24 * time.Hour); return o }(), keys)
	if err != nil || r3.ManifestWritten || !r3.PointerWritten || r3.ChunksWritten != 0 || r3.Sequence != 2 {
		t.Fatalf("refresh: %+v %v", r3, err)
	}

	// A changed build: one file differs, so only its chunks are new.
	writeFiles(t, build, map[string][]byte{"content/zone1.hpak": append(cdctest.Random(2, 700<<10), "patch"...)})
	o4 := opts
	o4.Now = t0.Add(5 * 24 * time.Hour)
	r4, err := patchcdn.Publish(ctx, o4, keys)
	if err != nil || r4.BuildID == r1.BuildID || r4.Sequence != 3 || r4.ChunksWritten == 0 || r4.ChunksWritten > 3 {
		t.Fatalf("changed build: %+v %v", r4, err)
	}
	now := uint64(o4.Now.Unix()) + 60
	res, err = patchcdn.Verify(ctx, patchcdn.DirSource{Root: cdn}, v, now, store)
	if err != nil || res.Pointer.BuildID != r4.BuildID || store.State.PointerSequence != 3 {
		t.Fatalf("verify the new build: %v", err)
	}

	// An install that saw sequence 4 refuses sequence 3 (no rollback flag).
	p3, _ := os.ReadFile(filepath.Join(cdn, filepath.FromSlash(patchcdn.PointerPath(target.ProductID, target.Channel,
		target.Platform))))
	if _, err := v.VerifyPointer(p3, res.Keyset, now, patchtrust.State{PointerSequence: 4}); patchtrust.CheckOf(err) !=
		patchtrust.CheckPointerSequence {
		t.Fatalf("rolled back: %v", err)
	}
}

func snapshot(t *testing.T, dir string) map[string][]byte {
	out := map[string][]byte{}
	_ = filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			rel, _ := filepath.Rel(dir, p)
			b, _ := os.ReadFile(p)
			out[filepath.ToSlash(rel)] = b
		}
		return nil
	})
	return out
}

func equalSnapshots(a, b map[string][]byte) bool {
	if len(a) != len(b) {
		return false
	}
	for k, v := range a {
		if !bytes.Equal(v, b[k]) {
			return false
		}
	}
	return true
}

func TestPublishRefusals(t *testing.T) {
	target := patchtrust.Target{ProductID: "sample-game", Channel: "dev", Platform: "win64"}
	keys, _, _ := devSetup(t, target)
	build, cdn := t.TempDir(), t.TempDir()
	writeFiles(t, build, map[string][]byte{"a.txt": []byte("a")})
	base := patchcdn.PublishOptions{CDNRoot: cdn, BuildDir: build, Target: target, Now: t0, BuildID: "fixed-1"}
	ctx := context.Background()
	for name, mod := range map[string]func(*patchcdn.PublishOptions){
		"dev keys for live": func(o *patchcdn.PublishOptions) { o.Target.Channel = "live" },
		"dev keys for a non-loopback host": func(o *patchcdn.PublishOptions) {
			o.CDNHosts = []string{"https://cdn.example"}
		},
		"dev keys for a remote host behind localhost userinfo": func(o *patchcdn.PublishOptions) {
			o.CDNHosts = []string{"http://localhost:1@evil.example/cdn"}
		},
		"dev keys for a remote host behind 127.0.0.1 userinfo": func(o *patchcdn.PublishOptions) {
			o.CDNHosts = []string{"http://127.0.0.1:80@203.0.113.9/"}
		},
		"another product":   func(o *patchcdn.PublishOptions) { o.Target.ProductID = "other-game" },
		"no clock":          func(o *patchcdn.PublishOptions) { o.Now = time.Time{} },
		"a bad build ID":    func(o *patchcdn.PublishOptions) { o.BuildID = "../escape" },
		"a lifetime of 8 d": func(o *patchcdn.PublishOptions) { o.Lifetime = 8 * 24 * time.Hour },
		"the key expired":   func(o *patchcdn.PublishOptions) { o.Now = t0.Add(patchcdn.DevKeyLifetime) },
		"a bad path":        func(o *patchcdn.PublishOptions) { writeFiles(t, o.BuildDir, map[string][]byte{"CON.txt": nil}) },
		"a missing build":   func(o *patchcdn.PublishOptions) { o.BuildDir = filepath.Join(o.BuildDir, "nope") },
	} {
		o := base
		o.BuildDir = t.TempDir()
		writeFiles(t, o.BuildDir, map[string][]byte{"a.txt": []byte("a")})
		mod(&o)
		if _, err := patchcdn.Publish(ctx, o, keys); err == nil {
			t.Errorf("%s: published", name)
		}
	}
	// A build ID is immutable: the same ID with other content is refused.
	if _, err := patchcdn.Publish(ctx, base, keys); err != nil {
		t.Fatal(err)
	}
	writeFiles(t, build, map[string][]byte{"a.txt": []byte("b")})
	if _, err := patchcdn.Publish(ctx, base, keys); err == nil || !strings.Contains(err.Error(), "already published") {
		t.Fatalf("republished build fixed-1 with other content: %v", err)
	}
	if runtime.GOOS != "windows" {
		link := t.TempDir()
		writeFiles(t, link, map[string][]byte{"a.txt": []byte("a")})
		if err := os.Symlink(filepath.Join(link, "a.txt"), filepath.Join(link, "b.txt")); err != nil {
			t.Fatal(err)
		}
		o := base
		o.BuildDir, o.BuildID = link, ""
		if _, err := patchcdn.Publish(ctx, o, keys); err == nil {
			t.Error("published a symbolic link")
		}
	}
	// Non-dev keys are not marked dev and refused by LoadOrCreateDevKeys.
	nondev := t.TempDir()
	writeSigningDir(t, nondev, signKeyset(t, baseKeyset(), "root-1"), "manifest-a", false)
	if _, _, err := patchcdn.LoadOrCreateDevKeys(nondev, vProduct, t0); err == nil {
		t.Error("loaded production keys as dev keys")
	}
	if _, err := patchcdn.LoadKeys(nondev, "other-game"); err == nil {
		t.Error("loaded keys for another product")
	}
	newsDir := t.TempDir()
	writeSigningDir(t, newsDir, signKeyset(t, baseKeyset(), "root-1"), "news-a", false)
	if _, err := patchcdn.LoadKeys(newsDir, vProduct); err == nil {
		t.Error("loaded a news key as the manifest key")
	}
}

func TestChunkObjects(t *testing.T) {
	raw := cdctest.Random(5, 40000)
	stored, err := patchcdn.EncodeChunk(raw)
	if err != nil {
		t.Fatal(err)
	}
	if back, err := patchcdn.DecodeChunk(stored, uint32(len(raw))); err != nil || !bytes.Equal(back, raw) {
		t.Fatalf("round trip: %v", err)
	}
	for name, c := range map[string]struct {
		stored []byte
		size   uint32
	}{
		"longer than rawSize":  {stored, uint32(len(raw)) - 1},
		"shorter than rawSize": {stored, uint32(len(raw)) + 1},
		"not zstd":             {[]byte("hello"), 5},
		"empty":                {nil, 1},
		"truncated":            {stored[:len(stored)/2], uint32(len(raw))},
		"two frames":           {append(bytes.Clone(stored), stored...), uint32(len(raw))},
	} {
		if _, err := patchcdn.DecodeChunk(c.stored, c.size); err == nil {
			t.Errorf("%s: decoded", name)
		}
	}
	// A frame may declare a window of at most cdc.MaxSize (256 KiB), as engine/patch: a hand-made frame with
	// no content size, a window descriptor and one raw block.
	windowFrame := func(log uint) []byte {
		f := []byte{0x28, 0xB5, 0x2F, 0xFD, 0x00, byte((log - 10) << 3)}
		h := uint32(len(raw))<<3 | 1
		return append(append(f, byte(h), byte(h>>8), byte(h>>16)), raw...)
	}
	if back, err := patchcdn.DecodeChunk(windowFrame(18), uint32(len(raw))); err != nil || !bytes.Equal(back, raw) {
		t.Errorf("a 256 KiB window: %v", err)
	}
	if _, err := patchcdn.DecodeChunk(windowFrame(20), uint32(len(raw))); err == nil {
		t.Error("decoded a frame that declares a 1 MiB window")
	}
	// A frame that claims a huge window or content is refused without allocating it.
	enc, _ := zstd.NewWriter(nil, zstd.WithWindowSize(1<<26), zstd.WithSingleSegment(false))
	bomb := enc.EncodeAll(make([]byte, 64<<20), nil)
	enc.Close()
	if _, err := patchcdn.DecodeChunk(bomb, cdc.MaxSize); err == nil {
		t.Error("decoded 64 MiB into a chunk")
	}
}

func TestSources(t *testing.T) {
	dir := t.TempDir()
	writeFiles(t, dir, map[string][]byte{"keys/p/keyset.json": []byte("12345")})
	ctx := context.Background()
	src := patchcdn.DirSource{Root: dir}
	if b, err := src.Fetch(ctx, "keys/p/keyset.json", 5); err != nil || string(b) != "12345" {
		t.Fatalf("%q %v", b, err)
	}
	if _, err := src.Fetch(ctx, "keys/p/keyset.json", 4); err == nil {
		t.Error("read past the limit")
	}
	if _, err := src.Fetch(ctx, "keys/p/none.json", 4); !errors.Is(err, patchcdn.ErrNotFound) {
		t.Errorf("missing: %v", err)
	}
	for _, bad := range []string{"", "/etc/passwd", "../x", "a/../../x", "a//b", `a\b`, "c:/x", "./a"} {
		if _, err := src.Fetch(ctx, bad, 10); err == nil || errors.Is(err, patchcdn.ErrNotFound) {
			t.Errorf("path %q: %v", bad, err)
		}
	}
	srv := httptest.NewServer(http.FileServer(http.Dir(dir)))
	defer srv.Close()
	h := patchcdn.HTTPSource{Base: srv.URL + "/"}
	if b, err := h.Fetch(ctx, "keys/p/keyset.json", 5); err != nil || string(b) != "12345" {
		t.Fatalf("%q %v", b, err)
	}
	if _, err := h.Fetch(ctx, "keys/p/keyset.json", 4); err == nil {
		t.Error("HTTP read past the limit")
	}
	if _, err := h.Fetch(ctx, "keys/p/none.json", 4); !errors.Is(err, patchcdn.ErrNotFound) {
		t.Errorf("HTTP missing: %v", err)
	}
	if patchcdn.ChunkPath(cdc.Sum(nil)) != "chunks/0e/57/0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8.zst" {
		t.Errorf("chunk path %s", patchcdn.ChunkPath(cdc.Sum(nil)))
	}
}

// signingDir writes a non-dev signing directory for vector-game: keyset k signed by root, subkey manifest-a.
func signingDir(t *testing.T, k *patchtrust.Keyset, root string) *patchcdn.Keys {
	t.Helper()
	dir := t.TempDir()
	writeSigningDir(t, dir, signKeyset(t, k, root), "manifest-a", false)
	keys, err := patchcdn.LoadKeys(dir, vProduct)
	if err != nil {
		t.Fatal(err)
	}
	return keys
}

// Publish never replaces the CDN's keyset with a lower version (which would undo the revocations of the
// versions between), other bytes of the same version, or a lower root epoch.
func TestPublishKeysetNeverDowngrades(t *testing.T) {
	cdn, build := t.TempDir(), t.TempDir()
	writeFiles(t, build, map[string][]byte{"bin/game": cdctest.Random(3, 9000)})
	ctx := context.Background()
	o := patchcdn.PublishOptions{CDNRoot: cdn, BuildDir: build, Target: vTarget, Now: t0}
	kpath := filepath.Join(cdn, filepath.FromSlash(patchcdn.KeysetPath(vProduct)))
	version := func(v uint64, epoch uint32, extra bool) *patchtrust.Keyset {
		k := baseKeyset()
		k.Version, k.RootEpoch = v, epoch
		if extra {
			k.Keys = sortKeys(append(k.Keys, subkey("manifest-next", patchtrust.RoleManifest, vT0+60*vDay, vT0+150*vDay)))
		}
		return k
	}
	publish := func(k *patchtrust.Keyset, root string) (*patchcdn.PublishResult, error) {
		keys := signingDir(t, k, root)
		before, _ := os.ReadFile(kpath)
		res, err := patchcdn.Publish(ctx, o, keys)
		after, _ := os.ReadFile(kpath)
		if err != nil && !bytes.Equal(before, after) {
			t.Fatalf("a refused publish changed the CDN's keyset: %v", err)
		}
		return res, err
	}
	if res, err := publish(version(5, 1, false), "root-1"); err != nil || !res.KeysetWritten {
		t.Fatalf("v5: %+v %v", res, err)
	}
	if res, err := publish(version(5, 1, false), "root-1"); err != nil || res.KeysetWritten {
		t.Fatalf("v5 again: %+v %v", res, err)
	}
	for name, c := range map[string]struct {
		k    *patchtrust.Keyset
		root string
		want string
	}{
		"a lower version":                  {version(3, 1, false), "root-1", "would undo"},
		"the same version with other keys": {version(5, 1, true), "root-1", "needs a higher version"},
	} {
		if _, err := publish(c.k, c.root); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v", name, err)
		}
	}
	if res, err := publish(version(6, 2, false), "root-2"); err != nil || !res.KeysetWritten {
		t.Fatalf("v6 of the next root: %+v %v", res, err)
	}
	if _, err := publish(version(7, 1, false), "root-1"); err == nil || !strings.Contains(err.Error(), "root epoch") {
		t.Errorf("a lower root epoch: %v", err)
	}
	if err := os.WriteFile(kpath, []byte("{"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := publish(version(8, 2, false), "root-2"); err == nil || !strings.Contains(err.Error(), "does not parse") {
		t.Errorf("a corrupt CDN keyset: %v", err)
	}
}

// With the root pair (Roots, from the signing directory's roots.json), publish refuses a keyset the roots
// did not sign.
func TestPublishChecksKeysetAgainstRoots(t *testing.T) {
	target := patchtrust.Target{ProductID: "sample-game", Channel: "dev", Platform: "win64"}
	keys, _, dir := devSetup(t, target)
	_, rp, err := patchcdn.LoadRoots(filepath.Join(dir, "roots.json"))
	if err != nil {
		t.Fatal(err)
	}
	build := t.TempDir()
	writeFiles(t, build, map[string][]byte{"a.txt": []byte("a")})
	o := patchcdn.PublishOptions{CDNRoot: t.TempDir(), BuildDir: build, Target: target, Now: t0, Roots: &rp}
	if _, err := patchcdn.Publish(context.Background(), o, keys); err != nil {
		t.Fatal(err)
	}
	_, other, _ := ed25519.GenerateKey(nil)
	ks := *keys.Keyset
	ks.Version++
	if err := ks.Sign(other); err != nil {
		t.Fatal(err)
	}
	forged := *keys
	forged.Keyset = &ks
	if forged.KeysetBytes, err = ks.Marshal(); err != nil {
		t.Fatal(err)
	}
	if _, err := patchcdn.Publish(context.Background(), o, &forged); patchtrust.CheckOf(err) !=
		patchtrust.CheckKeysetSignature {
		t.Fatalf("published a keyset the roots did not sign: %v", err)
	}
}

// A chunk object already on the CDN that does not decode to its chunk is rewritten by the next publish,
// instead of being signed into another manifest.
func TestPublishRepairsChunkObjects(t *testing.T) {
	target := patchtrust.Target{ProductID: "sample-game", Channel: "dev", Platform: "win64"}
	keys, v, _ := devSetup(t, target)
	build, cdn := t.TempDir(), t.TempDir()
	writeFiles(t, build, map[string][]byte{"bin/game": cdctest.Random(4, 200<<10), "data": cdctest.Random(5, 300<<10)})
	ctx := context.Background()
	o := patchcdn.PublishOptions{CDNRoot: cdn, BuildDir: build, Target: target, Now: t0}
	r1, err := patchcdn.Publish(ctx, o, keys)
	if err != nil || r1.Chunks < 3 {
		t.Fatalf("%+v %v", r1, err)
	}
	var objects []string
	_ = filepath.WalkDir(filepath.Join(cdn, "chunks"), func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			objects = append(objects, p)
		}
		return nil
	})
	flipped, _ := os.ReadFile(objects[0])
	flipped[len(flipped)/2] ^= 0x40
	truncated, _ := os.ReadFile(objects[1])
	if err := os.WriteFile(objects[0], flipped, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(objects[1], truncated[:len(truncated)-1], 0o644); err != nil {
		t.Fatal(err)
	}
	now := uint64(t0.Unix()) + 60
	if _, err := patchcdn.Verify(ctx, patchcdn.DirSource{Root: cdn}, v, now, &patchcdn.MemoryStateStore{}); err == nil {
		t.Fatal("verified corrupt chunk objects")
	}
	r2, err := patchcdn.Publish(ctx, o, keys)
	if err != nil || r2.ChunksRepaired != 2 || r2.ChunksWritten != 2 || r2.ManifestWritten {
		t.Fatalf("republish: %+v %v", r2, err)
	}
	if _, err := patchcdn.Verify(ctx, patchcdn.DirSource{Root: cdn}, v, now, &patchcdn.MemoryStateStore{}); err != nil {
		t.Fatalf("after the repair: %v", err)
	}
	if r3, err := patchcdn.Publish(ctx, o, keys); err != nil || r3.ChunksWritten != 0 || r3.ChunksRepaired != 0 {
		t.Fatalf("a third publish: %+v %v", r3, err)
	}
}

// Dev key directories ignore themselves in git, wherever they are, before any seed is written; a directory
// made without the file gets it on the next load.
func TestDevKeysAreGitIgnored(t *testing.T) {
	repo := t.TempDir()
	dir := filepath.Join(repo, "tools", "helios-data", "keys", "patch", "sample-game")
	if err := patchcdn.CreateDevKeys(dir, "sample-game", t0, nil); err != nil {
		t.Fatal(err)
	}
	if b, err := os.ReadFile(filepath.Join(dir, ".gitignore")); err != nil || !strings.Contains(string(b), "\n*\n") {
		t.Fatalf(".gitignore: %q %v", b, err)
	}
	if err := os.Remove(filepath.Join(dir, ".gitignore")); err != nil {
		t.Fatal(err)
	}
	if _, _, err := patchcdn.LoadOrCreateDevKeys(dir, "sample-game", t0); err != nil {
		t.Fatal(err)
	}
	git, err := exec.LookPath("git")
	if err != nil {
		t.Skip("git is not installed; the .gitignore file was checked")
	}
	if out, err := exec.Command(git, "init", "-q", repo).CombinedOutput(); err != nil {
		t.Fatalf("git init: %v %s", err, out)
	}
	for _, f := range []string{"root-keys.json", "manifest-key.json", "keyset.json", "roots.json", ".gitignore"} {
		cmd := exec.Command(git, "-C", repo, "check-ignore", "-q", filepath.Join(dir, f))
		if err := cmd.Run(); err != nil {
			t.Errorf("git does not ignore %s: %v", f, err)
		}
	}
}
