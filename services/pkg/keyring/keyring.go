// Package keyring stores generations of 32-byte secrets in a small JSON file: the shard netcode
// key, reconnect-ticket keys and the JWT signing seed (05 §5 `keys/`). Each generation has a
// numeric ID; the highest ID is current, older ones stay until pruned so in-flight material
// (connect tokens ≤ 45 s, tickets ≤ 5 min, JWTs ≤ 10 min) keeps verifying across a rotation.
//
// Dev generates files under <data>/keys on first run; staging/prod mount the same format from
// the secret store (05 §6.5). Files are written atomically with 0600 permissions.
package keyring

import (
	"cmp"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"time"
)

// SecretBytes is the size of every secret in a keyring.
const SecretBytes = 32

// Entry is one key generation.
type Entry struct {
	ID      uint32    `json:"id"`
	Secret  []byte    `json:"secret"` // base64 in JSON
	Created time.Time `json:"created"`
}

// Ring is the file content. Not safe for concurrent mutation; callers load it at startup.
type Ring struct {
	Version int     `json:"version"`
	Purpose string  `json:"purpose"`
	Keys    []Entry `json:"keys"`
}

// ErrEmpty is returned for a keyring without keys.
var ErrEmpty = errors.New("keyring: no keys")

// New returns a ring with one fresh key.
func New(purpose string, rnd io.Reader, now time.Time) (*Ring, error) {
	r := &Ring{Version: 1, Purpose: purpose}
	if _, err := r.Add(rnd, now); err != nil {
		return nil, err
	}
	return r, nil
}

// Load reads and validates a keyring file.
func Load(path string) (*Ring, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var r Ring
	if err := json.Unmarshal(b, &r); err != nil {
		return nil, fmt.Errorf("keyring %s: %w", path, err)
	}
	if err := r.validate(); err != nil {
		return nil, fmt.Errorf("keyring %s: %w", path, err)
	}
	return &r, nil
}

// LoadOrCreate loads path, or creates it with one fresh key when it does not exist.
// created reports whether a new file was written.
func LoadOrCreate(path, purpose string, rnd io.Reader, now time.Time) (r *Ring, created bool, err error) {
	r, err = Load(path)
	if err == nil {
		return r, false, nil
	}
	if !errors.Is(err, os.ErrNotExist) {
		return nil, false, err
	}
	r, err = New(purpose, rnd, now)
	if err != nil {
		return nil, false, err
	}
	if err := r.Save(path); err != nil {
		return nil, false, err
	}
	return r, true, nil
}

func (r *Ring) validate() error {
	if r.Version != 1 {
		return fmt.Errorf("unsupported version %d", r.Version)
	}
	if len(r.Keys) == 0 {
		return ErrEmpty
	}
	seen := map[uint32]bool{}
	for _, k := range r.Keys {
		if len(k.Secret) != SecretBytes {
			return fmt.Errorf("key %d: secret must be %d bytes", k.ID, SecretBytes)
		}
		if k.ID == 0 || seen[k.ID] {
			return fmt.Errorf("key id %d is zero or duplicated", k.ID)
		}
		seen[k.ID] = true
	}
	slices.SortFunc(r.Keys, func(a, b Entry) int { return cmp.Compare(a.ID, b.ID) })
	return nil
}

// Current returns the newest key generation.
func (r *Ring) Current() Entry { return r.Keys[len(r.Keys)-1] }

// Get returns the generation with the given ID.
func (r *Ring) Get(id uint32) (Entry, bool) {
	for _, k := range r.Keys {
		if k.ID == id {
			return k, true
		}
	}
	return Entry{}, false
}

// Add appends a fresh generation (ID = newest + 1) and returns it.
func (r *Ring) Add(rnd io.Reader, now time.Time) (Entry, error) {
	if rnd == nil {
		rnd = rand.Reader
	}
	e := Entry{ID: 1, Secret: make([]byte, SecretBytes), Created: now.UTC().Truncate(time.Second)}
	if len(r.Keys) > 0 {
		e.ID = r.Current().ID + 1
	}
	if _, err := io.ReadFull(rnd, e.Secret); err != nil {
		return Entry{}, err
	}
	r.Keys = append(r.Keys, e)
	return e, nil
}

// Prune drops all generations older than keep newest ones.
func (r *Ring) Prune(keep int) {
	if keep < 1 {
		keep = 1
	}
	if len(r.Keys) > keep {
		r.Keys = append([]Entry(nil), r.Keys[len(r.Keys)-keep:]...)
	}
}

// Save writes the ring atomically (temp file + rename) with owner-only permissions.
func (r *Ring) Save(path string) error {
	if err := r.validate(); err != nil {
		return err
	}
	b, err := json.MarshalIndent(r, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".keyring-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	// CreateTemp opens with 0600 on POSIX; on Windows the user profile ACLs protect the file.
	defer os.Remove(name) // no-op after a successful rename
	if _, err := tmp.Write(append(b, '\n')); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(name, path)
}
