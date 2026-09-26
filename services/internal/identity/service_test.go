package identity_test

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus"

	"github.com/PageMastr/scifi-test/services/internal/identity"
	"github.com/PageMastr/scifi-test/services/internal/identity/storetest"
	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/authn"
	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/idgen"
	"github.com/PageMastr/scifi-test/services/pkg/keyring"
	"github.com/PageMastr/scifi-test/services/pkg/ratelimit"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
	"github.com/PageMastr/scifi-test/services/pkg/testkit"
)

func TestMemStoreConformance(t *testing.T) {
	storetest.Run(t, func(*testing.T) identity.Store { return identity.NewMemStore() })
}

type fixture struct {
	svc    *identity.Service
	store  *identity.MemStore
	pii    *identity.PIIKeys
	clk    *clock.Fake
	redis  *testkit.Redis
	reg    *prometheus.Registry
	cfg    platform.IdentityConfig
	banned []int64 // OnBan calls
}

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

func fastHasher() *identity.Hasher {
	return identity.NewHasher(identity.HashParams{MemoryKiB: 64, Iterations: 1, Parallelism: 1}, 4)
}

func newFixture(t *testing.T, mutate ...func(*platform.IdentityConfig)) *fixture {
	t.Helper()
	cfg := platform.Default().Identity
	for _, m := range mutate {
		m(&cfg)
	}
	fc := clock.NewFake(time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC))
	r := testkit.StartRedis(t)
	store := identity.NewMemStore()
	key, err := authn.GenerateSigningKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	issuer := authn.NewIssuer(key, cfg.Issuer, cfg.Audience, cfg.AccessTTL.D(), fc)
	ids, err := idgen.NewMinter(0, idgen.NewMemSource(fc), idgen.Options{Clock: fc, Log: quiet})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(ids.Close)
	reg := prometheus.NewRegistry()
	f := &fixture{store: store, pii: newPIIKeys(t), clk: fc, redis: r, reg: reg, cfg: cfg}
	svc, err := identity.New(identity.Deps{
		Config: cfg, Store: store, PII: f.pii, Hasher: fastHasher(), Issuer: issuer, Keys: authn.NewKeySet(issuer.PublicKey()),
		Limiter: ratelimit.New(r.Client, fc, "rl:", quiet), Cache: r.Client, IDs: ids, Clock: fc, Log: quiet, Metrics: reg,
		OnBan: func(_ context.Context, account int64) { f.banned = append(f.banned, account) },
	})
	if err != nil {
		t.Fatal(err)
	}
	f.svc = svc
	return f
}

// newPIIKeys returns a fresh KEK and blind-index pepper, as a new dev data directory has.
func newPIIKeys(t *testing.T) *identity.PIIKeys {
	t.Helper()
	kek, err := keyring.New("subject-kek", nil, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	pepper, err := keyring.New("email-bidx-pepper", nil, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	k, err := identity.NewPIIKeys(kek, pepper)
	if err != nil {
		t.Fatal(err)
	}
	return k
}

var meta = identity.Meta{ClientIP: "203.0.113.9", UserAgent: "test"}

func wantCode(t *testing.T, err error, code rpc.Code) {
	t.Helper()
	if rpc.CodeOf(err) != code {
		t.Fatalf("want %s, got %v", code, err)
	}
}

func (f *fixture) register(t *testing.T, email, handle, pw string) *identity.RegisterResponse {
	t.Helper()
	res, err := f.svc.Register(context.Background(), meta, &identity.RegisterRequest{Email: email, Handle: handle, Password: pw})
	if err != nil {
		t.Fatalf("register %s: %v", email, err)
	}
	return res
}

func TestRegisterLoginVerify(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	reg := f.register(t, "Ada@Example.com", "Ada", "correct horse battery")
	if reg.Discriminator < 1 || reg.Discriminator > 9999 || !strings.HasPrefix(reg.Tag, "Ada#") || reg.AccountID <= 0 {
		t.Fatalf("register: %+v", reg)
	}

	// Login by (differently cased) email and by tag.
	for _, login := range []string{"ada@example.COM", reg.Tag, strings.ToLower(reg.Tag)} {
		pair, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: login, Password: "correct horse battery"})
		if err != nil {
			t.Fatalf("login %q: %v", login, err)
		}
		p, err := f.svc.Verifier().Verify(pair.AccessToken)
		if err != nil || p.AccountID != reg.AccountID || !p.HasScope(authn.ScopeGame) || p.Handle != reg.Tag {
			t.Fatalf("access token: %+v %v", p, err)
		}
		if !pair.AccessExpiresAt.Equal(f.clk.Now().Add(10*time.Minute)) || pair.TokenType != "Bearer" ||
			!strings.HasPrefix(pair.RefreshToken, "hrt1_") {
			t.Fatalf("pair: %+v", pair)
		}
		info, err := f.svc.GetAccount(ctx, p)
		if err != nil || info.Email != "Ada@Example.com" || info.LastLoginAt == nil {
			t.Fatalf("account: %+v %v", info, err)
		}
	}
	if got := f.counter(t, "helios_identity_logins_total", "ok"); got != 3 {
		t.Fatalf("login metric %v", got)
	}
	// Access tokens expire after 10 minutes.
	pair, _ := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: reg.Tag, Password: "correct horse battery"})
	f.clk.Advance(11 * time.Minute)
	if _, err := f.svc.Verifier().Verify(pair.AccessToken); err == nil {
		t.Fatal("expired access token accepted")
	}
	if n, err := f.svc.VerifyAudit(ctx); err != nil || n < 5 {
		t.Fatalf("audit chain: %d %v", n, err)
	}
}

// counter returns the value of counter `name` whose label equals labelValue.
func (f *fixture) counter(t *testing.T, name, labelValue string) float64 {
	t.Helper()
	mfs, err := f.reg.Gather()
	if err != nil {
		t.Fatal(err)
	}
	for _, mf := range mfs {
		if mf.GetName() != name {
			continue
		}
		for _, m := range mf.GetMetric() {
			for _, l := range m.GetLabel() {
				if l.GetValue() == labelValue {
					return m.GetCounter().GetValue()
				}
			}
		}
	}
	return 0
}

func TestRegisterValidation(t *testing.T) {
	f := newFixture(t, func(c *platform.IdentityConfig) { c.RegisterPerIP.Rate = 0 })
	ctx := context.Background()
	f.register(t, "taken@example.com", "Taken", "long enough pw")
	cases := []struct {
		req  identity.RegisterRequest
		code rpc.Code
	}{
		{identity.RegisterRequest{Email: "nope", Handle: "Valid", Password: "long enough pw"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "A <a@b.io>", Handle: "Valid", Password: "long enough pw"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "a@localhost", Handle: "Valid", Password: "long enough pw"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "a@b.io", Handle: "x", Password: "long enough pw"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "a@b.io", Handle: "9lives", Password: "long enough pw"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "a@b.io", Handle: "has space", Password: "long enough pw"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "a@b.io", Handle: "Admin", Password: "long enough pw"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "a@b.io", Handle: "Valid", Password: "short"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "a@b.io", Handle: "Validhandle", Password: "validhandle"}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "a@b.io", Handle: "Valid", Password: strings.Repeat("p", 2000)}, rpc.CodeInvalidArgument},
		{identity.RegisterRequest{Email: "TAKEN@example.com", Handle: "Other", Password: "long enough pw"}, rpc.CodeAlreadyExists},
	}
	for _, c := range cases {
		_, err := f.svc.Register(ctx, meta, &c.req)
		if rpc.CodeOf(err) != c.code {
			t.Errorf("%s/%s/%d: want %s got %v", c.req.Email, c.req.Handle, len(c.req.Password), c.code, err)
		}
	}
	limited := newFixture(t)
	var err error
	for i := 0; i < 6; i++ {
		_, err = limited.svc.Register(ctx, meta, &identity.RegisterRequest{Email: "bad"})
	}
	wantCode(t, err, rpc.CodeResourceExhausted) // registration is rate limited per IP (burst 5)

	closed := newFixture(t, func(c *platform.IdentityConfig) { c.AllowRegistration = false })
	_, err = closed.svc.Register(ctx, meta, &identity.RegisterRequest{Email: "a@b.io", Handle: "Valid", Password: "long enough pw"})
	wantCode(t, err, rpc.CodePermissionDenied)
}

func TestSameHandleGetsDistinctDiscriminators(t *testing.T) {
	f := newFixture(t, func(c *platform.IdentityConfig) { c.RegisterPerIP.Rate = 0 })
	seen := map[int]bool{}
	for i := 0; i < 20; i++ {
		r := f.register(t, strings.Repeat("x", i+1)+"@example.com", "Nova", "long enough pw")
		if seen[r.Discriminator] {
			t.Fatalf("discriminator %d reused", r.Discriminator)
		}
		seen[r.Discriminator] = true
	}
}

func TestLoginFailuresAndRateLimits(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	reg := f.register(t, "bob@example.com", "Bob", "hunter2hunter2")

	_, errUnknown := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "ghost@example.com", Password: "x"})
	_, errBad := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "bob@example.com", Password: "wrong"})
	wantCode(t, errUnknown, rpc.CodeUnauthenticated)
	wantCode(t, errBad, rpc.CodeUnauthenticated)
	if errUnknown.Error() != errBad.Error() {
		t.Fatal("unknown account and wrong password must be indistinguishable")
	}
	_, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "Bob#99999", Password: "x"})
	wantCode(t, err, rpc.CodeInvalidArgument)

	// Per-account limit: burst 5 (one attempt already spent above), then 6/min. The bucket is
	// the account, so switching between email and tag does not buy more guesses.
	for i := 0; i < 4; i++ {
		_, err := f.svc.Login(ctx, identity.Meta{ClientIP: "198.51.100." + strconv.Itoa(i+1)},
			&identity.LoginRequest{Login: "bob@example.com", Password: "wrong"})
		wantCode(t, err, rpc.CodeUnauthenticated)
	}
	_, err = f.svc.Login(ctx, identity.Meta{ClientIP: "192.0.2.77"}, &identity.LoginRequest{Login: reg.Tag, Password: "hunter2hunter2"})
	e := rpc.AsError(err)
	if e == nil || e.Code != rpc.CodeResourceExhausted || e.RetryAfter <= 0 {
		t.Fatalf("expected per-account rate limit via the tag, got %v", err)
	}
	f.clk.Advance(time.Minute)
	if _, err := f.svc.Login(ctx, identity.Meta{ClientIP: "192.0.2.77"}, &identity.LoginRequest{Login: "bob@example.com", Password: "hunter2hunter2"}); err != nil {
		t.Fatalf("after window: %v", err)
	}

	// Per-IP limit: burst 10 from one address across different accounts.
	g := newFixture(t)
	var last error
	for i := 0; i < 11; i++ {
		_, last = g.svc.Login(ctx, meta, &identity.LoginRequest{Login: strings.Repeat("u", i+1) + "@example.com", Password: "x"})
	}
	wantCode(t, last, rpc.CodeResourceExhausted)
}

func TestRefreshRotationReuseAndLogout(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	f.register(t, "c@example.com", "Cyd", "long enough pw")
	p0, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "c@example.com", Password: "long enough pw"})
	if err != nil {
		t.Fatal(err)
	}
	p1, err := f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: p0.RefreshToken})
	if err != nil || p1.RefreshToken == p0.RefreshToken || p1.AccountID != p0.AccountID {
		t.Fatalf("refresh: %+v %v", p1, err)
	}
	a0, _ := f.svc.Verifier().Verify(p0.AccessToken)
	a1, _ := f.svc.Verifier().Verify(p1.AccessToken)
	if a0.Family == "" || a0.Family != a1.Family {
		t.Fatal("rotation must stay in the family")
	}
	// Replaying p0 revokes the family: p1 dies too.
	_, err = f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: p0.RefreshToken})
	wantCode(t, err, rpc.CodeUnauthenticated)
	_, err = f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: p1.RefreshToken})
	wantCode(t, err, rpc.CodeUnauthenticated)
	_, err = f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: "garbage"})
	wantCode(t, err, rpc.CodeUnauthenticated)

	// Logout.
	p2, _ := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "c@example.com", Password: "long enough pw"})
	if _, err := f.svc.Logout(ctx, meta, &identity.LogoutRequest{RefreshToken: p2.RefreshToken}); err != nil {
		t.Fatal(err)
	}
	_, err = f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: p2.RefreshToken})
	wantCode(t, err, rpc.CodeUnauthenticated)
	if _, err := f.svc.Logout(ctx, meta, &identity.LogoutRequest{RefreshToken: "hrt1_unknown"}); err != nil {
		t.Fatalf("logout must be idempotent: %v", err)
	}
	// Expired refresh token.
	p3, _ := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "c@example.com", Password: "long enough pw"})
	f.clk.Advance(31 * 24 * time.Hour)
	_, err = f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: p3.RefreshToken})
	wantCode(t, err, rpc.CodeUnauthenticated)
	if n, err := f.svc.VerifyAudit(ctx); err != nil || n == 0 {
		t.Fatalf("audit %d %v", n, err)
	}
}

func TestBan(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	r := f.register(t, "d@example.com", "Dex", "long enough pw")
	pair, _ := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "d@example.com", Password: "long enough pw"})
	until := f.clk.Now().Add(48 * time.Hour)
	wantCode(t, f.svc.Ban(ctx, 1, r.AccountID, &until, ""), rpc.CodeInvalidArgument)
	if err := f.svc.Ban(ctx, 1, r.AccountID, &until, "gold selling"); err != nil {
		t.Fatal(err)
	}
	if len(f.banned) != 1 || f.banned[0] != r.AccountID {
		t.Fatalf("a ban must end the live game session at once (OnBan): %v", f.banned)
	}
	_, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "d@example.com", Password: "long enough pw"})
	wantCode(t, err, rpc.CodePermissionDenied)
	_, err = f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: pair.RefreshToken})
	wantCode(t, err, rpc.CodeUnauthenticated) // the ban revoked every refresh token
	wantCode(t, f.svc.CheckAccount(ctx, r.AccountID), rpc.CodePermissionDenied)
	wantCode(t, f.svc.CheckAccount(ctx, 424242), rpc.CodeUnauthenticated)
	wantCode(t, f.svc.Ban(ctx, 1, 424242, &until, "x"), rpc.CodeNotFound)

	// Bans end on their own.
	f.clk.Advance(49 * time.Hour)
	if _, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "d@example.com", Password: "long enough pw"}); err != nil {
		t.Fatalf("after ban expiry: %v", err)
	}
	permanent := time.Date(9999, 12, 31, 0, 0, 0, 0, time.UTC)
	_ = f.svc.Ban(ctx, 1, r.AccountID, &permanent, "cheating")
	_, err = f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "d@example.com", Password: "long enough pw"})
	if !strings.Contains(err.Error(), "permanently") {
		t.Fatalf("permanent ban message: %v", err)
	}
	if err := f.svc.Ban(ctx, 1, r.AccountID, nil, ""); err != nil {
		t.Fatal(err)
	}
	if err := f.svc.CheckAccount(ctx, r.AccountID); err != nil {
		t.Fatalf("unbanned: %v", err)
	}
	if len(f.banned) != 2 {
		t.Fatalf("unban must not kick; permanent ban must: %v", f.banned)
	}
}

func TestLaunchCodes(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	f.register(t, "e@example.com", "Eve", "long enough pw")
	pair, _ := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "e@example.com", Password: "long enough pw"})
	p, _ := f.svc.Verifier().Verify(pair.AccessToken)
	code, err := f.svc.CreateLaunchCode(ctx, p)
	if err != nil || !strings.HasPrefix(code.Code, "hlc1_") || !code.ExpiresAt.Equal(f.clk.Now().Add(time.Minute)) {
		t.Fatalf("create: %+v %v", code, err)
	}
	got, err := f.svc.ExchangeLaunchCode(ctx, meta, &identity.ExchangeLaunchCodeRequest{Code: code.Code})
	if err != nil || got.AccountID != p.AccountID {
		t.Fatalf("exchange: %v", err)
	}
	_, err = f.svc.ExchangeLaunchCode(ctx, meta, &identity.ExchangeLaunchCodeRequest{Code: code.Code})
	wantCode(t, err, rpc.CodeUnauthenticated) // one-time
	code2, _ := f.svc.CreateLaunchCode(ctx, p)
	f.redis.Mini.FastForward(61 * time.Second)
	_, err = f.svc.ExchangeLaunchCode(ctx, meta, &identity.ExchangeLaunchCodeRequest{Code: code2.Code})
	wantCode(t, err, rpc.CodeUnauthenticated) // expired
	_, err = f.svc.ExchangeLaunchCode(ctx, meta, &identity.ExchangeLaunchCodeRequest{Code: "nope"})
	wantCode(t, err, rpc.CodeUnauthenticated)

	// The exchanged refresh token is a branch of the launcher's family: it rotates on its own...
	next, err := f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: got.RefreshToken})
	if err != nil {
		t.Fatalf("game client refresh: %v", err)
	}
	// ...and logging out of the launcher logs the game client out too.
	if _, err := f.svc.Logout(ctx, meta, &identity.LogoutRequest{RefreshToken: pair.RefreshToken}); err != nil {
		t.Fatal(err)
	}
	_, err = f.svc.Refresh(ctx, meta, &identity.RefreshRequest{RefreshToken: next.RefreshToken})
	wantCode(t, err, rpc.CodeUnauthenticated)
}

func TestLaunchCodesNeedALiveLogin(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	f.register(t, "g@example.com", "Gus", "long enough pw")
	pair, _ := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "g@example.com", Password: "long enough pw"})
	p, _ := f.svc.Verifier().Verify(pair.AccessToken)
	pending, err := f.svc.CreateLaunchCode(ctx, p)
	if err != nil {
		t.Fatal(err)
	}
	// Regression: after logout the (still unexpired) access token could mint a launch code and
	// trade it for a brand-new 30-day refresh family, defeating the logout.
	if _, err := f.svc.Logout(ctx, meta, &identity.LogoutRequest{RefreshToken: pair.RefreshToken}); err != nil {
		t.Fatal(err)
	}
	_, err = f.svc.CreateLaunchCode(ctx, p)
	wantCode(t, err, rpc.CodeUnauthenticated)
	// A code created before the logout dies with the family.
	_, err = f.svc.ExchangeLaunchCode(ctx, meta, &identity.ExchangeLaunchCodeRequest{Code: pending.Code})
	wantCode(t, err, rpc.CodeUnauthenticated)
	// An access token that names no family (or another account's) is refused.
	_, err = f.svc.CreateLaunchCode(ctx, &authn.Principal{AccountID: p.AccountID, Scope: p.Scope})
	wantCode(t, err, rpc.CodeUnauthenticated)
	other := f.register(t, "h@example.com", "Hal", "long enough pw")
	pair2, _ := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "g@example.com", Password: "long enough pw"})
	p2, _ := f.svc.Verifier().Verify(pair2.AccessToken)
	forged := *p2
	forged.AccountID = other.AccountID
	_, err = f.svc.CreateLaunchCode(ctx, &forged)
	wantCode(t, err, rpc.CodeUnauthenticated)
	// A ban between creation and exchange wins.
	code, _ := f.svc.CreateLaunchCode(ctx, p2)
	until := f.clk.Now().Add(time.Hour)
	if err := f.svc.Ban(ctx, 1, p2.AccountID, &until, "x"); err != nil {
		t.Fatal(err)
	}
	_, err = f.svc.ExchangeLaunchCode(ctx, meta, &identity.ExchangeLaunchCodeRequest{Code: code.Code})
	wantCode(t, err, rpc.CodePermissionDenied)
}

func TestRehashOnLogin(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	r := f.register(t, "g@example.com", "Gus", "long enough pw")
	acct, _ := f.store.AccountByID(ctx, r.AccountID)
	old := acct.PasswordHash
	// Simulate a stored hash from weaker parameters.
	weak, _ := identity.NewHasher(identity.HashParams{MemoryKiB: 32, Iterations: 1, Parallelism: 1}, 1).Hash(ctx, "long enough pw")
	_ = f.store.SetPasswordHash(ctx, r.AccountID, weak, f.clk.Now())
	if _, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "g@example.com", Password: "long enough pw"}); err != nil {
		t.Fatal(err)
	}
	acct, _ = f.store.AccountByID(ctx, r.AccountID)
	if acct.PasswordHash == weak || !strings.Contains(acct.PasswordHash, "m=64,t=1,p=1") || acct.PasswordHash == old {
		t.Fatalf("hash not upgraded: %s", acct.PasswordHash)
	}
}

func TestSeedDev(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	n, err := f.svc.SeedDev(ctx)
	if err != nil || n != 10 {
		t.Fatalf("seed: %d %v", n, err)
	}
	if n, _ := f.svc.SeedDev(ctx); n != 0 {
		t.Fatal("seeding must be idempotent")
	}
	if _, err := f.svc.Login(ctx, meta, &identity.LoginRequest{Login: "dev3#0001", Password: "dev"}); err != nil {
		t.Fatalf("dev3 login: %v", err)
	}
}

func TestHTTPTransport(t *testing.T) {
	f := newFixture(t)
	r := chi.NewRouter()
	r.Use(authn.Middleware(f.svc.Verifier(), nil))
	f.svc.Mount(r, identity.HTTPOptions{})
	admin := chi.NewRouter()
	f.svc.MountAdmin(admin)
	srv := httptest.NewServer(r)
	defer srv.Close()

	call := func(path, bearer, body string) (int, map[string]any) {
		req, _ := http.NewRequest(http.MethodPost, srv.URL+identity.ServicePath+path, strings.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		if bearer != "" {
			req.Header.Set("Authorization", "Bearer "+bearer)
		}
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer res.Body.Close()
		var m map[string]any
		_ = json.NewDecoder(res.Body).Decode(&m)
		return res.StatusCode, m
	}
	st, m := call("Register", "", `{"email":"h@example.com","handle":"Hal","password":"long enough pw"}`)
	if st != 200 || m["accountId"] == nil {
		t.Fatalf("register: %d %v", st, m)
	}
	if _, isString := m["accountId"].(string); !isString {
		t.Fatal("64-bit ids must be JSON strings")
	}
	st, m = call("Login", "", `{"login":"h@example.com","password":"long enough pw"}`)
	if st != 200 {
		t.Fatalf("login: %d %v", st, m)
	}
	access := m["accessToken"].(string)
	st, m = call("GetAccount", access, `{}`)
	if st != 200 || m["email"] != "h@example.com" {
		t.Fatalf("GetAccount: %d %v", st, m)
	}
	st, m = call("GetAccount", "", `{}`)
	if st != 401 || m["code"] != "unauthenticated" {
		t.Fatalf("GetAccount without token: %d %v", st, m)
	}
	st, m = call("GetAccount", "not.a.jwt", `{}`)
	if st != 401 || !strings.Contains(m["message"].(string), "invalid or expired") {
		t.Fatalf("bad token: %d %v", st, m)
	}
	// A stale token must not block public calls.
	if st, _ = call("Login", "not.a.jwt", `{"login":"h@example.com","password":"long enough pw"}`); st != 200 {
		t.Fatalf("login with stale bearer: %d", st)
	}
	st, m = call("Login", "", `{"login":"h@example.com","password":"nope"}`)
	if st != 401 || m["code"] != "unauthenticated" {
		t.Fatalf("bad login: %d %v", st, m)
	}
	st, m = call("CreateLaunchCode", access, `{}`)
	if st != 200 {
		t.Fatalf("launch code: %d %v", st, m)
	}
	st, _ = call("ExchangeLaunchCode", "", `{"code":"`+m["code"].(string)+`"}`)
	if st != 200 {
		t.Fatalf("exchange: %d", st)
	}

	res, err := http.Get(srv.URL + "/.well-known/jwks.json")
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(res.Body)
	res.Body.Close()
	keys, err := authn.ParseJWKS(body)
	if err != nil || len(keys) != 1 {
		t.Fatalf("jwks: %s %v", body, err)
	}

	// Admin ban through the ops router.
	p, _ := f.svc.Verifier().Verify(access)
	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/admin/identity/Ban",
		strings.NewReader(`{"accountId":"`+itoa(p.AccountID)+`","until":"2030-01-01T00:00:00Z","reason":"test"}`))
	admin.ServeHTTP(w, req)
	if w.Code != 200 {
		t.Fatalf("admin ban: %d %s", w.Code, w.Body)
	}
	w = httptest.NewRecorder()
	admin.ServeHTTP(w, httptest.NewRequest(http.MethodPost, "/admin/identity/VerifyAudit", strings.NewReader(`{}`)))
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"entries"`) {
		t.Fatalf("verify audit: %d %s", w.Code, w.Body)
	}
	st, m = call("Login", "", `{"login":"h@example.com","password":"long enough pw"}`)
	if st != 403 || m["code"] != "permission_denied" {
		t.Fatalf("banned login: %d %v", st, m)
	}
}

func itoa(v int64) string {
	b, _ := json.Marshal(v)
	return string(b)
}
