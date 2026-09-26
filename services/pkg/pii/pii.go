// Package pii holds the primitives that protect direct PII (05 §6.6, Phase 0 rule): per-subject
// data keys (DEKs) wrapped by a key-encryption key (KEK), field encryption with
// XChaCha20-Poly1305 bound to its table, column and row, and keyed blind indexes for lookups.
//
// Deleting a subject's wrapped DEK makes every copy of its ciphertext unreadable (crypto-shredding),
// so callers must never persist or log an unwrapped DEK. Dev keeps the KEK and the blind-index
// pepper in local keyring files; staging and prod mount them from the secret store until the KMS
// integration replaces the file-backed KEK and BlindIndex. That is a Phase 0 deviation: 05 §6.5
// keeps the KEKs in KMS and says the pepper never leaves it.
//
// Everything here is safe for concurrent use.
package pii

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"math"

	"golang.org/x/crypto/chacha20poly1305"

	"github.com/PageMastr/scifi-test/services/pkg/keyring"
)

// DEKBytes is the size of a data key (256 bits, 05 §6.6).
const DEKBytes = chacha20poly1305.KeySize

// DEK is one subject's data key. Keep it in memory only, as briefly as possible.
type DEK [DEKBytes]byte

// Clear overwrites the key (best effort: Go may have copied it).
func (k *DEK) Clear() { clear(k[:]) }

// NewDEK returns a fresh random data key.
func NewDEK() (DEK, error) {
	var k DEK
	if _, err := rand.Read(k[:]); err != nil {
		return DEK{}, fmt.Errorf("pii: dek: %w", err)
	}
	return k, nil
}

// Sealed-value layout: format byte, 24-byte random nonce, ciphertext and 16-byte tag.
const (
	formatV1 = 1
	// Overhead is the number of bytes Seal adds to a plaintext.
	Overhead = 1 + chacha20poly1305.NonceSizeX + chacha20poly1305.Overhead
	// MaxPlaintext bounds what Seal accepts and Open returns; PII fields are small.
	MaxPlaintext = 64 << 10
)

// Errors.
var (
	// ErrDecrypt is returned for a value that does not open under the given key and AAD: a
	// wrong key, a value moved to another row or column, or tampering.
	ErrDecrypt = errors.New("pii: value does not decrypt")
	// ErrUnknownKEK is returned when a wrapped DEK names a KEK version the keyring lacks.
	ErrUnknownKEK = errors.New("pii: unknown kek version")
)

// aadKeyed marks AADKey's encoding in the table's length prefix.
const aadKeyed = 1 << 31

// AAD builds the associated data that binds a sealed value to where it is stored: table,
// column and row ID (05 §6.6). The encoding is length-prefixed, so no two locations share it.
func AAD(table, column string, rowID int64) []byte {
	b := make([]byte, 0, 4+len(table)+4+len(column)+8)
	b = binary.BigEndian.AppendUint32(b, uint32(len(table)))
	b = append(b, table...)
	b = binary.BigEndian.AppendUint32(b, uint32(len(column)))
	b = append(b, column...)
	return binary.BigEndian.AppendUint64(b, uint64(rowID))
}

// AADKey is AAD for a row whose key is not a 64-bit ID (a token hash, for example). The key is
// length-prefixed too, and the table's length prefix has its top bit set, so AADKey never equals
// an AAD (whose table is shorter than 2 GiB) whatever the key's length.
func AADKey(table, column string, rowKey []byte) []byte {
	b := make([]byte, 0, 4+len(table)+4+len(column)+4+len(rowKey))
	b = binary.BigEndian.AppendUint32(b, uint32(len(table))|aadKeyed)
	b = append(b, table...)
	b = binary.BigEndian.AppendUint32(b, uint32(len(column)))
	b = append(b, column...)
	b = binary.BigEndian.AppendUint32(b, uint32(len(rowKey)))
	return append(b, rowKey...)
}

func seal(key []byte, aad, plaintext []byte) ([]byte, error) {
	if len(plaintext) > MaxPlaintext {
		return nil, fmt.Errorf("pii: plaintext of %d bytes exceeds %d", len(plaintext), MaxPlaintext)
	}
	aead, err := chacha20poly1305.NewX(key)
	if err != nil {
		return nil, err
	}
	out := make([]byte, 1+chacha20poly1305.NonceSizeX, Overhead+len(plaintext))
	out[0] = formatV1
	if _, err := rand.Read(out[1:]); err != nil {
		return nil, fmt.Errorf("pii: nonce: %w", err)
	}
	// The format byte is authenticated too, so it cannot be swapped for a future format.
	return aead.Seal(out, out[1:], plaintext, append([]byte{formatV1}, aad...)), nil
}

func open(key []byte, aad, sealed []byte) ([]byte, error) {
	if len(sealed) < Overhead || len(sealed) > Overhead+MaxPlaintext || sealed[0] != formatV1 {
		return nil, ErrDecrypt
	}
	aead, err := chacha20poly1305.NewX(key)
	if err != nil {
		return nil, err
	}
	nonce := sealed[1 : 1+chacha20poly1305.NonceSizeX]
	pt, err := aead.Open(nil, nonce, sealed[1+chacha20poly1305.NonceSizeX:], append([]byte{formatV1}, aad...))
	if err != nil {
		return nil, ErrDecrypt
	}
	return pt, nil
}

// Seal encrypts plaintext under dek with XChaCha20-Poly1305 and a random nonce; aad (see AAD)
// must be supplied again to Open.
func Seal(dek *DEK, aad, plaintext []byte) ([]byte, error) { return seal(dek[:], aad, plaintext) }

// Open decrypts a value written by Seal. It returns ErrDecrypt for any mismatch.
func Open(dek *DEK, aad, sealed []byte) ([]byte, error) { return open(dek[:], aad, sealed) }

// KEK wraps and unwraps DEKs with the generations of a keyring (05 §6.6: dev uses a local key
// file). The newest generation wraps; any generation still in the ring unwraps, so pruning a
// generation that still wraps DEKs would shred those subjects: rotation must re-wrap first.
type KEK struct {
	ring *keyring.Ring
}

// NewKEK uses ring's generations as key-encryption keys.
func NewKEK(ring *keyring.Ring) (*KEK, error) {
	if ring == nil || len(ring.Keys) == 0 {
		return nil, errors.New("pii: kek keyring is empty")
	}
	for _, k := range ring.Keys {
		if k.ID > math.MaxInt32 {
			return nil, fmt.Errorf("pii: kek version %d does not fit kek_version INT", k.ID)
		}
	}
	return &KEK{ring: ring}, nil
}

// Wrap encrypts dek under the current KEK generation, bound to aad (the subject's row), and
// returns the wrapped key and the generation (kek_version) that must be passed to Unwrap.
func (k *KEK) Wrap(dek *DEK, aad []byte) (wrapped []byte, version int32, err error) {
	cur := k.ring.Current()
	wrapped, err = seal(cur.Secret, aad, dek[:])
	return wrapped, int32(cur.ID), err
}

// Unwrap recovers a DEK wrapped by Wrap. A wrong KEK, version or aad yields ErrDecrypt or
// ErrUnknownKEK.
func (k *KEK) Unwrap(version int32, wrapped, aad []byte) (DEK, error) {
	if version <= 0 {
		return DEK{}, ErrUnknownKEK
	}
	e, ok := k.ring.Get(uint32(version))
	if !ok {
		return DEK{}, ErrUnknownKEK
	}
	pt, err := open(e.Secret, aad, wrapped)
	if err != nil {
		return DEK{}, err
	}
	defer clear(pt)
	if len(pt) != DEKBytes {
		return DEK{}, ErrDecrypt
	}
	var dek DEK
	copy(dek[:], pt)
	return dek, nil
}

// BlindIndex computes keyed lookup values: HMAC-SHA256(pepper, value) (05 §6.6). Equal inputs
// give equal indexes, so a unique column can be searched without storing the value, while
// someone holding the database but not the pepper cannot test guesses.
type BlindIndex struct {
	pepper []byte
}

// NewBlindIndex uses ring's only generation as the pepper. The index has no version column, so
// the pepper cannot rotate without re-indexing every row; a ring with a second generation is
// refused rather than silently breaking every stored index.
func NewBlindIndex(ring *keyring.Ring) (*BlindIndex, error) {
	if ring == nil || len(ring.Keys) == 0 {
		return nil, errors.New("pii: pepper keyring is empty")
	}
	if len(ring.Keys) != 1 {
		return nil, fmt.Errorf("pii: pepper keyring has %d generations; a blind-index pepper cannot rotate without a re-index", len(ring.Keys))
	}
	return &BlindIndex{pepper: ring.Current().Secret}, nil
}

// Sum returns the 32-byte index of value. Callers normalize value first (e-mail: lower case).
func (b *BlindIndex) Sum(value string) []byte {
	m := hmac.New(sha256.New, b.pepper)
	m.Write([]byte(value))
	return m.Sum(nil)
}
