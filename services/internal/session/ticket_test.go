package session

import (
	"context"
	"encoding/base64"
	"errors"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/keyring"
	"github.com/PageMastr/scifi-test/services/pkg/testkit"
)

var t0 = time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC)

func newRing(t *testing.T) *keyring.Ring {
	t.Helper()
	r, err := keyring.New("test", nil, t0)
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func TestTicketRoundTripAndRotation(t *testing.T) {
	ring := newRing(t)
	s := NewTicketSealer(ring)
	in := &Ticket{SessionID: 1<<63 + 5, AccountID: 42, CharacterID: 7, ZoneID: 1001, Epoch: 3, ContentBuild: 99,
		Flags: 0x02, IssuedAt: t0, ExpiresAt: t0.Add(5 * time.Minute)}
	tok, err := s.Seal(in)
	if err != nil {
		t.Fatal(err)
	}
	if raw, _ := base64.RawURLEncoding.DecodeString(tok); len(raw) != ticketSealedLen {
		t.Fatalf("ticket length %d", len(raw))
	}
	out, err := s.Open(tok, t0.Add(time.Minute))
	if err != nil || *out != *in {
		t.Fatalf("round trip: %+v %v", out, err)
	}
	tok2, _ := s.Seal(in)
	if tok2 == tok {
		t.Fatal("nonce reuse")
	}
	// After a rotation, tickets sealed under the previous key still open.
	if _, err := ring.Add(nil, t0); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Open(tok, t0.Add(time.Minute)); err != nil {
		t.Fatalf("old-key ticket after rotation: %v", err)
	}
	newTok, _ := s.Seal(in)
	ring.Prune(1)
	if _, err := s.Open(tok, t0.Add(time.Minute)); !errors.Is(err, ErrTicketInvalid) {
		t.Fatalf("pruned key must invalidate: %v", err)
	}
	if _, err := s.Open(newTok, t0.Add(time.Minute)); err != nil {
		t.Fatalf("current key: %v", err)
	}
}

func TestTicketRejects(t *testing.T) {
	s := NewTicketSealer(newRing(t))
	tok, _ := s.Seal(&Ticket{SessionID: 1, ExpiresAt: t0.Add(time.Minute)})
	if _, err := s.Open(tok, t0.Add(time.Minute)); !errors.Is(err, ErrTicketExpired) {
		t.Fatalf("expiry boundary: %v", err)
	}
	raw, _ := base64.RawURLEncoding.DecodeString(tok)
	flip := func(i int) string {
		b := append([]byte(nil), raw...)
		b[i] ^= 1
		return base64.RawURLEncoding.EncodeToString(b)
	}
	for name, bad := range map[string]string{
		"garbage": "!!!", "short": tok[:20], "version": flip(0), "key id": flip(1), "nonce": flip(10),
		"ciphertext": flip(40), "tag": flip(len(raw) - 1),
	} {
		if _, err := s.Open(bad, t0); !errors.Is(err, ErrTicketInvalid) {
			t.Errorf("%s: %v", name, err)
		}
	}
	// Another shard's sealer cannot open it.
	if _, err := NewTicketSealer(newRing(t)).Open(tok, t0); !errors.Is(err, ErrTicketInvalid) {
		t.Fatalf("foreign key: %v", err)
	}
}

func TestStoreLifecycle(t *testing.T) {
	r := testkit.StartRedis(t)
	st := NewStore(r.Client)
	ctx := context.Background()
	if err := st.Ping(ctx); err != nil {
		t.Fatal(err)
	}
	a := &Session{ID: 10, AccountID: 1, CharacterID: 5, ZoneID: 1001, ContentBuild: 7, CreatedAt: t0}
	prev, err := st.Create(ctx, a, time.Hour, time.Minute)
	if err != nil || prev != 0 || a.Epoch != 1 {
		t.Fatalf("create: %d %v", prev, err)
	}
	got, err := st.Get(ctx, 10)
	if err != nil || got.AccountID != 1 || got.CharacterID != 5 || got.ZoneID != 1001 || got.ContentBuild != 7 ||
		got.Epoch != 1 || got.Ended || !got.CreatedAt.Equal(t0) {
		t.Fatalf("get: %+v %v", got, err)
	}
	if cur, _ := st.CurrentFor(ctx, 1); cur != 10 {
		t.Fatalf("current %d", cur)
	}
	if _, err := st.Get(ctx, 11); !errors.Is(err, ErrNotFound) {
		t.Fatalf("missing: %v", err)
	}

	// Epoch CAS.
	if e, err := st.AdvanceEpoch(ctx, 1, a, time.Hour); err != nil || e != 2 {
		t.Fatalf("advance: %d %v", e, err)
	}
	if _, err := st.AdvanceEpoch(ctx, 1, a, time.Hour); !errors.Is(err, ErrEpochMismatch) {
		t.Fatalf("stale epoch: %v", err)
	}

	// Supersede.
	b := &Session{ID: 20, AccountID: 1, CreatedAt: t0}
	prev, _ = st.Create(ctx, b, time.Hour, time.Minute)
	if prev != 10 {
		t.Fatalf("superseded id %d", prev)
	}
	if ok, err := st.End(ctx, 10, 1, time.Minute); !ok || err != nil {
		t.Fatalf("end: %v %v", ok, err)
	}
	if cur, _ := st.CurrentFor(ctx, 1); cur != 20 {
		t.Fatal("ending the old session must not clear the new one")
	}
	if _, err := st.AdvanceEpoch(ctx, 2, a, time.Hour); !errors.Is(err, ErrEnded) {
		t.Fatalf("ended: %v", err)
	}
	if ok, _ := st.End(ctx, 999, 1, time.Minute); ok {
		t.Fatal("ending an unknown session")
	}

	// Valkey loss: a valid ticket rebuilds its session at epoch+1...
	r.Mini.FlushAll()
	c := &Session{ID: 30, AccountID: 2, CharacterID: 9, ZoneID: 1001, CreatedAt: t0}
	if e, err := st.AdvanceEpoch(ctx, 4, c, time.Hour); err != nil || e != 5 {
		t.Fatalf("restore: %d %v", e, err)
	}
	if got, _ := st.Get(ctx, 30); got.Epoch != 5 || got.CharacterID != 9 {
		t.Fatalf("restored %+v", got)
	}
	// ...but not when the account has moved on to another session.
	r.Mini.FlushAll()
	_, _ = st.Create(ctx, &Session{ID: 40, AccountID: 2, CreatedAt: t0}, time.Hour, time.Minute)
	if _, err := st.AdvanceEpoch(ctx, 5, c, time.Hour); !errors.Is(err, ErrSuperseded) {
		t.Fatalf("superseded restore: %v", err)
	}

	// TTLs: touch slides, tombstones expire.
	d := &Session{ID: 50, AccountID: 3, CreatedAt: t0}
	_, _ = st.Create(ctx, d, time.Minute, time.Minute)
	r.Mini.FastForward(50 * time.Second)
	if err := st.Touch(ctx, d, time.Minute); err != nil {
		t.Fatal(err)
	}
	r.Mini.FastForward(50 * time.Second)
	if _, err := st.Get(ctx, 50); err != nil {
		t.Fatal("touch did not slide the TTL")
	}
	r.Mini.FastForward(2 * time.Minute)
	if _, err := st.Get(ctx, 50); !errors.Is(err, ErrNotFound) {
		t.Fatal("session did not expire")
	}
}
