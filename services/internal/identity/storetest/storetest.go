// Package storetest is the conformance suite every identity.Store implementation must pass.
// Unit tests run it against MemStore; the integration build runs it against PostgreSQL.
package storetest

import (
	"bytes"
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"sync"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/internal/identity"
)

// Factory returns a fresh, empty store for one subtest.
type Factory func(t *testing.T) identity.Store

var base = time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC)

func account(id int64, email, handle string, disc int16) *identity.Account {
	return &identity.Account{ID: id, Email: email, EmailNorm: email, Handle: handle, HandleNorm: handle,
		Discriminator: disc, PasswordHash: "$argon2id$x", CreatedAt: base, UpdatedAt: base}
}

func hash(s string) []byte {
	h := sha256.Sum256([]byte(s))
	return h[:]
}

// Run executes the suite.
func Run(t *testing.T, newStore Factory) {
	ctx := context.Background()

	t.Run("accounts", func(t *testing.T) {
		s := newStore(t)
		a := account(1, "a@x.io", "ada", 42)
		if err := s.CreateAccount(ctx, a, identity.NewAudit(base, 1, 1, identity.ActionRegister, "1.2.3.4", nil)); err != nil {
			t.Fatal(err)
		}
		if err := s.CreateAccount(ctx, account(2, "a@x.io", "bob", 1), nil); !errors.Is(err, identity.ErrEmailTaken) {
			t.Fatalf("duplicate email: %v", err)
		}
		if err := s.CreateAccount(ctx, account(3, "c@x.io", "ada", 42), nil); !errors.Is(err, identity.ErrTagTaken) {
			t.Fatalf("duplicate tag: %v", err)
		}
		if err := s.CreateAccount(ctx, account(4, "d@x.io", "ada", 43), nil); err != nil {
			t.Fatalf("same handle, other discriminator: %v", err)
		}
		for name, get := range map[string]func() (*identity.Account, error){
			"id":    func() (*identity.Account, error) { return s.AccountByID(ctx, 1) },
			"email": func() (*identity.Account, error) { return s.AccountByEmail(ctx, "a@x.io") },
			"tag":   func() (*identity.Account, error) { return s.AccountByTag(ctx, "ada", 42) },
		} {
			got, err := get()
			if err != nil || got.ID != 1 || got.Handle != "ada" || !got.CreatedAt.Equal(base) || got.BannedUntil != nil {
				t.Fatalf("lookup by %s: %+v %v", name, got, err)
			}
		}
		if _, err := s.AccountByID(ctx, 99); !errors.Is(err, identity.ErrNotFound) {
			t.Fatalf("missing: %v", err)
		}
		if _, err := s.AccountByTag(ctx, "ada", 44); !errors.Is(err, identity.ErrNotFound) {
			t.Fatalf("missing tag: %v", err)
		}
		if err := s.SetPasswordHash(ctx, 1, "$argon2id$new", base.Add(time.Hour)); err != nil {
			t.Fatal(err)
		}
		if err := s.SetPasswordHash(ctx, 99, "x", base); !errors.Is(err, identity.ErrNotFound) {
			t.Fatalf("rehash missing: %v", err)
		}
		if err := s.RecordLogin(ctx, 1, base.Add(2*time.Hour), nil); err != nil {
			t.Fatal(err)
		}
		got, _ := s.AccountByID(ctx, 1)
		if got.PasswordHash != "$argon2id$new" || got.LastLoginAt == nil || !got.LastLoginAt.Equal(base.Add(2*time.Hour)) {
			t.Fatalf("updates: %+v", got)
		}
	})

	t.Run("refresh rotation and reuse", func(t *testing.T) {
		s := newStore(t)
		if err := s.CreateAccount(ctx, account(1, "a@x.io", "ada", 1), nil); err != nil {
			t.Fatal(err)
		}
		t0 := &identity.RefreshToken{Hash: hash("t0"), FamilyID: 500, AccountID: 1, IssuedAt: base, ExpiresAt: base.Add(time.Hour)}
		if err := s.InsertRefreshToken(ctx, t0); err != nil {
			t.Fatal(err)
		}
		t1 := &identity.RefreshToken{Hash: hash("t1"), IssuedAt: base, ExpiresAt: base.Add(time.Hour)}
		old, err := s.RotateRefreshToken(ctx, hash("t0"), t1, base.Add(time.Minute), nil)
		if err != nil || old.AccountID != 1 || old.FamilyID != 500 || t1.FamilyID != 500 || t1.AccountID != 1 {
			t.Fatalf("rotate: %+v %+v %v", old, t1, err)
		}
		// Reusing t0 revokes the family, including the live t1.
		reuse := identity.NewAudit(base, 0, 0, identity.ActionRefreshReuse, "", nil)
		if _, err := s.RotateRefreshToken(ctx, hash("t0"), &identity.RefreshToken{Hash: hash("t2"), IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base.Add(2*time.Minute), reuse); !errors.Is(err, identity.ErrTokenReused) {
			t.Fatalf("reuse: %v", err)
		}
		if reuse.Seq == 0 || reuse.Subject != 1 {
			t.Fatalf("reuse audit not appended with subject: %+v", reuse)
		}
		if _, err := s.RotateRefreshToken(ctx, hash("t1"), &identity.RefreshToken{Hash: hash("t3"), IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base.Add(3*time.Minute), nil); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("revoked successor must be invalid: %v", err)
		}
		if _, err := s.RotateRefreshToken(ctx, hash("unknown"), &identity.RefreshToken{Hash: hash("t4")}, base, nil); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("unknown: %v", err)
		}
		// Expiry.
		e0 := &identity.RefreshToken{Hash: hash("e0"), FamilyID: 600, AccountID: 1, IssuedAt: base, ExpiresAt: base.Add(time.Minute)}
		_ = s.InsertRefreshToken(ctx, e0)
		if _, err := s.RotateRefreshToken(ctx, hash("e0"), &identity.RefreshToken{Hash: hash("e1"), IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base.Add(time.Minute), nil); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("expired: %v", err)
		}
		// Logout revokes the family.
		l0 := &identity.RefreshToken{Hash: hash("l0"), FamilyID: 700, AccountID: 1, IssuedAt: base, ExpiresAt: base.Add(time.Hour)}
		_ = s.InsertRefreshToken(ctx, l0)
		got, err := s.RevokeFamilyOf(ctx, hash("l0"), base, identity.NewAudit(base, 0, 0, identity.ActionLogout, "", nil))
		if err != nil || got.FamilyID != 700 {
			t.Fatalf("logout: %v", err)
		}
		if _, err := s.RotateRefreshToken(ctx, hash("l0"), &identity.RefreshToken{Hash: hash("l1"), IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base, nil); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("after logout: %v", err)
		}
		if _, err := s.RevokeFamilyOf(ctx, hash("nope"), base, nil); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("logout unknown: %v", err)
		}
	})

	t.Run("concurrent rotation has one winner", func(t *testing.T) {
		s := newStore(t)
		_ = s.CreateAccount(ctx, account(1, "a@x.io", "ada", 1), nil)
		_ = s.InsertRefreshToken(ctx, &identity.RefreshToken{Hash: hash("c0"), FamilyID: 9, AccountID: 1, IssuedAt: base, ExpiresAt: base.Add(time.Hour)})
		var wg sync.WaitGroup
		results := make(chan error, 8)
		for i := 0; i < 8; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				_, err := s.RotateRefreshToken(ctx, hash("c0"), &identity.RefreshToken{Hash: hash(fmt.Sprint("n", i)),
					IssuedAt: base, ExpiresAt: base.Add(time.Hour)}, base, nil)
				results <- err
			}(i)
		}
		wg.Wait()
		close(results)
		wins := 0
		for err := range results {
			if err == nil {
				wins++
			} else if !errors.Is(err, identity.ErrTokenReused) && !errors.Is(err, identity.ErrTokenInvalid) {
				t.Errorf("unexpected: %v", err)
			}
		}
		if wins != 1 {
			t.Fatalf("%d rotations won", wins)
		}
	})

	t.Run("family branches and liveness", func(t *testing.T) {
		s := newStore(t)
		_ = s.CreateAccount(ctx, account(1, "a@x.io", "ada", 1), nil)
		_ = s.CreateAccount(ctx, account(2, "b@x.io", "bob", 1), nil)
		_ = s.InsertRefreshToken(ctx, &identity.RefreshToken{Hash: hash("f0"), FamilyID: 40, AccountID: 1, IssuedAt: base, ExpiresAt: base.Add(time.Hour)})
		if acct, err := s.ActiveFamily(ctx, 40, base); err != nil || acct != 1 {
			t.Fatalf("active family: %d %v", acct, err)
		}
		if _, err := s.ActiveFamily(ctx, 41, base); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("unknown family: %v", err)
		}
		if _, err := s.ActiveFamily(ctx, 40, base.Add(2*time.Hour)); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("expired family: %v", err)
		}
		branch := &identity.RefreshToken{Hash: hash("g0"), FamilyID: 40, AccountID: 1, IssuedAt: base, ExpiresAt: base.Add(time.Hour)}
		if err := s.ExtendFamily(ctx, branch, base); err != nil {
			t.Fatal(err)
		}
		if err := s.ExtendFamily(ctx, &identity.RefreshToken{Hash: hash("x0"), FamilyID: 40, AccountID: 2, IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("another account's family: %v", err)
		}
		// Both branches rotate independently; revoking through either ends both.
		if _, err := s.RotateRefreshToken(ctx, hash("f0"), &identity.RefreshToken{Hash: hash("f1"), IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base, nil); err != nil {
			t.Fatal(err)
		}
		if _, err := s.RotateRefreshToken(ctx, hash("g0"), &identity.RefreshToken{Hash: hash("g1"), IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base, nil); err != nil {
			t.Fatal(err)
		}
		if _, err := s.RevokeFamilyOf(ctx, hash("f1"), base, nil); err != nil {
			t.Fatal(err)
		}
		if _, err := s.RotateRefreshToken(ctx, hash("g1"), &identity.RefreshToken{Hash: hash("g2"), IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base, nil); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("branch survived logout: %v", err)
		}
		if err := s.ExtendFamily(ctx, &identity.RefreshToken{Hash: hash("g9"), FamilyID: 40, AccountID: 1, IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("revoked family extended: %v", err)
		}
		// A banned account's family cannot grow.
		_ = s.InsertRefreshToken(ctx, &identity.RefreshToken{Hash: hash("h0"), FamilyID: 50, AccountID: 2, IssuedAt: base, ExpiresAt: base.Add(time.Hour)})
		until := base.Add(time.Hour)
		_ = s.SetBan(ctx, 2, &until, "x", base, nil)
		_ = s.SetBan(ctx, 2, nil, "", base, nil) // unban: the ban revoked the tokens anyway
		_ = s.InsertRefreshToken(ctx, &identity.RefreshToken{Hash: hash("h1"), FamilyID: 51, AccountID: 2, IssuedAt: base, ExpiresAt: base.Add(time.Hour)})
		_ = s.SetBan(ctx, 2, &until, "x", base, nil)
		if err := s.ExtendFamily(ctx, &identity.RefreshToken{Hash: hash("h2"), FamilyID: 51, AccountID: 2, IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("banned family extended: %v", err)
		}
	})

	t.Run("revocation racing rotation leaves nothing live", func(t *testing.T) {
		// Regression: revoking a family through one token (logout with the used predecessor, or
		// reuse detection replaying it) while another token of the family rotated let the
		// rotation's successor survive the revocation. Each round races both kinds across
		// several families at once, then requires every family to be dead.
		s := newStore(t)
		_ = s.CreateAccount(ctx, account(1, "a@x.io", "ada", 1), nil)
		const families = 8
		for round := 0; round < 25; round++ {
			type fam struct {
				id     int64
				h0, h1 []byte
			}
			fs := make([]fam, families)
			for i := range fs {
				f := fam{id: int64(10000 + round*families + i), h0: hash(fmt.Sprint(round, "/", i, "/0")), h1: hash(fmt.Sprint(round, "/", i, "/1"))}
				_ = s.InsertRefreshToken(ctx, &identity.RefreshToken{Hash: f.h0, FamilyID: f.id, AccountID: 1, IssuedAt: base,
					ExpiresAt: base.Add(time.Hour)})
				if _, err := s.RotateRefreshToken(ctx, f.h0, &identity.RefreshToken{Hash: f.h1, IssuedAt: base,
					ExpiresAt: base.Add(time.Hour)}, base, nil); err != nil {
					t.Fatal(err)
				}
				fs[i] = f
			}
			start := make(chan struct{})
			var wg sync.WaitGroup
			for i, f := range fs {
				wg.Add(2)
				go func() {
					defer wg.Done()
					<-start
					_, _ = s.RotateRefreshToken(ctx, f.h1, &identity.RefreshToken{Hash: hash(fmt.Sprint(round, "/", i, "/2")),
						IssuedAt: base, ExpiresAt: base.Add(time.Hour)}, base, nil)
				}()
				go func() {
					defer wg.Done()
					<-start
					if i%2 == 0 { // logout presenting the used predecessor
						if _, err := s.RevokeFamilyOf(ctx, f.h0, base, nil); err != nil {
							t.Error(err)
						}
					} else { // a thief replays the used predecessor: reuse detection
						_, _ = s.RotateRefreshToken(ctx, f.h0, &identity.RefreshToken{Hash: hash(fmt.Sprint(round, "/", i, "/x")),
							IssuedAt: base, ExpiresAt: base.Add(time.Hour)}, base, nil)
					}
				}()
			}
			close(start)
			wg.Wait()
			for _, f := range fs {
				if _, err := s.ActiveFamily(ctx, f.id, base); !errors.Is(err, identity.ErrTokenInvalid) {
					t.Fatalf("round %d: family %d still has a live token after revocation", round, f.id)
				}
			}
		}
	})

	t.Run("ban revokes tokens", func(t *testing.T) {
		s := newStore(t)
		_ = s.CreateAccount(ctx, account(1, "a@x.io", "ada", 1), nil)
		_ = s.InsertRefreshToken(ctx, &identity.RefreshToken{Hash: hash("b0"), FamilyID: 1, AccountID: 1, IssuedAt: base, ExpiresAt: base.Add(time.Hour)})
		until := base.Add(24 * time.Hour)
		if err := s.SetBan(ctx, 1, &until, "botting", base, identity.NewAudit(base, 7, 1, identity.ActionBan, "", nil)); err != nil {
			t.Fatal(err)
		}
		a, _ := s.AccountByID(ctx, 1)
		if a.BannedUntil == nil || !a.BannedUntil.Equal(until) || a.BanReason != "botting" || !a.BannedAt(base) || a.BannedAt(until) {
			t.Fatalf("ban: %+v", a)
		}
		if _, err := s.RotateRefreshToken(ctx, hash("b0"), &identity.RefreshToken{Hash: hash("b1"), IssuedAt: base,
			ExpiresAt: base.Add(time.Hour)}, base, nil); !errors.Is(err, identity.ErrTokenInvalid) {
			t.Fatalf("token survived ban: %v", err)
		}
		if err := s.SetBan(ctx, 1, nil, "", base, nil); err != nil {
			t.Fatal(err)
		}
		a, _ = s.AccountByID(ctx, 1)
		if a.BannedUntil != nil || a.BanReason != "" {
			t.Fatalf("unban: %+v", a)
		}
		if err := s.SetBan(ctx, 42, &until, "x", base, nil); !errors.Is(err, identity.ErrNotFound) {
			t.Fatalf("ban missing: %v", err)
		}
	})

	t.Run("audit chain", func(t *testing.T) {
		s := newStore(t)
		for i := 0; i < 25; i++ {
			e := identity.NewAudit(base.Add(time.Duration(i)*time.Second), int64(i), int64(i+1), "test.action", "10.0.0.1",
				map[string]any{"i": i, "z": "last", "a": "first"})
			if err := s.AppendAudit(ctx, e); err != nil {
				t.Fatal(err)
			}
			if e.Seq != int64(i+1) {
				t.Fatalf("seq %d at %d", e.Seq, i)
			}
		}
		all, err := s.ListAudit(ctx, 0, 0)
		if err != nil || len(all) != 25 {
			t.Fatalf("list: %d %v", len(all), err)
		}
		if err := identity.VerifyAuditChain([32]byte{}, all); err != nil {
			t.Fatal(err)
		}
		if all[0].Detail != `{"a":"first","i":0,"z":"last"}` {
			t.Fatalf("detail not canonical: %s", all[0].Detail)
		}
		page, _ := s.ListAudit(ctx, 10, 5)
		if len(page) != 5 || page[0].Seq != 11 {
			t.Fatalf("paging: %+v", page)
		}
		if err := identity.VerifyAuditChain(all[9].Hash, page); err != nil {
			t.Fatalf("page chains from its predecessor: %v", err)
		}
		// Concurrent appends still form one chain.
		var wg sync.WaitGroup
		for i := 0; i < 10; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				if err := s.AppendAudit(ctx, identity.NewAudit(base, 0, 0, "concurrent", "", map[string]any{"i": i})); err != nil {
					t.Error(err)
				}
			}(i)
		}
		wg.Wait()
		all, _ = s.ListAudit(ctx, 0, 0)
		if len(all) != 35 {
			t.Fatalf("after concurrent appends: %d", len(all))
		}
		if err := identity.VerifyAuditChain([32]byte{}, all); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(all[34].PrevHash[:], all[33].Hash[:]) {
			t.Fatal("tail link")
		}
	})

	t.Run("ping", func(t *testing.T) {
		if err := newStore(t).Ping(ctx); err != nil {
			t.Fatal(err)
		}
	})
}
