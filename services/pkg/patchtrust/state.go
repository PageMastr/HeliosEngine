package patchtrust

import (
	"bytes"
	"crypto/ed25519"
	"encoding/binary"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
)

// StateStore persists an install's ratchets. The launcher implements it on install.db (WP-0.17);
// FileStateStore is the tools' version. Save must be durable before the caller acts on what it verified.
type StateStore interface {
	Load() (State, error) // a store that has never been saved returns the zero State
	Save(State) error
}

// StateRecordSize is the size of a state record (EncodeState).
const StateRecordSize = 32

const stateMagic = 0x53525448 // "HTRS"

// EncodeState returns state's 32-byte record, the same bytes as engine/patch's encodeTrustState: "HTRS",
// version 0, rootEpoch (u32), keysetVersion and pointerSequence (u64), all little-endian, then the first
// 4 bytes of BLAKE2b-256 of the first 28 bytes. Pure; any goroutine.
func EncodeState(st State) [StateRecordSize]byte {
	var b [StateRecordSize]byte
	binary.LittleEndian.PutUint32(b[0:], stateMagic)
	binary.LittleEndian.PutUint32(b[8:], st.RootEpoch)
	binary.LittleEndian.PutUint64(b[12:], st.KeysetVersion)
	binary.LittleEndian.PutUint64(b[20:], st.PointerSequence)
	h := cdc.Sum(b[:28])
	copy(b[28:], h[:4])
	return b
}

// DecodeState reads a record EncodeState wrote; any other bytes (another size, magic or version, or a
// checksum that does not match) are an error. Pure; any goroutine.
func DecodeState(b []byte) (State, error) {
	if len(b) != StateRecordSize || binary.LittleEndian.Uint32(b) != stateMagic || binary.LittleEndian.Uint32(b[4:]) != 0 {
		return State{}, errors.New("not a trust state record")
	}
	if h := cdc.Sum(b[:28]); !bytes.Equal(h[:4], b[28:]) {
		return State{}, errors.New("the trust state record's checksum does not match")
	}
	return State{RootEpoch: binary.LittleEndian.Uint32(b[8:]), KeysetVersion: binary.LittleEndian.Uint64(b[12:]),
		PointerSequence: binary.LittleEndian.Uint64(b[20:])}, nil
}

// FileStateStore keeps State in a file holding one state record (EncodeState: the format of engine/patch's
// FileTrustStateStore, so either language reads the other's file), replaced atomically. Not synchronized:
// one goroutine at a time.
type FileStateStore struct{ Path string }

// Load returns the zero State when the file does not exist. A file that is not exactly one valid record is
// an error, never a reset: a reset ratchet would accept a rolled-back pointer.
func (s FileStateStore) Load() (State, error) {
	b, err := os.ReadFile(s.Path)
	if errors.Is(err, fs.ErrNotExist) {
		return State{}, nil
	}
	if err != nil {
		return State{}, err
	}
	st, err := DecodeState(b)
	if err != nil {
		return State{}, fmt.Errorf("patchtrust: state %s: %w", s.Path, err)
	}
	return st, nil
}

// Save writes the state's record to a temporary file in the same directory and renames it over the old one.
func (s FileStateStore) Save(st State) error {
	rec := EncodeState(st)
	return WriteFileAtomic(s.Path, rec[:], 0o644)
}

// WriteFileAtomic writes data to a temporary file next to path (".tmp-*"), syncs it and renames it over
// path, so a reader sees the old or the new content, never a mix; then it syncs the directory (POSIX, best
// effort) so the rename survives a power cut. A crash before the rename can leave the temporary file
// behind; no CDN layout path names one.
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
		return err
	}
	if runtime.GOOS != "windows" { // Windows cannot open a directory for FlushFileBuffers
		if d, derr := os.Open(dir); derr == nil {
			_ = d.Sync() // some filesystems refuse to sync a directory; the rename has happened either way
			_ = d.Close()
		}
	}
	return nil
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
