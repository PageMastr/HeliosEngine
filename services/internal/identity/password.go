package identity

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"runtime"
	"strings"
	"time"

	"golang.org/x/crypto/argon2"
)

// HashParams are argon2id cost parameters. 05 §1.1 fixes production at m=64 MiB, t=3, p=1
// (~80 ms per core); tests use tiny values.
type HashParams struct {
	MemoryKiB   uint32
	Iterations  uint32
	Parallelism uint8
	SaltLen     uint32
	KeyLen      uint32
}

// DefaultHashParams returns the production parameters.
func DefaultHashParams() HashParams {
	return HashParams{MemoryKiB: 64 * 1024, Iterations: 3, Parallelism: 1, SaltLen: 16, KeyLen: 32}
}

// Limits on parameters accepted from stored hashes, so a corrupt row cannot make one login
// allocate gigabytes or spin for minutes.
const (
	maxMemoryKiB  = 1 << 20 // 1 GiB
	maxIterations = 64
	maxKeyLen     = 128
	// MaxPasswordBytes bounds the input; argon2 would hash anything, but a 1 MB "password" is abuse.
	MaxPasswordBytes = 1024
)

// ErrHasherBusy is returned when the caller gave up waiting for a hashing slot.
var ErrHasherBusy = errors.New("identity: password hasher saturated")

// ErrBadHash is returned for an unparseable stored hash.
var ErrBadHash = errors.New("identity: malformed password hash")

// Hasher computes and verifies argon2id hashes behind a semaphore of MaxConcurrent slots
// (default 2 x GOMAXPROCS), so a login storm queues instead of exhausting memory: at 64 MiB per
// hash, 16 concurrent hashes already need 1 GiB. Safe for concurrent use.
type Hasher struct {
	params HashParams
	slots  chan struct{}
	// Observe, if set, is called with the time spent waiting for a slot and hashing.
	Observe func(wait, work time.Duration)
	// MaxWait bounds the queueing for a slot (DefaultMaxWait). Past it the caller gets
	// ErrHasherBusy and answers "unavailable, retry": a storm is shed with a clear retry signal
	// instead of queueing without limit while clients time out.
	MaxWait time.Duration
	dummy   string
}

// DefaultMaxWait is the default bound on waiting for an argon2 slot.
const DefaultMaxWait = 5 * time.Second

// NewHasher returns a hasher. maxConcurrent ≤ 0 means 2 x GOMAXPROCS.
func NewHasher(p HashParams, maxConcurrent int) *Hasher {
	if p.SaltLen == 0 {
		p.SaltLen = 16
	}
	if p.KeyLen == 0 {
		p.KeyLen = 32
	}
	if maxConcurrent <= 0 {
		maxConcurrent = 2 * runtime.GOMAXPROCS(0)
	}
	h := &Hasher{params: p, slots: make(chan struct{}, maxConcurrent), MaxWait: DefaultMaxWait}
	// A fixed hash with the current parameters, verified against for unknown accounts so that
	// "no such account" costs the same as "wrong password" (no user enumeration by timing).
	h.dummy = encodePHC(p, make([]byte, p.SaltLen), make([]byte, p.KeyLen))
	return h
}

// Params returns the parameters new hashes are created with.
func (h *Hasher) Params() HashParams { return h.params }

func (h *Hasher) acquire(ctx context.Context) (time.Duration, error) {
	start := time.Now()
	select {
	case h.slots <- struct{}{}:
		return 0, nil
	default:
	}
	var expired <-chan time.Time
	if h.MaxWait > 0 {
		t := time.NewTimer(h.MaxWait)
		defer t.Stop()
		expired = t.C
	}
	select {
	case h.slots <- struct{}{}:
		return time.Since(start), nil
	case <-ctx.Done():
		return time.Since(start), ErrHasherBusy
	case <-expired:
		return time.Since(start), ErrHasherBusy
	}
}

func (h *Hasher) release() { <-h.slots }

// Hash returns a PHC-format argon2id hash of password with a random salt.
func (h *Hasher) Hash(ctx context.Context, password string) (string, error) {
	if len(password) > MaxPasswordBytes {
		return "", fmt.Errorf("identity: password longer than %d bytes", MaxPasswordBytes)
	}
	salt := make([]byte, h.params.SaltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	wait, err := h.acquire(ctx)
	if err != nil {
		return "", err
	}
	start := time.Now()
	key := argon2.IDKey([]byte(password), salt, h.params.Iterations, h.params.MemoryKiB, h.params.Parallelism, h.params.KeyLen)
	h.release()
	h.observe(wait, time.Since(start))
	return encodePHC(h.params, salt, key), nil
}

// Verify checks password against a stored PHC hash in constant time. needsRehash reports that
// the stored hash uses different parameters than the hasher's current ones, so the caller
// should re-hash after a successful login (parameter upgrades roll out on login).
func (h *Hasher) Verify(ctx context.Context, password, encoded string) (ok, needsRehash bool, err error) {
	p, salt, want, err := decodePHC(encoded)
	if err != nil {
		return false, false, err
	}
	if len(password) > MaxPasswordBytes {
		return false, false, nil
	}
	wait, err := h.acquire(ctx)
	if err != nil {
		return false, false, err
	}
	start := time.Now()
	got := argon2.IDKey([]byte(password), salt, p.Iterations, p.MemoryKiB, p.Parallelism, uint32(len(want)))
	h.release()
	h.observe(wait, time.Since(start))
	ok = subtle.ConstantTimeCompare(got, want) == 1
	needsRehash = p.MemoryKiB != h.params.MemoryKiB || p.Iterations != h.params.Iterations ||
		p.Parallelism != h.params.Parallelism || uint32(len(want)) != h.params.KeyLen || uint32(len(salt)) != h.params.SaltLen
	return ok, needsRehash, nil
}

// DummyVerify burns the same work as a real verification and always fails.
func (h *Hasher) DummyVerify(ctx context.Context, password string) error {
	_, _, err := h.Verify(ctx, password, h.dummy)
	return err
}

func (h *Hasher) observe(wait, work time.Duration) {
	if h.Observe != nil {
		h.Observe(wait, work)
	}
}

var b64 = base64.RawStdEncoding

func encodePHC(p HashParams, salt, key []byte) string {
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s", argon2.Version, p.MemoryKiB, p.Iterations, p.Parallelism,
		b64.EncodeToString(salt), b64.EncodeToString(key))
}

func decodePHC(s string) (HashParams, []byte, []byte, error) {
	parts := strings.Split(s, "$")
	// "", "argon2id", "v=19", "m=..,t=..,p=..", salt, hash
	if len(parts) != 6 || parts[0] != "" || parts[1] != "argon2id" {
		return HashParams{}, nil, nil, ErrBadHash
	}
	var version int
	if _, err := fmt.Sscanf(parts[2], "v=%d", &version); err != nil || version != argon2.Version {
		return HashParams{}, nil, nil, ErrBadHash
	}
	var p HashParams
	var par uint32
	if _, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &p.MemoryKiB, &p.Iterations, &par); err != nil {
		return HashParams{}, nil, nil, ErrBadHash
	}
	salt, err1 := b64.DecodeString(parts[4])
	key, err2 := b64.DecodeString(parts[5])
	if err1 != nil || err2 != nil || len(salt) < 8 || len(key) < 16 || len(key) > maxKeyLen ||
		p.MemoryKiB < 8 || p.MemoryKiB > maxMemoryKiB || p.Iterations < 1 || p.Iterations > maxIterations ||
		par < 1 || par > 255 || p.MemoryKiB < 8*par {
		return HashParams{}, nil, nil, ErrBadHash
	}
	p.Parallelism = uint8(par)
	p.SaltLen = uint32(len(salt))
	p.KeyLen = uint32(len(key))
	return p, salt, key, nil
}
