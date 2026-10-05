package patchtrust

import (
	"crypto/ed25519"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"

	"github.com/PageMastr/scifi-test/services/pkg/manifest"
)

// StateStore persists an install's ratchets. The launcher implements it on install.db (WP-0.17);
// FileStateStore is the tools' version. Save must be durable before the caller acts on what it verified.
type StateStore interface {
	Load() (State, error) // a store that has never been saved returns the zero State
	Save(State) error
}

// FileStateStore keeps State as a small JSON file, replaced atomically.
type FileStateStore struct{ Path string }

type stateFile struct {
	RootEpoch       uint32 `json:"rootEpoch"`
	KeysetVersion   uint64 `json:"keysetVersion"`
	PointerSequence uint64 `json:"pointerSequence"`
}

// Load returns the zero State when the file does not exist. A file that does not parse is an error, never
// a reset: a reset ratchet would accept a rolled-back pointer.
func (s FileStateStore) Load() (State, error) {
	b, err := os.ReadFile(s.Path)
	if errors.Is(err, fs.ErrNotExist) {
		return State{}, nil
	}
	if err != nil {
		return State{}, err
	}
	var f stateFile
	if err := json.Unmarshal(b, &f); err != nil {
		return State{}, fmt.Errorf("patchtrust: state %s: %w", s.Path, err)
	}
	return State(f), nil
}

// Save writes the state to a temporary file in the same directory and renames it over the old one.
func (s FileStateStore) Save(st State) error {
	b, _ := json.Marshal(stateFile(st))
	return WriteFileAtomic(s.Path, append(b, '\n'), 0o644)
}

// WriteFileAtomic writes data to a temporary file next to path, syncs it and renames it over path, so a
// reader sees the old or the new content, never a mix.
func WriteFileAtomic(path string, data []byte, perm os.FileMode) error {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".tmp-*")
	if err != nil {
		return err
	}
	tmp := f.Name()
	_, err = f.Write(data)
	if err == nil {
		err = f.Sync()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err == nil {
		err = os.Chmod(tmp, perm)
	}
	if err == nil {
		err = os.Rename(tmp, path)
	}
	if err != nil {
		_ = os.Remove(tmp)
	}
	return err
}

// SignManifest signs a marshaled .hman file in place: an Ed25519 signature over bytes [0, 256) written to
// bytes [288, 352). The header's keyId (set before marshaling, inside the signed bytes) must be the key's
// fingerprint.
func SignManifest(file []byte, key ed25519.PrivateKey) error {
	if len(file) < manifest.HeaderSize || binary.LittleEndian.Uint32(file) != manifest.Magic {
		return errors.New("patchtrust: not a .hman file")
	}
	id := Fingerprint(PublicKeyOf(key))
	if string(file[224:224+KeyIDSize]) != string(id[:]) {
		return fmt.Errorf("patchtrust: the manifest's keyId is not key %x", id)
	}
	copy(file[288:288+SignatureSize], ed25519.Sign(key, file[:manifest.SignedBytes]))
	return nil
}
