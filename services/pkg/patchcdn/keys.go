package patchcdn

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/patchtrust"
)

// A signing directory holds what publish needs (08 §2.10.3 names where real keys live: the root offline,
// the manifest subkey in Vault/KMS; `helios-tool product init` creates them in a later WP):
//
//	keyset.json        the root-signed keyset, copied to the CDN
//	manifest-key.json  {"dev":B,"productId":S,"id":H16,"seed":H32}: the manifest subkey (mode 0600)
//
// A dev directory (CreateDevKeys, `helios-patch publish --channel dev`) also holds
//
//	root-keys.json     {"dev":true,"productId":S,"epoch":1,"current":H32,"next":H32}: root seeds (0600)
//	roots.json         {"productId":S,"epoch":1,"current":H32,"next":H32}: the public root pair, what a
//	                   dev launcher would be stamped with (`helios-patch verify --roots`)
//	.gitignore         "*": git ignores the whole directory, wherever --data-dir puts it
//
// Dev keys are generated from the OS CSPRNG on the developer's machine, marked "dev": true, and publish signs
// only the dev channel, for loopback CDN hosts, with them. They are kept out of git twice: the directory's
// own .gitignore (written before any seed) and the repository's "helios-data/" pattern.

const devWarning = "throwaway dev keys: helios-patch signs only the dev channel (loopback CDNs) with them"

// DevKeyLifetime is how long a generated dev subkey may sign.
const DevKeyLifetime = 365 * 24 * time.Hour

// Keys is a loaded signing directory.
type Keys struct {
	Dev         bool
	KeysetBytes []byte             // keyset.json as stored (canonical)
	Keyset      *patchtrust.Keyset // parsed (its signature is checked by verify, not here)
	Manifest    ed25519.PrivateKey // the manifest subkey
	KeyID       patchtrust.KeyID
}

type subkeyFile struct {
	Dev       bool   `json:"dev"`
	Warning   string `json:"warning,omitempty"`
	ProductID string `json:"productId"`
	ID        string `json:"id"`
	Seed      string `json:"seed"`
}

type rootSeedsFile struct {
	Dev       bool   `json:"dev"`
	Warning   string `json:"warning,omitempty"`
	ProductID string `json:"productId"`
	Epoch     uint32 `json:"epoch"`
	Current   string `json:"current"`
	Next      string `json:"next"`
}

// RootsFile is roots.json: a product's public root pair.
type RootsFile struct {
	ProductID string `json:"productId"`
	Epoch     uint32 `json:"epoch"`
	Current   string `json:"current"`
	Next      string `json:"next"`
}

func decodeHex(s string, n int) ([]byte, error) {
	b, err := hex.DecodeString(s)
	if err != nil || len(b) != n {
		return nil, fmt.Errorf("expected %d bytes of hex", n)
	}
	return b, nil
}

// LoadRoots reads roots.json.
func LoadRoots(path string) (string, patchtrust.RootPair, error) {
	var f RootsFile
	b, err := os.ReadFile(path)
	if err == nil {
		err = json.Unmarshal(b, &f)
	}
	if err != nil {
		return "", patchtrust.RootPair{}, fmt.Errorf("patchcdn: roots %s: %w", path, err)
	}
	rp := patchtrust.RootPair{Epoch: f.Epoch}
	cur, err1 := decodeHex(f.Current, patchtrust.PublicKeySize)
	next, err2 := decodeHex(f.Next, patchtrust.PublicKeySize)
	if err := errors.Join(err1, err2); err != nil {
		return "", rp, fmt.Errorf("patchcdn: roots %s: %w", path, err)
	}
	copy(rp.Current[:], cur)
	copy(rp.Next[:], next)
	return f.ProductID, rp, nil
}

// LoadKeys reads a signing directory and checks that its subkey is in its keyset with the manifest role and
// that the keyset is the product's.
func LoadKeys(dir, productID string) (*Keys, error) {
	ks, err := os.ReadFile(filepath.Join(dir, "keyset.json"))
	if err != nil {
		return nil, fmt.Errorf("patchcdn: keys: %w", err)
	}
	k := &Keys{KeysetBytes: ks}
	if k.Keyset, err = patchtrust.ParseKeyset(ks); err != nil {
		return nil, fmt.Errorf("patchcdn: keys: keyset.json: %w", err)
	}
	var sf subkeyFile
	b, err := os.ReadFile(filepath.Join(dir, "manifest-key.json"))
	if err == nil {
		err = json.Unmarshal(b, &sf)
	}
	if err != nil {
		return nil, fmt.Errorf("patchcdn: keys: manifest-key.json: %w", err)
	}
	seed, err := decodeHex(sf.Seed, ed25519.SeedSize)
	if err != nil {
		return nil, fmt.Errorf("patchcdn: keys: manifest-key.json: seed: %w", err)
	}
	k.Dev, k.Manifest = sf.Dev, ed25519.NewKeyFromSeed(seed)
	k.KeyID = patchtrust.Fingerprint(patchtrust.PublicKeyOf(k.Manifest))
	switch key := k.Keyset.Key(k.KeyID); {
	case sf.ProductID != productID || k.Keyset.ProductID != productID:
		return nil, fmt.Errorf("patchcdn: keys in %s are for %q, not %q", dir, k.Keyset.ProductID, productID)
	case hex.EncodeToString(k.KeyID[:]) != sf.ID:
		return nil, fmt.Errorf("patchcdn: keys: manifest-key.json's id is not its key's fingerprint")
	case key == nil || key.Role != patchtrust.RoleManifest:
		return nil, fmt.Errorf("patchcdn: keys: key %x is not a manifest key of keyset.json", k.KeyID)
	}
	return k, nil
}

// devGitignore is the dev key directory's .gitignore: it ignores everything there, itself included.
const devGitignore = "# helios-patch dev keys: private seeds, never committed\n*\n"

// ensureGitignore writes dir's .gitignore unless it already holds devGitignore.
func ensureGitignore(dir string) error {
	path := filepath.Join(dir, ".gitignore")
	if b, err := os.ReadFile(path); err == nil && string(b) == devGitignore {
		return nil
	}
	return patchtrust.WriteFileAtomic(path, []byte(devGitignore), 0o644)
}

func writeJSON(path string, v any, perm os.FileMode) error {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	return patchtrust.WriteFileAtomic(path, append(b, '\n'), perm)
}

// CreateDevKeys writes a new dev signing directory for productID: first a .gitignore that ignores the whole
// directory, then a root pair (epoch 1), a manifest subkey valid from a day before now for DevKeyLifetime,
// and keyset version 1 signed by the current root. rnd nil means crypto/rand.
func CreateDevKeys(dir, productID string, now time.Time, rnd io.Reader) error {
	if rnd == nil {
		rnd = rand.Reader
	}
	var keys [3]ed25519.PrivateKey
	for i := range keys {
		_, priv, err := ed25519.GenerateKey(rnd)
		if err != nil {
			return err
		}
		keys[i] = priv
	}
	cur, next, sub := keys[0], keys[1], keys[2]
	pub := patchtrust.PublicKeyOf(sub)
	from := now.Add(-24 * time.Hour).Unix()
	ks := &patchtrust.Keyset{ProductID: productID, Version: 1, RootEpoch: 1, Keys: []patchtrust.KeysetKey{{
		ID: patchtrust.Fingerprint(pub), Role: patchtrust.RoleManifest, Pub: pub,
		NotBefore: uint64(max(from, 0)), NotAfter: uint64(now.Add(DevKeyLifetime).Unix()),
	}}}
	if err := ks.Sign(cur); err != nil {
		return err
	}
	ksBytes, err := ks.Marshal()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	if err := ensureGitignore(dir); err != nil {
		return err
	}
	curPub, nextPub := patchtrust.PublicKeyOf(cur), patchtrust.PublicKeyOf(next)
	id := ks.Keys[0].ID
	return errors.Join(
		writeJSON(filepath.Join(dir, "root-keys.json"), rootSeedsFile{Dev: true, Warning: devWarning,
			ProductID: productID, Epoch: 1, Current: hex.EncodeToString(cur.Seed()),
			Next: hex.EncodeToString(next.Seed())}, 0o600),
		writeJSON(filepath.Join(dir, "manifest-key.json"), subkeyFile{Dev: true, Warning: devWarning,
			ProductID: productID, ID: hex.EncodeToString(id[:]), Seed: hex.EncodeToString(sub.Seed())}, 0o600),
		writeJSON(filepath.Join(dir, "roots.json"), RootsFile{ProductID: productID, Epoch: 1,
			Current: hex.EncodeToString(curPub[:]), Next: hex.EncodeToString(nextPub[:])}, 0o644),
		patchtrust.WriteFileAtomic(filepath.Join(dir, "keyset.json"), ksBytes, 0o644),
	)
}

// LoadOrCreateDevKeys loads the dev signing directory, creating it first if it does not exist, and makes sure
// its .gitignore is there (a directory made before it was written gets one). It refuses a directory whose
// keys are not marked dev.
func LoadOrCreateDevKeys(dir, productID string, now time.Time) (*Keys, bool, error) {
	created := false
	if _, err := os.Stat(filepath.Join(dir, "manifest-key.json")); errors.Is(err, fs.ErrNotExist) {
		if err := CreateDevKeys(dir, productID, now, nil); err != nil {
			return nil, false, err
		}
		created = true
	}
	k, err := LoadKeys(dir, productID)
	if err != nil {
		return nil, false, err
	}
	if !k.Dev {
		return nil, false, fmt.Errorf("patchcdn: %s holds keys not marked dev", dir)
	}
	if err := ensureGitignore(dir); err != nil {
		return nil, false, err
	}
	return k, created, nil
}
