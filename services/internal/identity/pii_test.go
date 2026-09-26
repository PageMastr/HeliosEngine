package identity_test

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/internal/identity"
	"github.com/PageMastr/scifi-test/services/internal/platform"
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
	return f.serviceOver(t, f.store, keys)
}

// serviceOver builds another identity service over store with keys, sharing f's clock, cache
// and configuration.
func (f *fixture) serviceOver(t *testing.T, store identity.Store, keys *identity.PIIKeys) *identity.Service {
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
	svc, err := identity.New(identity.Deps{Config: f.cfg, Store: store, PII: keys, Hasher: fastHasher(), Issuer: issuer,
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

	// With another pepper the index differs, so the e-mail lookup finds nothing; with another
	// KEK nothing can be decrypted, nor sealed for this account: issuing tokens (which seal the
	// client IP) fails closed rather than storing an IP in the clear.
	other := f.sibling(t, newPIIKeys(t))
	if _, err := other.Login(ctx, meta, &identity.LoginRequest{Login: email, Password: pw}); rpc.CodeOf(err) != rpc.CodeUnauthenticated {
		t.Fatalf("lookup must depend on the pepper: %v", err)
	}
	if _, err := other.Login(ctx, meta, &identity.LoginRequest{Login: reg.Tag, Password: pw}); rpc.CodeOf(err) != rpc.CodeInternal {
		t.Fatalf("another KEK must not seal for this account: %v", err)
	}
	if _, err := other.GetAccount(ctx, p); rpc.CodeOf(err) != rpc.CodeInternal {
		t.Fatalf("another KEK must not decrypt the address, nor pass it off as shredded: %v", err)
	}
	if _, err := newPIIKeys(t).DecryptEmail(acct, key); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("wrong KEK: %v", err)
	}
	// DecryptEmail refuses another account's subject key before any cryptography (the row
	// binding itself is TestSealedEmailAndDEKAreBoundToTheirAccount's).
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
	// The chained audit row holds only pseudonymous IDs (05 §1.17): no address, no digest of it.
	sum := sha256.Sum256([]byte(ghost))
	keyed := hex.EncodeToString(f.pii.EmailIndex(ghost)[:12])
	if d := entries[0].Detail; d != `{"reason":"unknown_login"}` || entries[0].Subject != 0 {
		t.Fatalf("unknown-login audit row: %+v", entries[0])
	}
	// Only the TTL'd rate-limit bucket names the login, by its keyed index.
	keys := strings.Join(f.redis.Mini.Keys(), " ")
	if !strings.Contains(keys, "login:acct:"+keyed) || strings.Contains(keys, hex.EncodeToString(sum[:12])) || strings.Contains(keys, ghost) {
		t.Fatalf("rate-limit keys: %v", keys)
	}
}

// Blocking finding 5 of PR #8's first review: the row binding comes from the AAD, not from an
// ID comparison, so a database writer cannot move one account's ciphertext or wrapped DEK onto
// another account's rows.
func TestSealedEmailAndDEKAreBoundToTheirAccount(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	a := f.register(t, "a@example.com", "Ada", "analytical engine")
	b := f.register(t, "b@example.com", "Bob", "difference engine")
	acctA, _ := f.store.AccountByID(ctx, a.AccountID)
	keyA, _ := f.store.SubjectKey(ctx, a.AccountID)
	moved := &identity.SubjectKey{AccountID: b.AccountID, WrappedDEK: keyA.WrappedDEK, KEKVersion: keyA.KEKVersion}
	if _, err := f.pii.UnwrapSubjectKey(moved); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("A's wrapped DEK unwraps on B's subject_key row: %v", err)
	}
	dekA, err := f.pii.UnwrapSubjectKey(keyA)
	if err != nil {
		t.Fatal(err)
	}
	defer dekA.Clear()
	if _, err := pii.Open(&dekA, identity.EmailAAD(b.AccountID), acctA.EmailCT); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("A's email_ct opens on B's account row: %v", err)
	}
	if got, err := pii.Open(&dekA, identity.EmailAAD(a.AccountID), acctA.EmailCT); err != nil || string(got) != "a@example.com" {
		t.Fatalf("A's own row: %q %v", got, err)
	}
}

// shredded is a store whose subject keys have all been deleted (erasure, Phase 3).
type shredded struct{ identity.Store }

func (s shredded) SubjectKey(ctx context.Context, id int64) (*identity.SubjectKey, error) {
	k, err := s.Store.SubjectKey(ctx, id)
	if err != nil {
		return nil, err
	}
	return &identity.SubjectKey{AccountID: k.AccountID, KEKVersion: k.KEKVersion}, nil
}

func TestGetAccountAfterShreddingHasNoEmail(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	reg := f.register(t, "gone@example.com", "Gone", "analytical engine")
	svc := f.serviceOver(t, shredded{f.store}, f.pii)
	pair, err := svc.Login(ctx, meta, &identity.LoginRequest{Login: reg.Tag, Password: "analytical engine"})
	if err != nil {
		t.Fatalf("login with a shredded key (no IP is stored): %v", err)
	}
	p, _ := svc.Verifier().Verify(pair.AccessToken)
	if info, err := svc.GetAccount(ctx, p); err != nil || info.Email != "" || info.AccountID != reg.AccountID {
		t.Fatalf("shredded account: %+v %v", info, err)
	}
}

// Client IPs are direct PII (05 §6.6): refresh tokens and the login history keep them sealed
// under the account's DEK and bound to their row; audit rows and unknown logins keep none.
func TestClientIPsAreSealedPerAccount(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	reg := f.register(t, "ip@example.com", "Ipsy", "analytical engine")
	pair, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: reg.Tag, Password: "analytical engine"})
	if err != nil {
		t.Fatal(err)
	}
	_, _ = f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "ghost@example.com", Password: "x"})
	key, _ := f.store.SubjectKey(ctx, reg.AccountID)

	// Refresh token: sealed for its own row.
	h := sha256.Sum256([]byte(pair.RefreshToken))
	tok, err := f.store.RevokeFamilyOf(ctx, h[:], f.clk.Now(), nil)
	if err != nil || tok.ClientIPCT == nil || bytes.Contains(tok.ClientIPCT, []byte(meta.ClientIP)) {
		t.Fatalf("refresh token IP: %+v %v", tok, err)
	}
	if ip, err := f.pii.Open(key, identity.RefreshIPAAD(h[:]), tok.ClientIPCT); err != nil || ip != meta.ClientIP {
		t.Fatalf("refresh token IP: %q %v", ip, err)
	}
	if _, err := f.pii.Open(key, identity.RefreshIPAAD([]byte("another token")), tok.ClientIPCT); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("refresh token IP must be bound to its row: %v", err)
	}

	// Login history: register and login, each sealed for its own row.
	hist, err := f.store.LoginHistory(ctx, reg.AccountID)
	if err != nil || len(hist) != 2 || hist[0].Action != identity.ActionRegister || hist[1].Action != identity.ActionLogin {
		t.Fatalf("history: %+v %v", hist, err)
	}
	for _, e := range hist {
		if ip, err := f.pii.Open(key, identity.LoginIPAAD(e.ID), e.ClientIPCT); err != nil || ip != meta.ClientIP {
			t.Fatalf("history row %d: %q %v", e.ID, ip, err)
		}
	}
	if _, err := f.pii.Open(key, identity.LoginIPAAD(hist[1].ID), hist[0].ClientIPCT); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("history IP must be bound to its row: %v", err)
	}
	if h0, _ := f.store.LoginHistory(ctx, 0); len(h0) != 0 {
		t.Fatalf("an unknown login stored an IP: %+v", h0)
	}

	// Retention: 90 days, then the job deletes the history and clears the token's IP.
	f.clk.Advance(identity.LoginHistoryRetention + time.Hour)
	if n, err := f.svc.PurgeLoginHistory(ctx); err != nil || n != 3 {
		t.Fatalf("purge: %d %v (want 2 history rows and 1 token IP)", n, err)
	}
	if hist, _ := f.store.LoginHistory(ctx, reg.AccountID); len(hist) != 0 {
		t.Fatalf("after retention: %+v", hist)
	}
	if tok, err := f.store.RevokeFamilyOf(ctx, h[:], f.clk.Now(), nil); err != nil || tok.ClientIPCT != nil {
		t.Fatalf("refresh token IP after retention: %+v %v", tok, err)
	}
}

// The retention job keeps what is younger than 90 days, and Start runs it (05 §6.6).
func TestLoginHistoryRetentionKeepsRecentRows(t *testing.T) {
	if identity.LoginHistoryRetention != 90*24*time.Hour {
		t.Fatalf("05 §6.6 keeps login and IP history 90 days, not %s", identity.LoginHistoryRetention)
	}
	f := newFixture(t)
	ctx := context.Background()
	reg := f.register(t, "keep@example.com", "Keep", "analytical engine")
	f.clk.Advance(identity.LoginHistoryRetention - time.Hour)
	if n, err := f.svc.PurgeLoginHistory(ctx); err != nil || n != 0 {
		t.Fatalf("purged %d rows younger than the retention: %v", n, err)
	}
	f.clk.Advance(2 * time.Hour)
	if err := f.svc.Start(ctx); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for h, _ := f.store.LoginHistory(ctx, reg.AccountID); len(h) != 0; h, _ = f.store.LoginHistory(ctx, reg.AccountID) {
		if time.Now().After(deadline) {
			t.Fatalf("Start did not run the retention job: %+v", h)
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err := f.svc.Stop(ctx); err != nil {
		t.Fatal(err)
	}
}

// A ban's reason is GM free text (05 §1.17, §6.6): it is sealed under the account's DEK, bound
// to the account row, and kept off the hash-chained audit row.
func TestBanReasonIsSealedAndKeptOffTheChain(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	reg := f.register(t, "ban@example.com", "Banned", "analytical engine")
	other := f.register(t, "other@example.com", "Other", "difference engine")
	until := f.clk.Now().Add(24 * time.Hour)
	const reason = "gold selling to Zed Quillon"
	if err := f.svc.Ban(ctx, 7, reg.AccountID, &until, reason); err != nil {
		t.Fatal(err)
	}
	acct, _ := f.store.AccountByID(ctx, reg.AccountID)
	key, _ := f.store.SubjectKey(ctx, reg.AccountID)
	if acct.BanReasonCT == nil || bytes.Contains(acct.BanReasonCT, []byte("Quillon")) {
		t.Fatalf("ban_reason_ct is not ciphertext: %q", acct.BanReasonCT)
	}
	if got, err := f.pii.Open(key, identity.BanReasonAAD(reg.AccountID), acct.BanReasonCT); err != nil || got != reason {
		t.Fatalf("ban reason: %q %v", got, err)
	}
	if _, err := f.pii.Open(key, identity.BanReasonAAD(other.AccountID), acct.BanReasonCT); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("ban reason must be bound to its row: %v", err)
	}
	if _, err := f.pii.Open(key, identity.EmailAAD(reg.AccountID), acct.BanReasonCT); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("ban reason must be bound to its column: %v", err)
	}
	entries, err := f.store.ListAudit(ctx, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	var bans int
	for _, e := range entries {
		if e.Action == identity.ActionBan {
			bans++
			if strings.Contains(e.Detail, "reason") || strings.Contains(e.Detail, "Quillon") || e.Subject != reg.AccountID {
				t.Fatalf("the chained ban row carries the GM text: %+v", e)
			}
		}
	}
	if bans != 1 {
		t.Fatalf("%d ban rows", bans)
	}
	long := strings.Repeat("x", 1025)
	if err := f.svc.Ban(ctx, 7, reg.AccountID, &until, long); rpc.CodeOf(err) != rpc.CodeInvalidArgument {
		t.Fatalf("an unbounded reason was accepted: %v", err)
	}
	if err := f.svc.Ban(ctx, 7, reg.AccountID, nil, ""); err != nil {
		t.Fatal(err)
	}
	if acct, _ := f.store.AccountByID(ctx, reg.AccountID); acct.BanReasonCT != nil {
		t.Fatalf("unban kept the reason: %+v", acct)
	}
}

// Row format 2 is tagged, so no legacy row (with an IP) hashes like a new one: without the tag a
// v1 row with (ip X, detail Y) would encode exactly like a v2 row with (detail X, note_digest Y).
func TestAuditRowFormatsNeverCollide(t *testing.T) {
	var prev [32]byte
	v1 := identity.AuditEntry{Seq: 3, At: time.Date(2026, 9, 1, 0, 0, 0, 0, time.UTC), Actor: 1, Subject: 2,
		Action: identity.ActionLogin, Detail: "Y"}
	v2 := v1
	v2.Detail, v2.NoteDigest = "X", []byte("Y")
	if identity.LegacyChainHashV1(prev, &v1, "X") == identity.ChainHash(prev, &v2) {
		t.Fatal("a row-format-1 row hashes like a crossed row-format-2 row")
	}
}

// Every event with a subject records its client IP in the login history, sealed for its row, and
// a refreshed token carries its own sealed IP (05 §6.6).
func TestEveryIPEventIsSealedAndRecorded(t *testing.T) {
	f := newFixture(t, func(c *platform.IdentityConfig) { c.LoginPerAccount = platform.RateLimit{} })
	ctx := context.Background()
	from := func(ip string) identity.Meta { return identity.Meta{ClientIP: ip, UserAgent: "test"} }
	const pw = "analytical engine"
	reg, err := f.svc.Register(ctx, from("192.0.2.1"), &identity.RegisterRequest{Email: "all@example.com", Handle: "All", Password: pw})
	if err != nil {
		t.Fatal(err)
	}
	login := func(ip string) *identity.TokenPair {
		t.Helper()
		pair, err := f.svc.Login(ctx, from(ip), &identity.LoginRequest{Login: reg.Tag, Password: pw})
		if err != nil {
			t.Fatal(err)
		}
		return pair
	}
	if _, err := f.svc.Login(ctx, from("192.0.2.2"), &identity.LoginRequest{Login: reg.Tag, Password: "wrong"}); err == nil {
		t.Fatal("bad password accepted")
	}
	first := login("192.0.2.3")
	second, err := f.svc.Refresh(ctx, from("192.0.2.4"), &identity.RefreshRequest{RefreshToken: first.RefreshToken})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.svc.Refresh(ctx, from("192.0.2.5"), &identity.RefreshRequest{RefreshToken: first.RefreshToken}); err == nil {
		t.Fatal("reuse accepted")
	}
	out := login("192.0.2.6")
	if _, err := f.svc.Logout(ctx, from("192.0.2.7"), &identity.LogoutRequest{RefreshToken: out.RefreshToken}); err != nil {
		t.Fatal(err)
	}
	launcher := login("192.0.2.8")
	p, err := f.svc.Verifier().Verify(launcher.AccessToken)
	if err != nil {
		t.Fatal(err)
	}
	code, err := f.svc.CreateLaunchCode(ctx, p)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.svc.ExchangeLaunchCode(ctx, from("192.0.2.9"), &identity.ExchangeLaunchCodeRequest{Code: code.Code}); err != nil {
		t.Fatal(err)
	}
	until := f.clk.Now().Add(time.Hour)
	if err := f.svc.Ban(ctx, 7, reg.AccountID, &until, "test"); err != nil {
		t.Fatal(err)
	}
	if _, err := f.svc.Login(ctx, from("192.0.2.10"), &identity.LoginRequest{Login: reg.Tag, Password: pw}); err == nil {
		t.Fatal("banned login accepted")
	}

	key, _ := f.store.SubjectKey(ctx, reg.AccountID)
	hist, err := f.store.LoginHistory(ctx, reg.AccountID)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, e := range hist {
		ip, err := f.pii.Open(key, identity.LoginIPAAD(e.ID), e.ClientIPCT)
		if err != nil {
			t.Fatalf("history row %d: %v", e.ID, err)
		}
		got = append(got, e.Action+" "+ip)
	}
	slices.Sort(got)
	want := []string{identity.ActionRegister + " 192.0.2.1", identity.ActionLoginFailed + " 192.0.2.2",
		identity.ActionLogin + " 192.0.2.3", identity.ActionRefreshReuse + " 192.0.2.5", identity.ActionLogin + " 192.0.2.6",
		identity.ActionLogout + " 192.0.2.7", identity.ActionLogin + " 192.0.2.8", identity.ActionLaunchCode + " 192.0.2.9",
		identity.ActionLoginDenied + " 192.0.2.10"}
	slices.Sort(want)
	if !slices.Equal(got, want) {
		t.Fatalf("login history:\n got %v\nwant %v", got, want)
	}
	// The refreshed token carries the IP it was refreshed from, sealed for its own row.
	h := sha256.Sum256([]byte(second.RefreshToken))
	tok, err := f.store.RevokeFamilyOf(ctx, h[:], f.clk.Now(), nil)
	if err != nil {
		t.Fatal(err)
	}
	if ip, err := f.pii.Open(key, identity.RefreshIPAAD(h[:]), tok.ClientIPCT); err != nil || ip != "192.0.2.4" {
		t.Fatalf("refreshed token IP: %q %v", ip, err)
	}
}
