package patchtrust_test

import (
	"bytes"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
	"github.com/PageMastr/scifi-test/services/pkg/patchtrust"
	"github.com/PageMastr/scifi-test/services/pkg/patchtrust/trusttest"
)

var vectorsDir = filepath.Join("..", "..", "testdata", "vectors", "trust")

func pubOf(name string) patchtrust.PublicKey { return patchtrust.PublicKeyOf(trusttest.Key(name)) }

func testKeyset() *patchtrust.Keyset {
	ks := &patchtrust.Keyset{ProductID: "sample-game", Version: 2, RootEpoch: 1}
	for _, n := range []string{"manifest-a", "news-a"} {
		p := pubOf(n)
		ks.Keys = append(ks.Keys, patchtrust.KeysetKey{ID: patchtrust.Fingerprint(p), Role: strings.TrimSuffix(n, "-a"),
			Pub: p, NotBefore: 1000, NotAfter: 1_000_000})
	}
	if bytes.Compare(ks.Keys[0].ID[:], ks.Keys[1].ID[:]) > 0 {
		ks.Keys[0], ks.Keys[1] = ks.Keys[1], ks.Keys[0]
	}
	return ks
}

func testPointer() *patchtrust.Pointer {
	return &patchtrust.Pointer{ProductID: "sample-game", Channel: "live", Platform: "linux64",
		ManifestRef: patchtrust.ManifestRef{BuildID: "b1", ManifestHash: cdc.Sum([]byte("m")), CompatEpoch: 3},
		Sequence:    9, MinLauncher: "1", MinClient: "1.0.2.3", CDNHosts: []string{"https://a.example", "http://localhost:7700/cdn"},
		RolloutPct: 50, SignedAt: 5000, Expires: 5000 + patchtrust.MaxPointerLifetime}
}

// The test-only roots listed in the verifier are exactly the trusttest roots, and a verifier refuses them
// unless a test allows them.
func TestTestOnlyRoots(t *testing.T) {
	for _, n := range trusttest.RootNames {
		if !patchtrust.IsTestOnlyKey(pubOf(n)) {
			t.Fatalf("%s is not listed as test-only", n)
		}
	}
	if patchtrust.IsTestOnlyKey(pubOf("manifest-a")) {
		t.Fatal("a subkey is listed as a test-only root")
	}
	target := patchtrust.Target{ProductID: "sample-game", Channel: "live", Platform: "win64"}
	_, priv, _ := ed25519.GenerateKey(nil)
	for _, rp := range []patchtrust.RootPair{
		{Epoch: 1, Current: pubOf("root-1"), Next: patchtrust.PublicKeyOf(priv)},
		{Epoch: 1, Current: patchtrust.PublicKeyOf(priv), Next: pubOf("root-x")},
	} {
		if _, err := patchtrust.NewVerifier(target, rp, patchtrust.Options{}); err == nil {
			t.Fatal("a product verifier accepted a test-only root")
		}
		if _, err := patchtrust.NewVerifier(target, rp, patchtrust.Options{AllowTestKeys: true}); err != nil {
			t.Fatal(err)
		}
	}
	for _, bad := range []struct {
		t  patchtrust.Target
		rp patchtrust.RootPair
	}{
		{patchtrust.Target{ProductID: "Sample", Channel: "live", Platform: "win64"}, patchtrust.RootPair{Epoch: 1}},
		{target, patchtrust.RootPair{Epoch: 0, Next: pubOf("root-2")}},
		{target, patchtrust.RootPair{Epoch: ^uint32(0), Next: pubOf("root-2")}},
		{target, patchtrust.RootPair{Epoch: 1, Current: pubOf("root-1"), Next: pubOf("root-1")}},
	} {
		if _, err := patchtrust.NewVerifier(bad.t, bad.rp, patchtrust.Options{AllowTestKeys: true}); err == nil {
			t.Fatalf("accepted %+v %+v", bad.t, bad.rp)
		}
	}
}

func TestKeysetRoundTrip(t *testing.T) {
	ks := testKeyset()
	if err := ks.Sign(trusttest.Key("root-1")); err != nil {
		t.Fatal(err)
	}
	b, err := ks.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	back, err := patchtrust.ParseKeyset(b)
	if err != nil || !reflect.DeepEqual(back, ks) {
		t.Fatalf("round trip: %v\n%s", err, b)
	}
	if !json.Valid(b) {
		t.Fatal("not JSON")
	}
	// The signed message is the context and the document without "sig".
	msg := ks.SignedMessage()
	if !bytes.HasPrefix(msg, []byte("HELIOS-KEYSET-V0\n")) || bytes.Contains(msg, []byte(`"sig"`)) {
		t.Fatalf("signed message %s", msg)
	}
	ks.Sig[0] ^= 1
	if !bytes.Equal(msg, ks.SignedMessage()) {
		t.Fatal("the signature is part of the signed message")
	}
	if ks.Key(ks.Keys[1].ID) != &ks.Keys[1] || ks.Key(patchtrust.KeyID{}) != nil {
		t.Fatal("Key lookup")
	}
}

func TestPointerRoundTrip(t *testing.T) {
	for _, mod := range []func(*patchtrust.Pointer){
		func(*patchtrust.Pointer) {},
		func(p *patchtrust.Pointer) { p.CDNHosts = nil },
		func(p *patchtrust.Pointer) { p.Rollback = true },
		func(p *patchtrust.Pointer) {
			p.Next = &patchtrust.Next{ManifestRef: patchtrust.ManifestRef{BuildID: "b2", CompatEpoch: 4}, AvailableAt: 7}
		},
		func(p *patchtrust.Pointer) { p.Sequence, p.Expires = patchtrust.MaxInt, patchtrust.MaxInt },
	} {
		p := testPointer()
		mod(p)
		if err := p.Sign(trusttest.Key("manifest-a")); err != nil {
			t.Fatal(err)
		}
		b, err := p.Marshal()
		if err != nil {
			t.Fatal(err)
		}
		back, err := patchtrust.ParsePointer(b)
		if err != nil || !reflect.DeepEqual(back, p) {
			t.Fatalf("round trip: %v\n%s", err, b)
		}
		if p.KeyID != patchtrust.Fingerprint(pubOf("manifest-a")) || !json.Valid(b) {
			t.Fatal("key ID or JSON")
		}
	}
}

// The validators refuse what the parsers refuse, so Sign and Marshal never produce a document that does
// not parse.
func TestInvalidDocumentsAreNotSigned(t *testing.T) {
	for name, mod := range map[string]func(*patchtrust.Keyset){
		"product":     func(k *patchtrust.Keyset) { k.ProductID = "helios dev" },
		"version":     func(k *patchtrust.Keyset) { k.Version = 0 },
		"big version": func(k *patchtrust.Keyset) { k.Version = patchtrust.MaxInt + 1 },
		"epoch":       func(k *patchtrust.Keyset) { k.RootEpoch = 0 },
		"no keys":     func(k *patchtrust.Keyset) { k.Keys = nil },
		"order":       func(k *patchtrust.Keyset) { k.Keys[0], k.Keys[1] = k.Keys[1], k.Keys[0] },
		"fingerprint": func(k *patchtrust.Keyset) { k.Keys[0].ID[0] ^= 1 },
		"role":        func(k *patchtrust.Keyset) { k.Keys[0].Role = "Manifest" },
		"window":      func(k *patchtrust.Keyset) { k.Keys[0].NotAfter = k.Keys[0].NotBefore },
	} {
		ks := testKeyset()
		mod(ks)
		if ks.Sign(trusttest.Key("root-1")) == nil {
			t.Errorf("keyset %s: signed", name)
		}
		if _, err := ks.Marshal(); err == nil {
			t.Errorf("keyset %s: marshaled", name)
		}
	}
	for name, mod := range map[string]func(*patchtrust.Pointer){
		"product":      func(p *patchtrust.Pointer) { p.ProductID = "x" },
		"channel":      func(p *patchtrust.Pointer) { p.Channel = "l" },
		"platform":     func(p *patchtrust.Pointer) { p.Platform = "Linux" },
		"build":        func(p *patchtrust.Pointer) { p.BuildID = "" },
		"sequence":     func(p *patchtrust.Pointer) { p.Sequence = 0 },
		"min launcher": func(p *patchtrust.Pointer) { p.MinLauncher = "1..2" },
		"min client":   func(p *patchtrust.Pointer) { p.MinClient = "1234567890" },
		"rollout":      func(p *patchtrust.Pointer) { p.RolloutPct = 101 },
		"expires":      func(p *patchtrust.Pointer) { p.Expires = patchtrust.MaxInt + 1 },
		"host":         func(p *patchtrust.Pointer) { p.CDNHosts = []string{"ftp://x"} },
		"quote":        func(p *patchtrust.Pointer) { p.CDNHosts = []string{`https://a"b`} },
		"next build":   func(p *patchtrust.Pointer) { p.Next = &patchtrust.Next{} },
		"hosts":        func(p *patchtrust.Pointer) { p.CDNHosts = make([]string, 17) },
	} {
		p := testPointer()
		mod(p)
		if p.Sign(trusttest.Key("manifest-a")) == nil {
			t.Errorf("pointer %s: signed", name)
		}
	}
}

func TestAdvance(t *testing.T) {
	ks := &patchtrust.Keyset{Version: 4, RootEpoch: 2}
	st := patchtrust.State{RootEpoch: 3, KeysetVersion: 5, PointerSequence: 10}
	if got := patchtrust.Advance(st, ks, &patchtrust.Pointer{Sequence: 8}); got != st {
		t.Fatalf("a ratchet moved back: %+v", got)
	}
	got := patchtrust.Advance(st, ks, &patchtrust.Pointer{Sequence: 8, Rollback: true})
	if got != (patchtrust.State{RootEpoch: 3, KeysetVersion: 5, PointerSequence: 8}) {
		t.Fatalf("a rollback pointer must set the sequence: %+v", got)
	}
	got = patchtrust.Advance(patchtrust.State{}, ks, &patchtrust.Pointer{Sequence: 12})
	if got != (patchtrust.State{RootEpoch: 2, KeysetVersion: 4, PointerSequence: 12}) {
		t.Fatalf("%+v", got)
	}
}

// The state record is engine/patch's (test_trust.cpp pins the same bytes), and the file store refuses
// anything but one valid record: a reset ratchet would accept rolled-back pointers.
func TestFileStateStore(t *testing.T) {
	rec := patchtrust.EncodeState(patchtrust.State{RootEpoch: 3, KeysetVersion: 9, PointerSequence: 1 << 40})
	if got := hex.EncodeToString(rec[:]); got != "4854525300000000030000000900000000000000000000000001000047ae02f5" {
		t.Fatalf("record %s", got)
	}
	for i := range rec {
		bad := rec
		bad[i] ^= 0x10
		if _, err := patchtrust.DecodeState(bad[:]); err == nil {
			t.Fatalf("a record with byte %d changed decoded", i)
		}
	}
	path := filepath.Join(t.TempDir(), "sub", "state.bin")
	s := patchtrust.FileStateStore{Path: path}
	if st, err := s.Load(); err != nil || st != (patchtrust.State{}) {
		t.Fatalf("missing file: %+v %v", st, err)
	}
	want := patchtrust.State{RootEpoch: 2, KeysetVersion: 7, PointerSequence: 1 << 40}
	if err := s.Save(want); err != nil {
		t.Fatal(err)
	}
	if st, err := s.Load(); err != nil || st != want {
		t.Fatalf("%+v %v", st, err)
	}
	good, _ := os.ReadFile(path)
	for name, b := range map[string][]byte{
		"empty":           nil,
		"truncated":       good[:31],
		"trailing byte":   append(bytes.Clone(good), '\n'),
		"a second record": append(bytes.Clone(good), good...),
		"JSON":            []byte(`{"rootEpoch":1,"keysetVersion":2,"pointerSequence":3}`),
	} {
		if err := os.WriteFile(path, b, 0o644); err != nil {
			t.Fatal(err)
		}
		if _, err := s.Load(); err == nil {
			t.Fatalf("state file (%s) loaded", name)
		}
	}
}

func TestSignManifest(t *testing.T) {
	m := &manifest.Manifest{Header: manifest.Header{ProductID: "sample-game", Platform: "win64", BuildID: "b1",
		Sequence: 1, CreatedAt: 5000, KeyID: patchtrust.Fingerprint(pubOf("manifest-a"))}}
	file, err := m.Marshal(manifest.WriteOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if err := patchtrust.SignManifest(file, trusttest.Key("news-a")); err == nil {
		t.Fatal("signed with a key other than the header's keyId")
	}
	if err := patchtrust.SignManifest(file[:100], trusttest.Key("manifest-a")); err == nil {
		t.Fatal("signed a truncated file")
	}
	if err := patchtrust.SignManifest(file, trusttest.Key("manifest-a")); err != nil {
		t.Fatal(err)
	}
	info, err := manifest.ParseHeader(file, 0)
	if err != nil {
		t.Fatal(err)
	}
	p := pubOf("manifest-a")
	if !ed25519.Verify(p[:], file[:manifest.SignedBytes], info.Header.Signature[:]) {
		t.Fatal("the signature does not cover bytes [0, 256)")
	}
	before := info.HeaderHash
	if err := patchtrust.SignManifest(file, trusttest.Key("manifest-a")); err != nil || headerHashOf(t, file) != before {
		t.Fatal("signing changed the manifest's identity")
	}
}

func headerHashOf(t *testing.T, file []byte) cdc.Hash {
	info, err := manifest.ParseHeader(file, 0)
	if err != nil {
		t.Fatal(err)
	}
	return info.HeaderHash
}

func TestCheckOf(t *testing.T) {
	err := error(&patchtrust.Error{Check: patchtrust.CheckPointerExpired, Detail: "x"})
	if patchtrust.CheckOf(err) != patchtrust.CheckPointerExpired || !errors.Is(err, patchtrust.ErrRejected) ||
		err.Error() != "pointer-expired: x" || patchtrust.CheckOf(errors.New("io")) != "" {
		t.Fatal("Error")
	}
}

// fuzzSeeds are the shared vectors' documents: the CDN's, the variants and the syntax cases.
func fuzzSeeds(f *testing.F, suffix string) {
	_ = filepath.WalkDir(vectorsDir, func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() && strings.HasSuffix(p, suffix) && !strings.HasSuffix(p, "cases.json") &&
			!strings.HasSuffix(p, "syntax.json") {
			if b, err := os.ReadFile(p); err == nil {
				f.Add(b)
			}
		}
		return nil
	})
}

func fuzzVerifier(t *testing.T) *patchtrust.Verifier {
	v, err := patchtrust.NewVerifier(patchtrust.Target{ProductID: "vector-game", Channel: "live", Platform: "win64"},
		patchtrust.RootPair{Epoch: 1, Current: pubOf("root-1"), Next: pubOf("root-2")},
		patchtrust.Options{AllowTestKeys: true})
	if err != nil {
		t.Fatal(err)
	}
	return v
}

// FuzzParseKeyset: the parser never panics, a keyset that parses is valid and re-marshals to the exact input
// (one encoding per keyset), and verification returns a rejection or a keyset. The seeds (the shared
// vectors) run in every go test.
func FuzzParseKeyset(f *testing.F) {
	fuzzSeeds(f, ".json")
	f.Fuzz(func(t *testing.T, b []byte) {
		ks, err := patchtrust.ParseKeyset(b)
		if err == nil {
			out, err := ks.Marshal()
			if err != nil || !bytes.Equal(out, b) {
				t.Fatalf("a parsed keyset re-marshals differently: %v\n%s\n%s", err, b, out)
			}
		}
		got, err := fuzzVerifier(t).VerifyKeyset(b, patchtrust.State{})
		if (err == nil) == (got == nil) || (err != nil && !errors.Is(err, patchtrust.ErrRejected)) {
			t.Fatalf("VerifyKeyset: %v", err)
		}
	})
}

// FuzzParsePointer: as FuzzParseKeyset, for pointers, verified against the vectors' keyset.
func FuzzParsePointer(f *testing.F) {
	fuzzSeeds(f, ".json")
	ksBytes, err := os.ReadFile(filepath.Join(vectorsDir, "cdn", "keys", "vector-game", "keyset.json"))
	if err != nil {
		f.Fatal(err)
	}
	f.Fuzz(func(t *testing.T, b []byte) {
		p, err := patchtrust.ParsePointer(b)
		if err == nil {
			out, err := p.Marshal()
			if err != nil || !bytes.Equal(out, b) {
				t.Fatalf("a parsed pointer re-marshals differently: %v\n%s\n%s", err, b, out)
			}
		}
		v := fuzzVerifier(t)
		ks, err := v.VerifyKeyset(ksBytes, patchtrust.State{})
		if err != nil {
			t.Fatal(err)
		}
		got, err := v.VerifyPointer(b, ks, 1790000000, patchtrust.State{})
		if (err == nil) == (got == nil) || (err != nil && !errors.Is(err, patchtrust.ErrRejected)) {
			t.Fatalf("VerifyPointer: %v", err)
		}
	})
}

// FuzzVerifyManifestHeader: the manifest checks never panic on hostile .hman bytes.
func FuzzVerifyManifestHeader(f *testing.F) {
	fuzzSeeds(f, ".hman")
	ksBytes, err := os.ReadFile(filepath.Join(vectorsDir, "cdn", "keys", "vector-game", "keyset.json"))
	if err != nil {
		f.Fatal(err)
	}
	f.Fuzz(func(t *testing.T, b []byte) {
		v := fuzzVerifier(t)
		ks, err := v.VerifyKeyset(ksBytes, patchtrust.State{})
		if err != nil {
			t.Fatal(err)
		}
		info, err := manifest.ParseHeader(b, 0)
		ref := patchtrust.ManifestRef{BuildID: info.Header.BuildID, ManifestHash: info.HeaderHash,
			CompatEpoch: info.Header.CompatEpoch}
		if _, err2 := v.VerifyManifest(b, ks, ref, 1790000000); err == nil && err2 != nil &&
			!errors.Is(err2, patchtrust.ErrRejected) {
			t.Fatalf("VerifyManifest: %v", err2)
		}
	})
}
