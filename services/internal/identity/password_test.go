package identity

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

func fastParams() HashParams { return HashParams{MemoryKiB: 64, Iterations: 1, Parallelism: 1} }

func TestHashVerify(t *testing.T) {
	h := NewHasher(fastParams(), 2)
	ctx := context.Background()
	enc, err := h.Hash(ctx, "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(enc, "$argon2id$v=19$m=64,t=1,p=1$") {
		t.Fatalf("PHC format: %s", enc)
	}
	ok, rehash, err := h.Verify(ctx, "correct horse", enc)
	if err != nil || !ok || rehash {
		t.Fatalf("verify: %v %v %v", ok, rehash, err)
	}
	if ok, _, _ := h.Verify(ctx, "correct horsE", enc); ok {
		t.Fatal("wrong password accepted")
	}
	// Salts differ per hash.
	enc2, _ := h.Hash(ctx, "correct horse")
	if enc2 == enc {
		t.Fatal("salt reused")
	}
	// A hasher with new parameters still verifies old hashes and asks for a rehash.
	stronger := NewHasher(HashParams{MemoryKiB: 128, Iterations: 2, Parallelism: 1}, 1)
	ok, rehash, err = stronger.Verify(ctx, "correct horse", enc)
	if err != nil || !ok || !rehash {
		t.Fatalf("upgrade path: %v %v %v", ok, rehash, err)
	}
	if err := h.DummyVerify(ctx, "x"); err != nil {
		t.Fatal(err)
	}
	if _, err := h.Hash(ctx, strings.Repeat("x", MaxPasswordBytes+1)); err == nil {
		t.Fatal("oversized password hashed")
	}
	if ok, _, _ := h.Verify(ctx, strings.Repeat("x", MaxPasswordBytes+1), enc); ok {
		t.Fatal("oversized password verified")
	}
}

func TestKnownVectors(t *testing.T) {
	// Independent implementation: libargon2 (via argon2-cffi). The first is the argon2id vector
	// from the reference implementation's test.c; the second uses tiny memory.
	h := NewHasher(fastParams(), 1)
	for _, enc := range []string{
		"$argon2id$v=19$m=65536,t=2,p=1$c29tZXNhbHQ$CTFhFdXPJO1aFaMaO6Mm5c8y7cJHAph8ArZWb2GRPPc",
		"$argon2id$v=19$m=64,t=2,p=1$c29tZXNhbHQ$FqGkmHNGCd0BRW2kBt6fPZ2pPmyGwwChL8FGUhTOSSI",
	} {
		ok, _, err := h.Verify(context.Background(), "password", enc)
		if err != nil || !ok {
			t.Fatalf("reference hash %s rejected: %v %v", enc, ok, err)
		}
	}
}

func TestDecodeRejects(t *testing.T) {
	h := NewHasher(fastParams(), 1)
	for _, enc := range []string{
		"",
		"$argon2i$v=19$m=64,t=1,p=1$c29tZXNhbHQ$3N8QYtZpgp3ZmSX/eNNpvlKXcoDDrxDZNtGBEuxaoDM",
		"$argon2id$v=16$m=64,t=1,p=1$c29tZXNhbHQ$3N8QYtZpgp3ZmSX/eNNpvlKXcoDDrxDZNtGBEuxaoDM",
		"$argon2id$v=19$m=99999999,t=1,p=1$c29tZXNhbHQ$3N8QYtZpgp3ZmSX/eNNpvlKXcoDDrxDZNtGBEuxaoDM",
		"$argon2id$v=19$m=64,t=1000,p=1$c29tZXNhbHQ$3N8QYtZpgp3ZmSX/eNNpvlKXcoDDrxDZNtGBEuxaoDM",
		"$argon2id$v=19$m=64,t=1,p=1$!!$3N8QYtZpgp3ZmSX/eNNpvlKXcoDDrxDZNtGBEuxaoDM",
		"$argon2id$v=19$m=64,t=1,p=1$c29tZXNhbHQ$AAAA",
		"$argon2id$v=19$m=64,t=1,p=0$c29tZXNhbHQ$3N8QYtZpgp3ZmSX/eNNpvlKXcoDDrxDZNtGBEuxaoDM",
	} {
		if _, _, err := h.Verify(context.Background(), "password", enc); !errors.Is(err, ErrBadHash) {
			t.Errorf("%q: %v", enc, err)
		}
	}
}

func TestSemaphoreBoundsConcurrency(t *testing.T) {
	h := NewHasher(fastParams(), 1)
	h.slots <- struct{}{} // occupy the only slot
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if _, err := h.Hash(ctx, "pw"); !errors.Is(err, ErrHasherBusy) {
		t.Fatalf("expected busy, got %v", err)
	}
	<-h.slots
	var waits int
	h.Observe = func(_, _ time.Duration) { waits++ }
	if _, err := h.Hash(context.Background(), "pw"); err != nil || waits != 1 {
		t.Fatalf("after release: %v %d", err, waits)
	}
	// A storm is shed after MaxWait even when the caller would wait forever.
	h.slots <- struct{}{}
	h.MaxWait = 30 * time.Millisecond
	start := time.Now()
	if _, _, err := h.Verify(context.Background(), "pw", h.dummy); !errors.Is(err, ErrHasherBusy) || time.Since(start) > 5*time.Second {
		t.Fatalf("bounded wait: %v after %v", err, time.Since(start))
	}
	<-h.slots
	if def := NewHasher(fastParams(), 0); cap(def.slots) < 2 || def.MaxWait != DefaultMaxWait {
		t.Fatal("default slots must be 2 x GOMAXPROCS")
	}
	if DefaultHashParams().MemoryKiB != 65536 || DefaultHashParams().Iterations != 3 {
		t.Fatal("05 §1.1 parameters")
	}
}
