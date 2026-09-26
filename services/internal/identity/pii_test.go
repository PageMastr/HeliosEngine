package identity_test

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"strings"
	"testing"

	"github.com/PageMastr/scifi-test/services/internal/identity"
	"github.com/PageMastr/scifi-test/services/pkg/authn"
	"github.com/PageMastr/scifi-test/services/pkg/idgen"
	"github.com/PageMastr/scifi-test/services/pkg/pii"
	"github.com/PageMastr/scifi-test/services/pkg/ratelimit"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// sibling is another identity replica over the same store, but with its own PII keys (another
// environment's key files, or keys lost with a data directory).
func (f *fixture) sibling(t *testing.T, keys *identity.PIIKeys) *identity.Service {
	t.Helper()
	key, err := authn.GenerateSigningKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	issuer := authn.NewIssuer(key, f.cfg.Issuer, f.cfg.Audience, f.cfg.AccessTTL.D(), f.clk)
	ids, err := idgen.NewMinter(0, idgen.NewMemSource(f.clk), idgen.Options{Clock: f.clk, Log: quiet})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(ids.Close)
	svc, err := identity.New(identity.Deps{Config: f.cfg, Store: f.store, PII: keys, Hasher: fastHasher(), Issuer: issuer,
		Keys: authn.NewKeySet(issuer.PublicKey()), Limiter: ratelimit.New(f.redis.Client, f.clk, "rl-sibling:", quiet),
		Cache: f.redis.Client, IDs: ids, Clock: f.clk, Log: quiet})
	if err != nil {
		t.Fatal(err)
	}
	return svc
}

func TestEmailIsStoredEncryptedAndFoundByBlindIndex(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	const email, pw = "Ada.Lovelace@Example.com", "analytical engine"
	reg := f.register(t, "  "+email+" ", "Ada", pw)

	// The store holds ciphertext and a keyed index, never the address (05 §6.6).
	acct, err := f.store.AccountByID(ctx, reg.AccountID)
	if err != nil {
		t.Fatal(err)
	}
	lower := strings.ToLower(email)
	if bytes.Contains(bytes.ToLower(acct.EmailCT), []byte("lovelace")) || len(acct.EmailCT) != len(email)+pii.Overhead {
		t.Fatalf("email_ct is not ciphertext of the trimmed address: %q", acct.EmailCT)
	}
	plain := sha256.Sum256([]byte(lower))
	if !bytes.Equal(acct.EmailBidx, f.pii.EmailIndex(email)) || bytes.Equal(acct.EmailBidx, plain[:]) {
		t.Fatalf("email_bidx %x is not the keyed index", acct.EmailBidx)
	}
	key, err := f.store.SubjectKey(ctx, reg.AccountID)
	if err != nil || key.KEKVersion != 1 || len(key.WrappedDEK) != pii.DEKBytes+pii.Overhead {
		t.Fatalf("subject key: %+v %v", key, err)
	}
	if got, err := f.pii.DecryptEmail(acct, key); err != nil || got != email {
		t.Fatalf("decrypt: %q %v", got, err)
	}

	// Login finds the account through the blind index, whatever the case and spacing.
	pair, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: " " + strings.ToUpper(lower), Password: pw})
	if err != nil || pair.AccountID != reg.AccountID {
		t.Fatalf("login by email: %+v %v", pair, err)
	}
	p, _ := f.svc.Verifier().Verify(pair.AccessToken)
	if info, err := f.svc.GetAccount(ctx, p); err != nil || info.Email != email {
		t.Fatalf("GetAccount decrypts the address as entered: %+v %v", info, err)
	}
	if _, err := f.svc.Register(ctx, meta, &identity.RegisterRequest{Email: lower, Handle: "Other", Password: "different enough"}); rpc.CodeOf(err) != rpc.CodeAlreadyExists {
		t.Fatalf("the blind index must keep addresses unique: %v", err)
	}

	// With another pepper the index differs, so the e-mail lookup finds nothing (the tag still
	// works); with another KEK the address cannot be decrypted.
	other := f.sibling(t, newPIIKeys(t))
	if _, err := other.Login(ctx, meta, &identity.LoginRequest{Login: email, Password: pw}); rpc.CodeOf(err) != rpc.CodeUnauthenticated {
		t.Fatalf("lookup must depend on the pepper: %v", err)
	}
	pair, err = other.Login(ctx, meta, &identity.LoginRequest{Login: reg.Tag, Password: pw})
	if err != nil {
		t.Fatalf("login by tag: %v", err)
	}
	p, _ = other.Verifier().Verify(pair.AccessToken)
	if _, err := other.GetAccount(ctx, p); rpc.CodeOf(err) != rpc.CodeInternal {
		t.Fatalf("another KEK must not decrypt the address: %v", err)
	}
	if _, err := newPIIKeys(t).DecryptEmail(acct, key); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("wrong KEK: %v", err)
	}
	// A subject key belongs to one account.
	if _, err := f.pii.DecryptEmail(&identity.Account{ID: acct.ID + 1, EmailCT: acct.EmailCT}, key); err == nil {
		t.Fatal("another account's subject key accepted")
	}
	if _, err := f.pii.DecryptEmail(acct, &identity.SubjectKey{AccountID: acct.ID}); !errors.Is(err, identity.ErrShredded) {
		t.Fatalf("shredded key: %v", err)
	}
}

func TestUnknownEmailLoginsLeaveNoUnkeyedHash(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	const ghost = "ghost@example.com"
	if _, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: ghost, Password: "x"}); rpc.CodeOf(err) != rpc.CodeUnauthenticated {
		t.Fatal(err)
	}
	entries, err := f.store.ListAudit(ctx, 0, 0)
	if err != nil || len(entries) != 1 || entries[0].Action != identity.ActionLoginFailed {
		t.Fatalf("audit: %+v %v", entries, err)
	}
	sum := sha256.Sum256([]byte(ghost))
	keyed := hex.EncodeToString(f.pii.EmailIndex(ghost)[:12])
	if d := entries[0].Detail; strings.Contains(d, ghost) || strings.Contains(d, hex.EncodeToString(sum[:12])) || !strings.Contains(d, keyed) {
		t.Fatalf("an unknown e-mail login must be named by its keyed index only: %s", d)
	}
	if keys := f.redis.Mini.Keys(); len(keys) == 0 || strings.Contains(strings.Join(keys, " "), hex.EncodeToString(sum[:12])) ||
		strings.Contains(strings.Join(keys, " "), ghost) {
		t.Fatalf("rate-limit keys: %v", keys)
	}
}
