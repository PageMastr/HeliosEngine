package patchcdn_test

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/cdc/cdctest"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
	"github.com/PageMastr/scifi-test/services/pkg/patchcdn"
	"github.com/PageMastr/scifi-test/services/pkg/patchtrust"
	"github.com/PageMastr/scifi-test/services/pkg/patchtrust/trusttest"
)

// The shared trust vectors (services/testdata/vectors/trust/): engine/patch's doctest
// (tests/test_trust.cpp) runs the same cases, so both verifiers accept and reject the same artefacts with
// the same check. TestUpdateTrustVectors (-update) regenerates them from the test-only keys of package
// trusttest; Ed25519 signing is deterministic, so only a change of zstd encoder output changes the files.
var (
	trustDir     = filepath.Join("..", "..", "testdata", "vectors", "trust")
	updateVector = flag.Bool("update", false, "rewrite services/testdata/vectors/trust/")
)

// The vectors' world (README "Shared vectors"): product vector-game, channel live, platform win64, now T0.
const (
	vT0       = 1790000000
	vDay      = 86400
	vProduct  = "vector-game"
	vChannel  = "live"
	vPlatform = "win64"
	vBuild    = "2026.09.21-r1"
	vEpoch    = 4
)

var vTarget = patchtrust.Target{ProductID: vProduct, Channel: vChannel, Platform: vPlatform}

// vectorFile is cases.json.
type vectorFile struct {
	Comment  string           `json:"comment"`
	Product  string           `json:"product"`
	Channel  string           `json:"channel"`
	Platform string           `json:"platform"`
	Roots    vectorRoots      `json:"roots"`
	Now      uint64           `json:"now"`
	State    patchtrust.State `json:"state"`
	Cases    []vectorCase     `json:"cases"`
}

type vectorRoots struct {
	Epoch   uint32 `json:"epoch"`
	Current string `json:"current"`
	Next    string `json:"next"`
}

type vectorEdit struct {
	Artefact string `json:"artefact"`        // keyset, pointer, manifest or chunk
	Chunk    string `json:"chunk,omitempty"` // the chunk ID, for artefact chunk
	At       int    `json:"at"`
	Hex      string `json:"hex"`
	Insert   bool   `json:"insert,omitempty"`
}

type vectorCase struct {
	Name      string            `json:"name"`
	Check     string            `json:"check"`         // "" = accepted
	Now       uint64            `json:"now,omitempty"` // replaces the file's now
	State     *patchtrust.State `json:"state,omitempty"`
	Keyset    string            `json:"keyset,omitempty"`   // a file in trust/ replacing the CDN's
	Pointer   string            `json:"pointer,omitempty"`  // likewise
	Manifest  string            `json:"manifest,omitempty"` // replaces whatever manifest the pointer names
	Chunks    map[string]string `json:"chunks,omitempty"`   // chunk ID -> replacement file, "" = missing
	Edits     []vectorEdit      `json:"edits,omitempty"`
	WantState *patchtrust.State `json:"wantState,omitempty"` // the ratchets after an accepted case
}

type rawSyntaxCase struct {
	name string
	doc  []byte
}

// syntaxCase is a document made from the CDN's keyset or pointer: bytes [At, At+Delete) replaced by Put (or
// PutHex, for bytes outside printable ASCII), then padded with spaces up to PadTo bytes.
type syntaxCase struct {
	Name   string `json:"name"`
	At     int    `json:"at"`
	Delete int    `json:"delete"`
	Put    string `json:"put,omitempty"`
	PutHex string `json:"putHex,omitempty"`
	PadTo  int    `json:"padTo,omitempty"`
}

func (c syntaxCase) apply(base []byte) []byte {
	put := []byte(c.Put)
	if c.PutHex != "" {
		put, _ = hex.DecodeString(c.PutHex)
	}
	b := append(append(slices.Clone(base[:c.At]), put...), base[c.At+c.Delete:]...)
	if n := c.PadTo - len(b); n > 0 {
		b = append(b, bytes.Repeat([]byte(" "), n)...)
	}
	return b
}

// splice describes doc as one replacement in base.
func splice(name string, base, doc []byte) syntaxCase {
	a := 0
	for a < len(base) && a < len(doc) && base[a] == doc[a] {
		a++
	}
	b := 0
	for b < len(base)-a && b < len(doc)-a && base[len(base)-1-b] == doc[len(doc)-1-b] {
		b++
	}
	c := syntaxCase{Name: name, At: a, Delete: len(base) - b - a}
	put := doc[a : len(doc)-b]
	if printable := !bytes.ContainsFunc(put, func(r rune) bool { return r < 0x20 || r > 0x7E }); printable {
		c.Put = string(put)
	} else {
		c.PutHex = hex.EncodeToString(put)
	}
	return c
}

type syntaxFile struct {
	Comment string       `json:"comment"`
	Keyset  []syntaxCase `json:"keyset"`
	Pointer []syntaxCase `json:"pointer"`
}

func key(name string) ed25519.PrivateKey { return trusttest.Key(name) }

func pub(name string) patchtrust.PublicKey { return patchtrust.PublicKeyOf(key(name)) }

func hexKey(name string) string {
	p := pub(name)
	return hex.EncodeToString(p[:])
}

func subkey(name, role string, from, to uint64) patchtrust.KeysetKey {
	p := pub(name)
	return patchtrust.KeysetKey{ID: patchtrust.Fingerprint(p), Role: role, Pub: p, NotBefore: from, NotAfter: to}
}

func sortKeys(ks []patchtrust.KeysetKey) []patchtrust.KeysetKey {
	sort.Slice(ks, func(i, j int) bool { return bytes.Compare(ks[i].ID[:], ks[j].ID[:]) < 0 })
	return ks
}

// baseKeyset is keyset v3 of root epoch 1: manifest-a (current), manifest-old (expired 100 days ago),
// news-a, and store-a of a role this version does not know.
func baseKeyset() *patchtrust.Keyset {
	return &patchtrust.Keyset{ProductID: vProduct, Version: 3, RootEpoch: 1, Keys: sortKeys([]patchtrust.KeysetKey{
		subkey("manifest-a", patchtrust.RoleManifest, vT0-90*vDay, vT0+90*vDay),
		subkey("manifest-old", patchtrust.RoleManifest, vT0-200*vDay, vT0-100*vDay),
		subkey("news-a", patchtrust.RoleNews, vT0-90*vDay, vT0+90*vDay),
		subkey("store-a", "store", vT0-90*vDay, vT0+90*vDay),
	})}
}

func signKeyset(t *testing.T, k *patchtrust.Keyset, root string) []byte {
	t.Helper()
	if err := k.Sign(key(root)); err != nil {
		t.Fatal(err)
	}
	b, err := k.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func signPointer(t *testing.T, p *patchtrust.Pointer, signer string) []byte {
	t.Helper()
	if err := p.Sign(key(signer)); err != nil {
		t.Fatal(err)
	}
	b, err := p.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// buildDir writes the vectors' build: a tier-0 executable of random bytes (one chunk), a pak of a repeated
// unit (chunks that repeat, so the CDN stores fewer than the files reference), a text file and an empty file.
func buildDir(t *testing.T) string {
	dir := t.TempDir()
	files := map[string][]byte{
		"bin/game.exe":      cdctest.Random(1, 4<<10),
		"content/data.hpak": bytes.Repeat(cdctest.Random(2, 3000), 90),
		"readme.txt":        []byte("Vector Game: test build for the shared trust vectors.\n"),
		"saved/empty.dat":   nil,
	}
	for p, b := range files {
		full := filepath.Join(dir, filepath.FromSlash(p))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func basePointer(ref patchtrust.ManifestRef) *patchtrust.Pointer {
	return &patchtrust.Pointer{ProductID: vProduct, Channel: vChannel, Platform: vPlatform, ManifestRef: ref,
		Sequence: 7, MinLauncher: "1.2.0", MinClient: "1.2.0",
		CDNHosts: []string{"https://cdn1.vector-game.example", "https://cdn2.vector-game.example"},
		Next: &patchtrust.Next{ManifestRef: patchtrust.ManifestRef{BuildID: "2026.09.28-r1",
			ManifestHash: cdc.Sum([]byte("next build")), CompatEpoch: vEpoch + 1}, AvailableAt: vT0 + 7*vDay},
		RolloutPct: 100, SignedAt: vT0 - 600, Expires: vT0 + 6*vDay}
}

// signedManifest re-signs the base manifest with header changes, signed by signer.
func signedManifest(t *testing.T, base *manifest.Manifest, signer string, mod func(*manifest.Header)) []byte {
	t.Helper()
	m := *base
	m.Header.KeyID = patchtrust.Fingerprint(pub(signer))
	if mod != nil {
		mod(&m.Header)
	}
	b, err := m.Marshal(manifest.WriteOptions{Codec: manifest.CodecZstd, ZstdLevel: 19})
	if err != nil {
		t.Fatal(err)
	}
	if err := patchtrust.SignManifest(b, key(signer)); err != nil {
		t.Fatal(err)
	}
	return b
}

func headerHash(t *testing.T, file []byte) cdc.Hash {
	info, err := manifest.ParseHeader(file, 0)
	if err != nil {
		t.Fatal(err)
	}
	return info.HeaderHash
}

func TestUpdateTrustVectors(t *testing.T) {
	if !*updateVector {
		t.Skip("run with -update to rewrite services/testdata/vectors/trust/")
	}
	if err := os.RemoveAll(trustDir); err != nil {
		t.Fatal(err)
	}
	cdn := filepath.Join(trustDir, "cdn")
	ks := baseKeyset()
	ksBytes := signKeyset(t, ks, "root-1")
	// A previous pointer (sequence 6) so that publish writes sequence 7.
	prev := basePointer(patchtrust.ManifestRef{BuildID: "2026.09.14-r1", ManifestHash: cdc.Sum([]byte("previous")),
		CompatEpoch: vEpoch})
	prev.Sequence, prev.Next = 6, nil
	if err := patchtrust.WriteFileAtomic(filepath.Join(cdn, filepath.FromSlash(patchcdn.PointerPath(vProduct, vChannel,
		vPlatform))), signPointer(t, prev, "manifest-a"), 0o644); err != nil {
		t.Fatal(err)
	}
	keysDir := t.TempDir()
	writeSigningDir(t, keysDir, ksBytes, "manifest-a", false)
	keys, err := patchcdn.LoadKeys(keysDir, vProduct)
	if err != nil {
		t.Fatal(err)
	}
	res, err := patchcdn.Publish(context.Background(), patchcdn.PublishOptions{CDNRoot: cdn, BuildDir: buildDir(t),
		Target: vTarget, BuildID: vBuild, CompatEpoch: vEpoch, MinLauncher: "1.2.0", MinClient: "1.2.0",
		CDNHosts: basePointer(patchtrust.ManifestRef{}).CDNHosts, RolloutPct: 100, Now: time.Unix(vT0-600, 0),
		Lifetime: 6*24*time.Hour + 600*time.Second}, keys)
	if err != nil || res.Sequence != 7 || !res.ManifestWritten {
		t.Fatalf("publish: %+v %v", res, err)
	}
	mfile, err := os.ReadFile(filepath.Join(cdn, filepath.FromSlash(patchcdn.ManifestPath(vProduct, vBuild, vPlatform))))
	if err != nil {
		t.Fatal(err)
	}
	m, err := manifest.Parse(mfile, 0)
	if err != nil {
		t.Fatal(err)
	}
	ref := patchtrust.ManifestRef{BuildID: vBuild, ManifestHash: res.ManifestHash, CompatEpoch: vEpoch}
	// The CDN's pointer: publish's, plus a pre-download "next" (optional members in the canonical form).
	base := basePointer(ref)
	write := func(name string, b []byte) string {
		if err := os.WriteFile(filepath.Join(trustDir, name), b, 0o644); err != nil {
			t.Fatal(err)
		}
		return name
	}
	if err := os.WriteFile(filepath.Join(cdn, filepath.FromSlash(patchcdn.PointerPath(vProduct, vChannel, vPlatform))),
		signPointer(t, base, "manifest-a"), 0o644); err != nil {
		t.Fatal(err)
	}

	keyset := func(name, root string, mod func(*patchtrust.Keyset)) string {
		k := baseKeyset()
		mod(k)
		return write(name, signKeyset(t, k, root))
	}
	pointer := func(name, signer string, mod func(*patchtrust.Pointer)) string {
		p := basePointer(ref)
		mod(p)
		return write(name, signPointer(t, p, signer))
	}
	// manifestCase writes a manifest variant and a pointer (signed by manifest-a) naming it.
	manifestCase := func(name, signer string, mod func(*manifest.Header)) (string, string) {
		b := signedManifest(t, m, signer, mod)
		mf := write(name+".hman", b)
		pf := pointer(name+"-pointer.json", "manifest-a", func(p *patchtrust.Pointer) {
			p.ManifestHash = headerHash(t, b)
		})
		return mf, pf
	}
	st := func(root uint32, version, seq uint64) *patchtrust.State {
		return &patchtrust.State{RootEpoch: root, KeysetVersion: version, PointerSequence: seq}
	}
	pointerFile := patchcdn.PointerPath(vProduct, vChannel, vPlatform)
	cdnPointer, _ := os.ReadFile(filepath.Join(cdn, filepath.FromSlash(pointerFile)))
	cdnKeyset := ksBytes
	at := func(doc []byte, s string) int {
		i := bytes.Index(doc, []byte(s))
		if i < 0 {
			t.Fatalf("%q not found", s)
		}
		return i
	}
	// The chunk stored in the most bytes is game.exe's random one: zstd stores it raw, so random bytes of the
	// same length make a substitute of the same stored size.
	big := m.Chunks[0]
	for _, c := range m.Chunks {
		if c.StoredSize > big.StoredSize {
			big = c
		}
	}
	bigID := hex.EncodeToString(big.Hash[:])
	substitute, err := patchcdn.EncodeChunk(cdctest.Random(99, int(big.RawSize)))
	if err != nil || len(substitute) != int(big.StoredSize) {
		t.Fatalf("substitute chunk: %d bytes, want %d (%v)", len(substitute), big.StoredSize, err)
	}
	oversize, _ := patchcdn.EncodeChunk(cdctest.Random(98, int(big.RawSize)+1))
	mfOff := len(mfile) - 40 // a byte inside the zstd payload

	nextMf, nextPf := manifestCase("manifest-swapped", "manifest-a", func(h *manifest.Header) { h.CreatedAt-- })
	_ = nextPf
	var cases []vectorCase
	add := func(c vectorCase) { cases = append(cases, c) }
	ok := func(name string, state, want *patchtrust.State, c vectorCase) {
		c.Name, c.State, c.WantState = name, state, want
		add(c)
	}
	bad := func(name string, check patchtrust.Check, c vectorCase) {
		c.Name, c.Check = name, string(check)
		add(c)
	}

	// Accepted.
	ok("valid", nil, st(1, 3, 7), vectorCase{})
	ok("valid-fresh-install", st(0, 0, 0), st(1, 3, 7), vectorCase{})
	ok("valid-same-sequence", st(1, 3, 7), st(1, 3, 7), vectorCase{})
	ok("rollback-honoured", st(1, 3, 9), st(1, 3, 5), vectorCase{Pointer: pointer("pointer-rollback.json",
		"manifest-a", func(p *patchtrust.Pointer) { p.Sequence, p.Rollback = 5, true })})
	ok("rollback-flag-at-higher-sequence", nil, st(1, 3, 8), vectorCase{Pointer: pointer(
		"pointer-rollback-higher.json", "manifest-a", func(p *patchtrust.Pointer) { p.Sequence, p.Rollback = 8, true })})
	nextRoot := keyset("keyset-next-root.json", "root-2", func(k *patchtrust.Keyset) { k.Version, k.RootEpoch = 4, 2 })
	ok("root-rotation", nil, st(2, 4, 7), vectorCase{Keyset: nextRoot})
	ok("root-rotation-after-ratchet", st(2, 4, 7), st(2, 4, 7), vectorCase{Keyset: nextRoot})
	ok("pointer-without-next-or-hosts", nil, st(1, 3, 7), vectorCase{Pointer: pointer("pointer-minimal.json",
		"manifest-a", func(p *patchtrust.Pointer) { p.Next, p.CDNHosts, p.RolloutPct = nil, nil, 0 })})
	ok("pointer-signed-at-key-start", nil, st(1, 3, 7), vectorCase{Now: vT0 - 90*vDay + 3600, Pointer: pointer(
		"pointer-key-start.json", "manifest-a", func(p *patchtrust.Pointer) {
			p.SignedAt, p.Expires = vT0-90*vDay, vT0-90*vDay+7*vDay
		})})

	// Keysets.
	bad("keyset-tampered", patchtrust.CheckKeysetSignature, vectorCase{Edits: []vectorEdit{{Artefact: "keyset",
		At: at(cdnKeyset, `"version":3`) + len(`"version":`), Hex: hex.EncodeToString([]byte("9"))}}})
	bad("keyset-noncanonical", patchtrust.CheckKeysetMalformed, vectorCase{Edits: []vectorEdit{{Artefact: "keyset",
		At: 1, Hex: "20", Insert: true}}})
	bad("keyset-unknown-root", patchtrust.CheckKeysetSignature, vectorCase{Keyset: keyset("keyset-unknown-root.json",
		"root-x", func(*patchtrust.Keyset) {})})
	bad("keyset-next-root-claims-current", patchtrust.CheckKeysetSignature, vectorCase{Keyset: keyset(
		"keyset-root2-epoch1.json", "root-2", func(*patchtrust.Keyset) {})})
	bad("keyset-root-epoch-unknown", patchtrust.CheckKeysetRoot, vectorCase{Keyset: keyset("keyset-epoch3.json",
		"root-x", func(k *patchtrust.Keyset) { k.RootEpoch = 3 })})
	bad("keyset-ratcheted-root", patchtrust.CheckKeysetRootRatchet, vectorCase{State: st(2, 3, 6)})
	bad("keyset-wrong-product", patchtrust.CheckKeysetProduct, vectorCase{Keyset: keyset("keyset-other-product.json",
		"root-1", func(k *patchtrust.Keyset) { k.ProductID = "other-game" })})
	bad("keyset-version-below-ratchet", patchtrust.CheckKeysetVersion, vectorCase{State: st(1, 4, 6)})

	// Pointers.
	bad("pointer-tampered", patchtrust.CheckPointerSignature, vectorCase{Edits: []vectorEdit{{Artefact: "pointer",
		At: at(cdnPointer, `"min_launcher":"1.2.0"`) + len(`"min_launcher":"1.`), Hex: hex.EncodeToString([]byte("3"))}}})
	bad("pointer-noncanonical", patchtrust.CheckPointerMalformed, vectorCase{Edits: []vectorEdit{{Artefact: "pointer",
		At: at(cdnPointer, `"sig":"`) + len(`"sig":"`), Hex: hex.EncodeToString([]byte("A"))}}})
	revoked := keyset("keyset-revoked.json", "root-1", func(k *patchtrust.Keyset) {
		k.Version = 4
		k.Keys = slices.DeleteFunc(k.Keys, func(x patchtrust.KeysetKey) bool { return x.ID == patchtrust.Fingerprint(pub("manifest-a")) })
		k.Keys = sortKeys(append(k.Keys, subkey("manifest-b", patchtrust.RoleManifest, vT0-vDay, vT0+90*vDay)))
	})
	bad("pointer-revoked-subkey", patchtrust.CheckPointerKeyUnknown, vectorCase{Keyset: revoked})
	bad("pointer-unknown-subkey", patchtrust.CheckPointerKeyUnknown, vectorCase{Pointer: pointer(
		"pointer-unknown-key.json", "manifest-x", func(*patchtrust.Pointer) {})})
	bad("pointer-news-key", patchtrust.CheckPointerKeyRole, vectorCase{Pointer: pointer("pointer-news-key.json",
		"news-a", func(*patchtrust.Pointer) {})})
	bad("pointer-unknown-role-key", patchtrust.CheckPointerKeyRole, vectorCase{Pointer: pointer(
		"pointer-store-key.json", "store-a", func(*patchtrust.Pointer) {})})
	bad("pointer-subkey-outside-window", patchtrust.CheckPointerKeyWindow, vectorCase{Pointer: pointer(
		"pointer-old-key.json", "manifest-old", func(*patchtrust.Pointer) {})})
	bad("pointer-signed-before-key", patchtrust.CheckPointerKeyWindow, vectorCase{Pointer: pointer(
		"pointer-before-key.json", "manifest-a", func(p *patchtrust.Pointer) {
			p.SignedAt, p.Expires = vT0-90*vDay-1, vT0+vDay
		})})
	bad("pointer-lifetime-over-7-days", patchtrust.CheckPointerLifetime, vectorCase{Pointer: pointer(
		"pointer-long-lifetime.json", "manifest-a", func(p *patchtrust.Pointer) { p.Expires = p.SignedAt + 7*vDay + 1 })})
	bad("pointer-expires-before-signing", patchtrust.CheckPointerLifetime, vectorCase{Pointer: pointer(
		"pointer-expires-first.json", "manifest-a", func(p *patchtrust.Pointer) { p.Expires = p.SignedAt })})
	bad("pointer-wrong-product", patchtrust.CheckPointerProduct, vectorCase{Pointer: pointer(
		"pointer-other-product.json", "manifest-a", func(p *patchtrust.Pointer) { p.ProductID = "other-game" })})
	bad("pointer-wrong-channel", patchtrust.CheckPointerChannel, vectorCase{Pointer: pointer(
		"pointer-other-channel.json", "manifest-a", func(p *patchtrust.Pointer) { p.Channel = "ptr" })})
	bad("pointer-wrong-platform", patchtrust.CheckPointerPlatform, vectorCase{Pointer: pointer(
		"pointer-other-platform.json", "manifest-a", func(p *patchtrust.Pointer) { p.Platform = "linux64" })})
	bad("pointer-expired", patchtrust.CheckPointerExpired, vectorCase{Pointer: pointer("pointer-expired.json",
		"manifest-a", func(p *patchtrust.Pointer) { p.SignedAt, p.Expires = vT0-8*vDay, vT0-vDay })})
	bad("pointer-expires-now", patchtrust.CheckPointerExpired, vectorCase{Pointer: pointer("pointer-expires-now.json",
		"manifest-a", func(p *patchtrust.Pointer) { p.Expires = vT0 })})
	bad("pointer-rolled-back", patchtrust.CheckPointerSequence, vectorCase{Pointer: pointer("pointer-sequence-5.json",
		"manifest-a", func(p *patchtrust.Pointer) { p.Sequence = 5 })})
	bad("pointer-replayed-after-rollback", patchtrust.CheckPointerSequence, vectorCase{State: st(1, 3, 8)})

	// Manifests.
	bad("manifest-tampered-payload", patchtrust.CheckManifestBody, vectorCase{Edits: []vectorEdit{{
		Artefact: "manifest", At: mfOff, Hex: hex.EncodeToString([]byte{mfile[mfOff] ^ 0x40})}}})
	bad("manifest-tampered-header", patchtrust.CheckManifestMalformed, vectorCase{Edits: []vectorEdit{{
		Artefact: "manifest", At: 40, Hex: "05"}}})
	resealed := slices.Clone(mfile)
	resealed[40] = 5 // compatEpoch 4 -> 5 with the header hash recomputed: a different manifest identity
	h := cdc.Sum(resealed[:manifest.SignedBytes])
	copy(resealed[manifest.SignedBytes:], h[:])
	bad("manifest-tampered-header-resealed", patchtrust.CheckManifestHash, vectorCase{
		Manifest: write("manifest-resealed.hman", resealed)})
	bad("manifest-swapped", patchtrust.CheckManifestHash, vectorCase{Manifest: nextMf})
	bad("manifest-tampered-signature", patchtrust.CheckManifestSignature, vectorCase{Edits: []vectorEdit{{
		Artefact: "manifest", At: 300, Hex: hex.EncodeToString([]byte{mfile[300] ^ 1})}}})
	for _, mc := range []struct {
		name, signer string
		check        patchtrust.Check
		mod          func(*manifest.Header)
	}{
		{"manifest-unknown-subkey", "manifest-x", patchtrust.CheckManifestKeyUnknown, nil},
		{"manifest-news-key", "news-a", patchtrust.CheckManifestKeyRole, nil},
		{"manifest-subkey-outside-window", "manifest-old", patchtrust.CheckManifestKeyWindow, nil},
		{"manifest-wrong-product", "manifest-a", patchtrust.CheckManifestProduct,
			func(h *manifest.Header) { h.ProductID = "other-game" }},
		{"manifest-wrong-platform", "manifest-a", patchtrust.CheckManifestPlatform,
			func(h *manifest.Header) { h.Platform = "linux64" }},
		{"manifest-wrong-build", "manifest-a", patchtrust.CheckManifestBuild,
			func(h *manifest.Header) { h.BuildID = "2026.09.21-r2" }},
		{"manifest-wrong-compat-epoch", "manifest-a", patchtrust.CheckManifestCompatEpoch,
			func(h *manifest.Header) { h.CompatEpoch = vEpoch + 1 }},
		{"manifest-expired", "manifest-a", patchtrust.CheckManifestExpired,
			func(h *manifest.Header) { h.ExpiresAt = vT0 }},
	} {
		mf, pf := manifestCase(mc.name, mc.signer, mc.mod)
		bad(mc.name, mc.check, vectorCase{Manifest: mf, Pointer: pf})
	}
	ok("manifest-expires-later", nil, st(1, 3, 7), func() vectorCase {
		mf, pf := manifestCase("manifest-expires-later", "manifest-a", func(h *manifest.Header) { h.ExpiresAt = vT0 + 1 })
		return vectorCase{Manifest: mf, Pointer: pf}
	}())

	// Chunks.
	bad("chunk-substituted", patchtrust.CheckChunkHash, vectorCase{Chunks: map[string]string{
		bigID: write("chunk-substituted.zst", substitute)}})
	bad("chunk-tampered", patchtrust.CheckChunkCorrupt, vectorCase{Edits: []vectorEdit{{Artefact: "chunk",
		Chunk: bigID, At: 100, Hex: "ff"}}})
	bad("chunk-oversize", patchtrust.CheckChunkCorrupt, vectorCase{Chunks: map[string]string{
		bigID: write("chunk-oversize.zst", oversize)}})
	bad("chunk-missing", patchtrust.CheckChunkMissing, vectorCase{Chunks: map[string]string{bigID: ""}})

	vf := vectorFile{
		Comment: "Shared CL-14 vectors (engine/patch/README.md, Shared vectors): Go pkg/patchcdn and C++ engine/patch " +
			"verify each case against the CDN tree in cdn/ with the case's replacements and edits (keyset, pointer, " +
			"manifest and chunk files in this directory; edits overwrite, or insert, the hex bytes at an offset of the " +
			"artefact), the root pair, now and the ratchet state (state, or the case's), and must accept it (check \"\", " +
			"ending with wantState) or reject it with exactly check. The keys are test-only (package trusttest); " +
			"verifiers refuse their roots unless a test allows them. Regenerate with go test ./pkg/patchcdn -run " +
			"TestUpdateTrustVectors -update.",
		Product: vProduct, Channel: vChannel, Platform: vPlatform, Now: vT0, State: *st(1, 3, 6),
		Roots: vectorRoots{Epoch: 1, Current: hexKey("root-1"), Next: hexKey("root-2")},
		Cases: cases,
	}
	writeJSON(t, filepath.Join(trustDir, "cases.json"), vf)
	writeJSON(t, filepath.Join(trustDir, "syntax.json"), syntaxCases(t, cdnKeyset, cdnPointer))
}

// writeJSON writes v as indented JSON.
func writeJSON(t *testing.T, path string, v any) {
	b, err := json.MarshalIndent(v, "", " ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, append(b, '\n'), 0o644); err != nil {
		t.Fatal(err)
	}
}

func writeSigningDir(t *testing.T, dir string, keyset []byte, signer string, dev bool) {
	t.Helper()
	id := patchtrust.Fingerprint(pub(signer))
	sub := fmt.Sprintf(`{"dev":%t,"productId":%q,"id":%q,"seed":%q}`, dev, vProduct, hex.EncodeToString(id[:]),
		hex.EncodeToString(key(signer).Seed()))
	if err := os.WriteFile(filepath.Join(dir, "keyset.json"), keyset, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "manifest-key.json"), []byte(sub), 0o600); err != nil {
		t.Fatal(err)
	}
}

// syntaxCases are encodings both parsers must refuse: other encodings of valid content (whitespace, member
// order, escapes, number forms, hex case), invalid content and limits.
func syntaxCases(t *testing.T, ks, ptr []byte) syntaxFile {
	edit := func(doc []byte, old, new string) []byte {
		if !bytes.Contains(doc, []byte(old)) {
			t.Fatalf("%q not in %s", old, doc)
		}
		return bytes.Replace(doc, []byte(old), []byte(new), 1)
	}
	// upperSig uppercases the signature's first hex letter.
	upperSig := func(doc []byte) []byte {
		i := bytes.Index(doc, []byte(`"sig":"`)) + len(`"sig":"`)
		out := slices.Clone(doc)
		for j := i; j < i+2*patchtrust.SignatureSize; j++ {
			if out[j] >= 'a' && out[j] <= 'f' {
				out[j] -= 'a' - 'A'
				return out
			}
		}
		t.Fatal("the signature has no hex letter")
		return nil
	}
	common := func(doc []byte) []rawSyntaxCase {
		q := `"` + vProduct + `"`
		cs := []rawSyntaxCase{
			{"empty", nil},
			{"whitespace after the opening brace", edit(doc, "{", "{ ")},
			{"trailing newline", append(slices.Clone(doc), '\n')},
			{"trailing object", append(slices.Clone(doc), "{}"...)},
			{"truncated", doc[:len(doc)-1]},
			{"byte-order mark", append([]byte{0xEF, 0xBB, 0xBF}, doc...)},
			{"uppercase hex", upperSig(doc)},
			{"escaped character", edit(doc, q, `"\u0076`+vProduct[1:]+`"`)},
			{"non-ASCII byte", edit(doc, q, "\"v\xc3\xa9"+vProduct[2:]+`"`)},
			{"null value", edit(doc, q, `null`)},
		}
		return cs
	}
	sf := syntaxFile{Comment: "Documents Go pkg/patchtrust and C++ engine/patch must both refuse to parse " +
		"(keyset-malformed, pointer-malformed): cdn/'s keyset or pointer with bytes [at, at+delete) replaced by " +
		"put (or putHex), then padded with spaces to padTo bytes. Regenerate with go test ./pkg/patchcdn -run " +
		"TestUpdateTrustVectors -update."}
	for _, c := range common(ks) {
		sf.Keyset = append(sf.Keyset, splice(c.name, ks, c.doc))
	}
	for _, c := range common(ptr) {
		sf.Pointer = append(sf.Pointer, splice(c.name, ptr, c.doc))
	}
	k := func(name string, doc []byte) {
		sf.Keyset = append(sf.Keyset, splice(name, ks, doc))
	}
	k("leading zero", edit(ks, `"version":3`, `"version":03`))
	k("negative number", edit(ks, `"version":3`, `"version":-3`))
	k("fraction", edit(ks, `"version":3`, `"version":3.0`))
	k("exponent", edit(ks, `"version":3`, `"version":3e0`))
	k("integer 2^53", edit(ks, `"version":3`, `"version":9007199254740992`))
	k("integer 2^64", edit(ks, `"version":3`, `"version":18446744073709551616`))
	k("string number", edit(ks, `"version":3`, `"version":"3"`))
	k("version zero", edit(ks, `"version":3`, `"version":0`))
	k("members out of order", edit(ks, `"productId":"vector-game","rootEpoch":1`,
		`"rootEpoch":1,"productId":"vector-game"`))
	k("duplicate member", edit(ks, `"version":3}`, `"version":3,"version":3}`))
	k("unknown member", edit(ks, `"version":3}`, `"version":3,"zzz":1}`))
	k("missing member", edit(ks, `,"version":3}`, `}`))
	k("root epoch zero", edit(ks, `"rootEpoch":1`, `"rootEpoch":0`))
	k("root epoch 2^32-1", edit(ks, `"rootEpoch":1`, `"rootEpoch":4294967295`))
	k("root epoch 2^32", edit(ks, `"rootEpoch":1`, `"rootEpoch":4294967296`))
	k("product ID uppercase", edit(ks, `"productId":"vector-game"`, `"productId":"Vector-game"`))
	k("role uppercase", edit(ks, `"role":"manifest"`, `"role":"Manifest"`))
	pubAt := bytes.Index(ks, []byte(`"pub":"`)) + len(`"pub":"`)
	k("short public key", append(slices.Clone(ks[:pubAt]), ks[pubAt+2:]...))
	// The key objects, to reorder, repeat or edit them.
	lo, hi := bytes.Index(ks, []byte(`"keys":[`))+len(`"keys":[`), bytes.Index(ks, []byte(`],"productId"`))
	var objs [][]byte
	for i := lo; i < hi; {
		j := i + bytes.IndexByte(ks[i:], '}') + 1
		objs = append(objs, ks[i:j])
		i = j + 1 // past the comma
	}
	if len(objs) != 4 {
		t.Fatalf("%d key objects", len(objs))
	}
	keys := func(list ...[]byte) []byte {
		return append(append(slices.Clone(ks[:lo]), bytes.Join(list, []byte(","))...), ks[hi:]...)
	}
	k("no keys", keys())
	k("keys out of order", keys(objs[1], objs[0], objs[2], objs[3]))
	k("duplicate key", keys(objs[0], objs[0], objs[1], objs[2], objs[3]))
	k("65 keys", keys(slices.Repeat([][]byte{objs[0]}, 65)...))
	badID := slices.Clone(objs[0])
	d := bytes.Index(badID, []byte(`"id":"`)) + len(`"id":"`)
	badID[d] ^= 1 // '0'<->'1', '2'<->'3', 'a'<->'`'... keep it a hex digit:
	if !(badID[d] >= '0' && badID[d] <= '9' || badID[d] >= 'a' && badID[d] <= 'f') {
		badID[d] = 'a'
		if objs[0][d] == 'a' {
			badID[d] = 'b'
		}
	}
	k("key ID not the key's fingerprint", keys(badID, objs[1], objs[2], objs[3]))
	o := objs[0]
	na := bytes.Index(o, []byte(`"notAfter":`)) + len(`"notAfter":`)
	nb := bytes.Index(o, []byte(`"notBefore":`)) + len(`"notBefore":`)
	nbEnd := nb + bytes.IndexByte(o[nb:], ',')
	emptyWindow := append(append(slices.Clone(o[:nb]), o[na:na+bytes.IndexByte(o[na:], ',')]...), o[nbEnd:]...)
	k("empty validity window", keys(emptyWindow, objs[1], objs[2], objs[3]))
	sf.Keyset = append(sf.Keyset, syntaxCase{Name: "oversize", At: len(ks), PadTo: patchtrust.MaxKeysetSize + 1})

	p := func(name string, doc []byte) {
		sf.Pointer = append(sf.Pointer, splice(name, ptr, doc))
	}
	p("leading zero", edit(ptr, `"sequence":7`, `"sequence":07`))
	p("sequence zero", edit(ptr, `"sequence":7`, `"sequence":0`))
	p("boolean as number", edit(ptr, `"rollback":false`, `"rollback":0`))
	p("rollout above 100", edit(ptr, `"rollout_pct":100`, `"rollout_pct":101`))
	p("compat epoch 2^32", edit(ptr, `"compat_epoch":4`, `"compat_epoch":4294967296`))
	p("members out of order", edit(ptr, `"platform":"win64","product_id":"vector-game"`,
		`"product_id":"vector-game","platform":"win64"`))
	ni := bytes.Index(ptr, []byte(`,"next":{`))
	nj := ni + bytes.IndexByte(ptr[ni:], '}') + 1
	withoutNext := append(slices.Clone(ptr[:ni]), ptr[nj:]...)
	p("next out of order", edit(withoutNext, `,"product_id"`, string(ptr[ni:nj])+`,"product_id"`))
	p("empty next", append(append(slices.Clone(ptr[:ni]), `,"next":{}`...), ptr[nj:]...))
	p("next missing a member", edit(ptr, `"next":{"available_at":`, `"next":{"x":`))
	p("channel uppercase", edit(ptr, `"channel":"live"`, `"channel":"Live"`))
	p("channel one letter", edit(ptr, `"channel":"live"`, `"channel":"l"`))
	p("platform uppercase", edit(ptr, `"platform":"win64"`, `"platform":"Win64"`))
	p("build ID starting with a dash", edit(ptr, `"build_id":"2026`, `"build_id":"-026`))
	p("http host off loopback", edit(ptr, `"https://cdn1.`, `"http://cdn1.`))
	p("loopback name as a prefix", edit(ptr, `"https://cdn1.vector-game.example"`,
		`"http://localhost.vector-game.example"`))
	p("host with a space", edit(ptr, `"https://cdn1.vector-game.example"`, `"https://cdn1 vector-game.example"`))
	p("17 hosts", edit(ptr, `"cdn_hosts":[`, `"cdn_hosts":[`+strings.Repeat(`"https://x.example",`, 15)))
	p("version with five parts", edit(ptr, `"min_launcher":"1.2.0"`, `"min_launcher":"1.2.0.0.0"`))
	p("version with a letter", edit(ptr, `"min_client":"1.2.0"`, `"min_client":"v1.2.0"`))
	p("key ID too long", edit(ptr, `"key_id":"`, `"key_id":"00`))
	sf.Pointer = append(sf.Pointer, syntaxCase{Name: "oversize", At: len(ptr), PadTo: patchtrust.MaxPointerSize + 1})
	return sf
}

// overlay serves the CDN tree with a case's replacements and edits.
type overlay struct {
	base     patchcdn.DirSource
	keyset   []byte
	pointer  []byte
	manifest []byte
	chunks   map[string][]byte // ID -> bytes; nil value = missing
}

func (o *overlay) Fetch(ctx context.Context, path string, limit int64) ([]byte, error) {
	var b []byte
	switch {
	case path == patchcdn.KeysetPath(vProduct):
		b = o.keyset
	case path == patchcdn.PointerPath(vProduct, vChannel, vPlatform):
		b = o.pointer
	case strings.HasPrefix(path, "manifests/"):
		b = o.manifest
	case strings.HasPrefix(path, "chunks/"):
		id := strings.TrimSuffix(path[strings.LastIndexByte(path, '/')+1:], ".zst")
		if c, ok := o.chunks[id]; ok {
			if c == nil {
				return nil, fmt.Errorf("%w: %s", patchcdn.ErrNotFound, path)
			}
			b = c
		}
	}
	if b == nil {
		return o.base.Fetch(ctx, path, limit)
	}
	if int64(len(b)) > limit {
		return nil, fmt.Errorf("%s is larger than %d bytes", path, limit)
	}
	return b, nil
}

func readVector(t testing.TB, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(trustDir, name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func loadVectors(t testing.TB) (vectorFile, *patchtrust.Verifier) {
	var vf vectorFile
	if err := json.Unmarshal(readVector(t, "cases.json"), &vf); err != nil {
		t.Fatal(err)
	}
	var rp patchtrust.RootPair
	rp.Epoch = vf.Roots.Epoch
	c, _ := hex.DecodeString(vf.Roots.Current)
	n, _ := hex.DecodeString(vf.Roots.Next)
	copy(rp.Current[:], c)
	copy(rp.Next[:], n)
	tgt := patchtrust.Target{ProductID: vf.Product, Channel: vf.Channel, Platform: vf.Platform}
	if _, err := patchtrust.NewVerifier(tgt, rp, patchtrust.Options{}); err == nil {
		t.Fatal("a verifier accepted the test-only roots without AllowTestKeys")
	}
	v, err := patchtrust.NewVerifier(tgt, rp, patchtrust.Options{AllowTestKeys: true})
	if err != nil {
		t.Fatal(err)
	}
	return vf, v
}

// caseSource assembles a case's artefacts.
func caseSource(t testing.TB, c vectorCase) *overlay {
	cdn := patchcdn.DirSource{Root: filepath.Join(trustDir, "cdn")}
	o := &overlay{base: cdn, chunks: map[string][]byte{}}
	load := func(file, path string) []byte {
		if file != "" {
			return readVector(t, file)
		}
		b, err := cdn.Fetch(context.Background(), path, 1<<30)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	o.keyset = load(c.Keyset, patchcdn.KeysetPath(vProduct))
	o.pointer = load(c.Pointer, patchcdn.PointerPath(vProduct, vChannel, vPlatform))
	if c.Manifest != "" {
		o.manifest = readVector(t, c.Manifest)
	} else if p, err := patchtrust.ParsePointer(o.pointer); err == nil {
		o.manifest = load("", patchcdn.ManifestPath(vProduct, p.BuildID, vPlatform))
	}
	for id, f := range c.Chunks {
		if f == "" {
			o.chunks[id] = nil
		} else {
			o.chunks[id] = readVector(t, f)
		}
	}
	for _, e := range c.Edits {
		var target *[]byte
		switch e.Artefact {
		case "keyset":
			target = &o.keyset
		case "pointer":
			target = &o.pointer
		case "manifest":
			if o.manifest == nil {
				o.manifest = load("", patchcdn.ManifestPath(vProduct, vBuild, vPlatform))
			}
			target = &o.manifest
		case "chunk":
			h, _ := hex.DecodeString(e.Chunk)
			if _, ok := o.chunks[e.Chunk]; !ok {
				o.chunks[e.Chunk] = load("", patchcdn.ChunkPath(cdc.Hash(h)))
			}
			b := o.chunks[e.Chunk]
			target = &b
			defer func() { o.chunks[e.Chunk] = b }()
		default:
			t.Fatalf("unknown artefact %q", e.Artefact)
		}
		bs, err := hex.DecodeString(e.Hex)
		if err != nil {
			t.Fatal(err)
		}
		*target = slices.Clone(*target)
		if e.Insert {
			*target = slices.Insert(*target, e.At, bs...)
		} else {
			copy((*target)[e.At:], bs)
		}
	}
	return o
}

// TestSharedTrustVectors runs cases.json: CL-14's subset (tampered, expired, rolled back, and the
// wrong-product, revoked-subkey and ratchet cases). Each rejection names the one check it must fail, so a
// verifier with that check removed fails this test.
func TestSharedTrustVectors(t *testing.T) {
	vf, v := loadVectors(t)
	checks := map[string]bool{}
	for _, c := range vf.Cases {
		t.Run(c.Name, func(t *testing.T) {
			src := caseSource(t, c)
			store := &patchcdn.MemoryStateStore{State: vf.State}
			if c.State != nil {
				store.State = *c.State
			}
			now := vf.Now
			if c.Now != 0 {
				now = c.Now
			}
			res, err := patchcdn.Verify(context.Background(), src, v, now, store)
			if got := string(patchtrust.CheckOf(err)); got != c.Check || (c.Check == "" && err != nil) {
				t.Fatalf("got check %q (%v), want %q", got, err, c.Check)
			}
			if c.Check == "" {
				if c.WantState != nil && store.State != *c.WantState {
					t.Fatalf("state %+v, want %+v", store.State, *c.WantState)
				}
				if res.Chunks != len(res.Manifest.Chunks) || res.Chunks == 0 {
					t.Fatalf("%d chunks checked", res.Chunks)
				}
			}
		})
		checks[c.Check] = true
	}
	// Every check of the chain is the one a case fails.
	for _, c := range allChecks {
		if !checks[string(c)] {
			t.Errorf("no vector fails check %s", c)
		}
	}
}

var allChecks = []patchtrust.Check{
	patchtrust.CheckKeysetMalformed, patchtrust.CheckKeysetRoot, patchtrust.CheckKeysetRootRatchet,
	patchtrust.CheckKeysetSignature, patchtrust.CheckKeysetProduct, patchtrust.CheckKeysetVersion,
	patchtrust.CheckPointerMalformed, patchtrust.CheckPointerKeyUnknown, patchtrust.CheckPointerKeyRole,
	patchtrust.CheckPointerSignature, patchtrust.CheckPointerKeyWindow, patchtrust.CheckPointerLifetime,
	patchtrust.CheckPointerProduct, patchtrust.CheckPointerChannel, patchtrust.CheckPointerPlatform,
	patchtrust.CheckPointerExpired, patchtrust.CheckPointerSequence, patchtrust.CheckManifestMalformed,
	patchtrust.CheckManifestHash, patchtrust.CheckManifestKeyUnknown, patchtrust.CheckManifestKeyRole,
	patchtrust.CheckManifestSignature, patchtrust.CheckManifestKeyWindow, patchtrust.CheckManifestProduct,
	patchtrust.CheckManifestPlatform, patchtrust.CheckManifestBuild, patchtrust.CheckManifestCompatEpoch,
	patchtrust.CheckManifestExpired, patchtrust.CheckManifestBody, patchtrust.CheckChunkMissing,
	patchtrust.CheckChunkCorrupt, patchtrust.CheckChunkHash,
}

// TestSharedSyntaxVectors: both parsers refuse every document of syntax.json, and the verifiers report it as
// malformed.
func TestSharedSyntaxVectors(t *testing.T) {
	var sf syntaxFile
	if err := json.Unmarshal(readVector(t, "syntax.json"), &sf); err != nil {
		t.Fatal(err)
	}
	_, v := loadVectors(t)
	ksDoc := readVector(t, filepath.Join("cdn", filepath.FromSlash(patchcdn.KeysetPath(vProduct))))
	ptrDoc := readVector(t, filepath.Join("cdn", filepath.FromSlash(patchcdn.PointerPath(vProduct, vChannel, vPlatform))))
	ks, err := v.VerifyKeyset(ksDoc, patchtrust.State{})
	if err != nil {
		t.Fatal(err)
	}
	if len(sf.Keyset) < 30 || len(sf.Pointer) < 30 {
		t.Fatalf("%d keyset and %d pointer cases", len(sf.Keyset), len(sf.Pointer))
	}
	for _, c := range sf.Keyset {
		b := c.apply(ksDoc)
		if _, err := patchtrust.ParseKeyset(b); err == nil {
			t.Errorf("keyset %q parsed", c.Name)
		}
		if _, err := v.VerifyKeyset(b, patchtrust.State{}); patchtrust.CheckOf(err) != patchtrust.CheckKeysetMalformed {
			t.Errorf("keyset %q: %v", c.Name, err)
		}
	}
	for _, c := range sf.Pointer {
		b := c.apply(ptrDoc)
		if _, err := patchtrust.ParsePointer(b); err == nil {
			t.Errorf("pointer %q parsed", c.Name)
		}
		if _, err := v.VerifyPointer(b, ks, vT0, patchtrust.State{}); patchtrust.CheckOf(err) != patchtrust.CheckPointerMalformed {
			t.Errorf("pointer %q: %v", c.Name, err)
		}
	}
}

// Every single-byte change of the keyset, the pointer and the manifest's signed header and signature is
// rejected (CL-14: 100 % rejection of tampered artefacts).
func TestEveryByteTampered(t *testing.T) {
	vf, v := loadVectors(t)
	base := caseSource(t, vectorCase{})
	for _, art := range []string{"keyset", "pointer", "manifest"} {
		var doc []byte
		switch art {
		case "keyset":
			doc = base.keyset
		case "pointer":
			doc = base.pointer
		default:
			doc = base.manifest[:manifest.HeaderSize]
		}
		for i := range doc {
			for _, x := range []byte{0x01, 0x20, 0x80} {
				o := *base
				b := slices.Clone(doc)
				b[i] ^= x
				switch art {
				case "keyset":
					o.keyset = b
				case "pointer":
					o.pointer = b
				default:
					o.manifest = append(b, base.manifest[manifest.HeaderSize:]...)
				}
				_, err := patchcdn.Verify(context.Background(), &o, v, vf.Now, &patchcdn.MemoryStateStore{State: vf.State})
				if !errors.Is(err, patchtrust.ErrRejected) {
					t.Fatalf("%s byte %d ^ %#x: %v", art, i, x, err)
				}
			}
		}
	}
}

// BenchmarkVerifyChain: the per-launch cost before any chunk, keyset, pointer and manifest header (three
// Ed25519 verifications and the canonical parses) of the shared vectors.
func BenchmarkVerifyChain(b *testing.B) {
	vf, v := loadVectors(b)
	o := caseSource(b, vectorCase{})
	for i := 0; i < b.N; i++ {
		ks, err := v.VerifyKeyset(o.keyset, vf.State)
		if err != nil {
			b.Fatal(err)
		}
		p, err := v.VerifyPointer(o.pointer, ks, vf.Now, vf.State)
		if err != nil {
			b.Fatal(err)
		}
		if _, err := v.VerifyManifestHeader(o.manifest, ks, p.ManifestRef, vf.Now); err != nil {
			b.Fatal(err)
		}
	}
}
