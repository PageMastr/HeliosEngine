// Package identity is the Identity/Auth service (05 §1.1): accounts with a global
// handle#discriminator registry, argon2id passwords, EdDSA access JWTs, rotating refresh-token
// families with reuse detection, one-time launch codes, bans, per-IP/per-account rate limits and
// a hash-chained audit log. It is the only store of direct PII, which it keeps encrypted under
// per-account data keys and finds by blind index (05 §6.6, Phase 0 rule).
package identity

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"math/big"
	"net/mail"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/redis/go-redis/v9"

	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/authn"
	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/idgen"
	"github.com/PageMastr/scifi-test/services/pkg/ratelimit"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// Audit actions.
const (
	ActionRegister      = "account.register"
	ActionLogin         = "auth.login"
	ActionLoginFailed   = "auth.login.failed"
	ActionLoginDenied   = "auth.login.denied"
	ActionRefreshReuse  = "auth.refresh.reuse"
	ActionLogout        = "auth.logout"
	ActionLaunchCode    = "auth.launch_code.exchange"
	ActionBan           = "account.ban"
	ActionUnban         = "account.unban"
	ActionAuditRechain  = "audit.rechain" // WP-0.15r's one-time move to row format 2
	refreshTokenPrefix  = "hrt1_"
	launchCodePrefix    = "hlc1_"
	launchCodeKeyPrefix = "identity:lc:"
	maxBanReason        = 1024 // bytes of GM text per ban
)

// Deps are the collaborators of the service.
type Deps struct {
	Config  platform.IdentityConfig
	Store   Store
	PII     *PIIKeys // the KEK and blind-index pepper that protect e-mail addresses (05 §6.6)
	Hasher  *Hasher
	Issuer  *authn.Issuer
	Keys    *authn.KeySet
	Limiter *ratelimit.Limiter
	Cache   redis.Cmdable // launch codes
	IDs     *idgen.Minter
	Clock   clock.Clock
	Log     *slog.Logger
	Metrics prometheus.Registerer
	// OnBan, if set, is called after a ban is recorded so live game sessions end now instead of
	// at the next token issuance (the backend wires the session service's EndAccount).
	OnBan func(ctx context.Context, accountID int64)
}

// Service implements the identity API. Safe for concurrent use.
type Service struct {
	cfg      platform.IdentityConfig
	store    Store
	pii      *PIIKeys
	hasher   *Hasher
	issuer   *authn.Issuer
	keys     *authn.KeySet
	verifier *authn.Verifier
	limiter  *ratelimit.Limiter
	cache    redis.Cmdable
	ids      *idgen.Minter
	clk      clock.Clock
	log      *slog.Logger
	m        *metrics
	onBan    func(ctx context.Context, accountID int64)

	stopPurge context.CancelFunc
	purgeDone chan struct{}
}

type metrics struct {
	logins        *prometheus.CounterVec
	registrations *prometheus.CounterVec
	refreshes     *prometheus.CounterVec
	hashWait      prometheus.Histogram
	hashWork      prometheus.Histogram
}

func newMetrics(reg prometheus.Registerer) *metrics {
	m := &metrics{
		logins: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "helios_identity_logins_total",
			Help: "Login attempts by result."}, []string{"result"}),
		registrations: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "helios_identity_registrations_total",
			Help: "Registration attempts by result."}, []string{"result"}),
		refreshes: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "helios_identity_refreshes_total",
			Help: "Refresh-token rotations by result."}, []string{"result"}),
		hashWait: prometheus.NewHistogram(prometheus.HistogramOpts{Name: "helios_identity_argon2_wait_seconds",
			Help: "Time spent queueing for an argon2 slot.", Buckets: prometheus.ExponentialBuckets(0.001, 4, 8)}),
		hashWork: prometheus.NewHistogram(prometheus.HistogramOpts{Name: "helios_identity_argon2_seconds",
			Help: "argon2id computation time.", Buckets: prometheus.ExponentialBuckets(0.005, 2, 10)}),
	}
	if reg != nil {
		reg.MustRegister(m.logins, m.registrations, m.refreshes, m.hashWait, m.hashWork)
	}
	return m
}

// New builds the service.
func New(d Deps) (*Service, error) {
	if d.Store == nil || d.PII == nil || d.Hasher == nil || d.Issuer == nil || d.Keys == nil || d.Limiter == nil || d.Cache == nil || d.IDs == nil {
		return nil, errors.New("identity: missing dependency")
	}
	if d.Clock == nil {
		d.Clock = clock.System{}
	}
	if d.Log == nil {
		d.Log = slog.Default()
	}
	s := &Service{
		cfg: d.Config, store: d.Store, pii: d.PII, hasher: d.Hasher, issuer: d.Issuer, keys: d.Keys, limiter: d.Limiter,
		cache: d.Cache, ids: d.IDs, clk: d.Clock, log: d.Log, m: newMetrics(d.Metrics), onBan: d.OnBan,
		verifier: authn.NewVerifier(d.Keys, d.Config.Issuer, d.Config.Audience, d.Clock),
	}
	d.Hasher.Observe = func(wait, work time.Duration) {
		s.m.hashWait.Observe(wait.Seconds())
		s.m.hashWork.Observe(work.Seconds())
	}
	return s, nil
}

// Name implements app.Service.
func (s *Service) Name() string { return platform.ServiceIdentity }

// loginHistoryPurgeEvery is how often Start's job deletes login history past its retention.
const loginHistoryPurgeEvery = time.Hour

// Start implements app.Service: it runs the login-history retention job (05 §6.6: 90 days) now
// and then hourly until Stop.
func (s *Service) Start(context.Context) error {
	ctx, cancel := context.WithCancel(context.Background())
	s.stopPurge, s.purgeDone = cancel, make(chan struct{})
	go func() {
		defer close(s.purgeDone)
		t := time.NewTicker(loginHistoryPurgeEvery)
		defer t.Stop()
		for {
			if n, err := s.PurgeLoginHistory(ctx); err != nil && ctx.Err() == nil {
				s.log.WarnContext(ctx, "login history purge failed", "err", err)
			} else if n > 0 {
				s.log.InfoContext(ctx, "login history purged", "rows", n)
			}
			select {
			case <-ctx.Done():
				return
			case <-t.C:
			}
		}
	}()
	return nil
}

// Stop implements app.Service.
func (s *Service) Stop(context.Context) error {
	if s.stopPurge != nil {
		s.stopPurge()
		<-s.purgeDone
	}
	return nil
}

// PurgeLoginHistory deletes login history older than LoginHistoryRetention (the retention job;
// ops and tests may call it directly).
func (s *Service) PurgeLoginHistory(ctx context.Context) (int64, error) {
	return s.store.PurgeLoginHistory(ctx, s.clk.Now().UTC().Add(-LoginHistoryRetention))
}

// sealIP seals a client IP for storage under the account's DEK at the location aad. No IP, or a
// shredded key, stores nothing.
func (s *Service) sealIP(ctx context.Context, accountID int64, aad []byte, ip string) ([]byte, error) {
	if ip == "" {
		return nil, nil
	}
	key, err := s.store.SubjectKey(ctx, accountID)
	if err != nil {
		return nil, err
	}
	return s.pii.Seal(key, aad, ip)
}

// recordIP appends the IP an account's event came from to the login history, sealed under the
// account's DEK (05 §6.6); the audit row itself holds only pseudonymous IDs (05 §1.17). Events
// without a subject (an unknown login) keep no IP at all. Failures are logged, never surfaced.
func (s *Service) recordIP(ctx context.Context, accountID int64, action, ip string, at time.Time) {
	if accountID == 0 || ip == "" {
		return
	}
	err := func() error {
		id, err := s.ids.Next()
		if err != nil {
			return err
		}
		ct, err := s.sealIP(ctx, accountID, LoginIPAAD(id), ip)
		if err != nil || ct == nil {
			return err
		}
		return s.store.AppendLoginEvent(ctx, &LoginEvent{ID: id, AccountID: accountID, Action: action,
			At: at.UTC().Truncate(time.Microsecond), ClientIPCT: ct})
	}()
	if err != nil {
		s.log.ErrorContext(ctx, "login history append failed", "action", action, "account", accountID, "err", err)
	}
}

// Health implements app.Service.
func (s *Service) Health(ctx context.Context) error { return s.store.Ping(ctx) }

// Verifier returns the access-token verifier other in-process services share.
func (s *Service) Verifier() *authn.Verifier { return s.verifier }

// Meta is per-request context captured by the transport.
type Meta struct {
	ClientIP  string
	UserAgent string
}

// --- API messages (JSON field names follow proto3 JSON: lowerCamelCase, 64-bit ints as strings) ---

// RegisterRequest creates an account.
type RegisterRequest struct {
	Email    string `json:"email"`
	Handle   string `json:"handle"`
	Password string `json:"password"`
}

// RegisterResponse identifies the new account.
type RegisterResponse struct {
	AccountID     int64  `json:"accountId,string"`
	Handle        string `json:"handle"`
	Discriminator int    `json:"discriminator"`
	Tag           string `json:"tag"`
}

// LoginRequest authenticates with an email or a "Handle#1234" tag.
type LoginRequest struct {
	Login    string `json:"login"`
	Password string `json:"password"`
}

// TokenPair is returned by Login, Refresh and ExchangeLaunchCode.
type TokenPair struct {
	AccountID        int64     `json:"accountId,string"`
	Tag              string    `json:"tag"`
	TokenType        string    `json:"tokenType"`
	AccessToken      string    `json:"accessToken"`
	AccessExpiresAt  time.Time `json:"accessExpiresAt"`
	RefreshToken     string    `json:"refreshToken"`
	RefreshExpiresAt time.Time `json:"refreshExpiresAt"`
}

// RefreshRequest rotates a refresh token.
type RefreshRequest struct {
	RefreshToken string `json:"refreshToken"`
}

// LogoutRequest revokes the refresh-token family.
type LogoutRequest struct {
	RefreshToken string `json:"refreshToken"`
}

// Empty is an empty message.
type Empty struct{}

// LaunchCode is a one-time code the launcher passes to the client on its command line, so the
// refresh token never appears there (05 §1.1).
type LaunchCode struct {
	Code      string    `json:"code"`
	ExpiresAt time.Time `json:"expiresAt"`
}

// ExchangeLaunchCodeRequest redeems a launch code.
type ExchangeLaunchCodeRequest struct {
	Code string `json:"code"`
}

// AccountInfo describes the caller's account.
type AccountInfo struct {
	AccountID   int64      `json:"accountId,string"`
	Email       string     `json:"email"`
	Tag         string     `json:"tag"`
	CreatedAt   time.Time  `json:"createdAt"`
	LastLoginAt *time.Time `json:"lastLoginAt,omitempty"`
}

// --- validation ---

var (
	handleRE        = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_-]{2,23}$`)
	reservedHandles = map[string]bool{"admin": true, "administrator": true, "gm": true, "system": true, "helios": true,
		"support": true, "moderator": true, "root": true, "server": true}
	errInvalidCredentials = rpc.Errorf(rpc.CodeUnauthenticated, "invalid login or password")
	// errBusy sheds load when every argon2 slot stays taken for Hasher.MaxWait.
	errBusy = &rpc.Error{Code: rpc.CodeUnavailable, Message: "server busy; retry", RetryAfter: 2 * time.Second}
)

// NormalizeEmail lowercases and trims an email address.
func NormalizeEmail(s string) string { return strings.ToLower(strings.TrimSpace(s)) }

func validateEmail(email string) error {
	if len(email) > 254 {
		return rpc.Errorf(rpc.CodeInvalidArgument, "email is too long")
	}
	addr, err := mail.ParseAddress(email)
	if err != nil || addr.Address != email || addr.Name != "" {
		return rpc.Errorf(rpc.CodeInvalidArgument, "email is not a valid address")
	}
	at := strings.LastIndexByte(email, '@')
	if at < 1 || !strings.Contains(email[at+1:], ".") {
		return rpc.Errorf(rpc.CodeInvalidArgument, "email is not a valid address")
	}
	return nil
}

func (s *Service) validatePassword(pw, email, handle string) error {
	n := utf8.RuneCountInString(pw)
	switch {
	case n < s.cfg.PasswordMinLength:
		return rpc.Errorf(rpc.CodeInvalidArgument, "password must be at least %d characters", s.cfg.PasswordMinLength)
	case len(pw) > MaxPasswordBytes:
		return rpc.Errorf(rpc.CodeInvalidArgument, "password must be at most %d bytes", MaxPasswordBytes)
	case strings.EqualFold(pw, email) || strings.EqualFold(pw, handle):
		return rpc.Errorf(rpc.CodeInvalidArgument, "password must differ from email and handle")
	}
	return nil
}

// parseLogin splits a login into an email or a handle#discriminator tag.
func parseLogin(login string) (email, handleNorm string, disc int16, isTag bool, err error) {
	login = strings.TrimSpace(login)
	if i := strings.LastIndexByte(login, '#'); i > 0 && !strings.Contains(login, "@") {
		d, perr := strconv.Atoi(login[i+1:])
		if perr != nil || d < 1 || d > 9999 {
			return "", "", 0, false, rpc.Errorf(rpc.CodeInvalidArgument, "tag must look like Name#1234")
		}
		return "", strings.ToLower(login[:i]), int16(d), true, nil
	}
	if login == "" {
		return "", "", 0, false, rpc.Errorf(rpc.CodeInvalidArgument, "login is required")
	}
	return NormalizeEmail(login), "", 0, false, nil
}

func (s *Service) limit(ctx context.Context, key string, l platform.RateLimit, what string) error {
	res, err := s.limiter.Allow(ctx, key, ratelimit.Limit{Rate: l.Rate, Per: l.Per.D(), Burst: l.Burst})
	if err != nil {
		return rpc.Internal(err)
	}
	if !res.Allowed {
		return &rpc.Error{Code: rpc.CodeResourceExhausted, Message: "too many " + what + " attempts; retry later", RetryAfter: res.RetryAfter}
	}
	return nil
}

func hashKey(s string) string {
	sum := sha256.Sum256([]byte(s))
	return hex.EncodeToString(sum[:12])
}

func randomDiscriminator() (int16, error) {
	n, err := rand.Int(rand.Reader, big.NewInt(9999))
	if err != nil {
		return 0, err
	}
	return int16(n.Int64() + 1), nil
}

// --- API ---

// Register creates an account with a random free discriminator for its handle.
func (s *Service) Register(ctx context.Context, meta Meta, req *RegisterRequest) (*RegisterResponse, error) {
	res, err := s.register(ctx, meta, req, false)
	result := "ok"
	if err != nil {
		result = string(rpc.CodeOf(err))
	}
	s.m.registrations.WithLabelValues(result).Inc()
	return res, err
}

func (s *Service) register(ctx context.Context, meta Meta, req *RegisterRequest, seed bool) (*RegisterResponse, error) {
	if !s.cfg.AllowRegistration && !seed {
		return nil, rpc.Errorf(rpc.CodePermissionDenied, "registration is closed")
	}
	if !seed {
		if err := s.limit(ctx, "register:ip:"+meta.ClientIP, s.cfg.RegisterPerIP, "registration"); err != nil {
			return nil, err
		}
	}
	email := strings.TrimSpace(req.Email)
	handle := strings.TrimSpace(req.Handle)
	if err := validateEmail(email); err != nil {
		return nil, err
	}
	if !handleRE.MatchString(handle) {
		return nil, rpc.Errorf(rpc.CodeInvalidArgument, "handle must be 3-24 characters: a letter, then letters, digits, _ or -")
	}
	if reservedHandles[strings.ToLower(handle)] && !seed {
		return nil, rpc.Errorf(rpc.CodeInvalidArgument, "handle is reserved")
	}
	if !seed {
		if err := s.validatePassword(req.Password, email, handle); err != nil {
			return nil, err
		}
	}
	hash, err := s.hasher.Hash(ctx, req.Password)
	if err != nil {
		if errors.Is(err, ErrHasherBusy) {
			return nil, errBusy
		}
		return nil, rpc.Internal(err)
	}
	id, err := s.ids.Next()
	if err != nil {
		return nil, rpc.Internal(err)
	}
	now := s.clk.Now().UTC()
	emailCT, emailBidx, key, err := s.pii.EncryptEmail(id, email)
	if err != nil {
		return nil, rpc.Internal(err)
	}
	acct := &Account{ID: id, EmailCT: emailCT, EmailBidx: emailBidx, Handle: handle, HandleNorm: strings.ToLower(handle),
		PasswordHash: hash, CreatedAt: now, UpdatedAt: now}
	// Random discriminators; with 9,999 per handle, 20 tries fail only for nearly full handles.
	for attempt := 0; attempt < 20; attempt++ {
		if seed && attempt == 0 {
			acct.Discriminator = 1
		} else if acct.Discriminator, err = randomDiscriminator(); err != nil {
			return nil, rpc.Internal(err)
		}
		audit := NewAudit(now, id, id, ActionRegister, map[string]any{"tag": acct.Tag(), "seed": seed})
		err = s.store.CreateAccount(ctx, acct, key, audit)
		switch {
		case err == nil:
			s.log.InfoContext(ctx, "account registered", "account", id, "tag", acct.Tag())
			s.recordIP(ctx, id, ActionRegister, meta.ClientIP, now)
			return &RegisterResponse{AccountID: id, Handle: handle, Discriminator: int(acct.Discriminator), Tag: acct.Tag()}, nil
		case errors.Is(err, ErrEmailTaken):
			return nil, rpc.Errorf(rpc.CodeAlreadyExists, "an account with this email already exists")
		case errors.Is(err, ErrTagTaken):
			continue
		default:
			return nil, rpc.Internal(err)
		}
	}
	return nil, rpc.Errorf(rpc.CodeAlreadyExists, "handle %q is unavailable; choose another", handle)
}

// Login verifies credentials and returns a fresh token pair (a new refresh family).
func (s *Service) Login(ctx context.Context, meta Meta, req *LoginRequest) (*TokenPair, error) {
	res, result, err := s.login(ctx, meta, req)
	s.m.logins.WithLabelValues(result).Inc()
	return res, err
}

func (s *Service) login(ctx context.Context, meta Meta, req *LoginRequest) (*TokenPair, string, error) {
	if err := s.limit(ctx, "login:ip:"+meta.ClientIP, s.cfg.LoginPerIP, "login"); err != nil {
		return nil, "rate_limited", err
	}
	email, handleNorm, disc, isTag, err := parseLogin(req.Login)
	if err != nil {
		return nil, "invalid", err
	}
	// unknownKey names a login that matches no account for its rate-limit bucket (Valkey, TTL'd).
	// An e-mail is looked up, and named, only by its keyed blind index, so neither the address
	// nor an unkeyed hash of it reaches Valkey; the audit row names neither (05 §1.17, §6.6).
	var acct *Account
	var unknownKey string
	if isTag {
		acct, err = s.store.AccountByTag(ctx, handleNorm, disc)
		unknownKey = hashKey(fmt.Sprintf("%s#%d", handleNorm, disc))
	} else {
		bidx := s.pii.EmailIndex(email)
		acct, err = s.store.AccountByEmailIndex(ctx, bidx)
		unknownKey = hex.EncodeToString(bidx[:12])
	}
	if err != nil && !errors.Is(err, ErrNotFound) {
		return nil, "error", rpc.Internal(err)
	}
	// The per-account bucket is keyed by the resolved account, so alternating between the email
	// and the tag does not double an attacker's guesses; unknown logins get a bucket of their own.
	bucket := "login:acct:" + unknownKey
	if acct != nil {
		bucket = "login:acct:" + strconv.FormatInt(acct.ID, 10)
	}
	if err := s.limit(ctx, bucket, s.cfg.LoginPerAccount, "login"); err != nil {
		return nil, "rate_limited", err
	}
	now := s.clk.Now().UTC()
	if acct == nil {
		// DummyVerify spends what argon2id would, so an unknown login costs about what a bad
		// password does. The known-account paths also seal and store the client IP (well under a
		// millisecond against argon2id's tens): an accepted, rate-limited oracle, since whether an
		// address or tag exists already shows through Register.
		if derr := s.hasher.DummyVerify(ctx, req.Password); errors.Is(derr, ErrHasherBusy) {
			return nil, "busy", errBusy
		}
		s.audit(ctx, NewAudit(now, 0, 0, ActionLoginFailed, map[string]any{"reason": "unknown_login"}))
		return nil, "unknown", errInvalidCredentials
	}
	ok, rehash, err := s.hasher.Verify(ctx, req.Password, acct.PasswordHash)
	if errors.Is(err, ErrHasherBusy) {
		return nil, "busy", errBusy
	}
	if err != nil {
		return nil, "error", rpc.Internal(err)
	}
	if !ok {
		s.audit(ctx, NewAudit(now, 0, acct.ID, ActionLoginFailed, map[string]any{"reason": "bad_password"}))
		s.recordIP(ctx, acct.ID, ActionLoginFailed, meta.ClientIP, now)
		return nil, "bad_password", errInvalidCredentials
	}
	if acct.BannedAt(now) {
		s.audit(ctx, NewAudit(now, acct.ID, acct.ID, ActionLoginDenied, map[string]any{"reason": "banned"}))
		s.recordIP(ctx, acct.ID, ActionLoginDenied, meta.ClientIP, now)
		return nil, "banned", bannedError(acct)
	}
	if rehash {
		if h, err := s.hasher.Hash(ctx, req.Password); err == nil {
			if err := s.store.SetPasswordHash(ctx, acct.ID, h, now); err != nil {
				s.log.WarnContext(ctx, "password rehash failed", "account", acct.ID, "err", err)
			}
		}
	}
	pair, err := s.issuePair(ctx, acct, 0, meta)
	if err != nil {
		return nil, "error", err
	}
	if err := s.store.RecordLogin(ctx, acct.ID, now,
		NewAudit(now, acct.ID, acct.ID, ActionLogin, map[string]any{"ua": truncate(meta.UserAgent, 128)})); err != nil {
		return nil, "error", rpc.Internal(err)
	}
	s.recordIP(ctx, acct.ID, ActionLogin, meta.ClientIP, now)
	return pair, "ok", nil
}

func bannedError(a *Account) *rpc.Error {
	if a.BannedUntil != nil && a.BannedUntil.Year() >= 9999 {
		return rpc.Errorf(rpc.CodePermissionDenied, "account is suspended permanently")
	}
	return rpc.Errorf(rpc.CodePermissionDenied, "account is suspended until %s", a.BannedUntil.UTC().Format(time.RFC3339))
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

// audit appends a standalone entry; failures are logged, never surfaced to the caller.
func (s *Service) audit(ctx context.Context, e *AuditEntry) {
	if err := s.store.AppendAudit(ctx, e); err != nil {
		s.log.ErrorContext(ctx, "audit append failed", "action", e.Action, "err", err)
	}
}

func newOpaqueToken(prefix string, n int) (string, []byte, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", nil, err
	}
	tok := prefix + base64.RawURLEncoding.EncodeToString(b)
	sum := sha256.Sum256([]byte(tok))
	return tok, sum[:], nil
}

func hashOpaque(tok string) []byte {
	sum := sha256.Sum256([]byte(tok))
	return sum[:]
}

func scopesFor(a *Account) []string {
	if a.IsBot {
		return []string{authn.ScopeGame, authn.ScopeBot}
	}
	return []string{authn.ScopeGame}
}

// issuePair mints an access token and a refresh token. family 0 starts a new family.
func (s *Service) issuePair(ctx context.Context, acct *Account, family int64, meta Meta) (*TokenPair, error) {
	now := s.clk.Now().UTC()
	refresh, refreshHash, err := newOpaqueToken(refreshTokenPrefix, 32)
	if err != nil {
		return nil, rpc.Internal(err)
	}
	rt := &RefreshToken{Hash: refreshHash, AccountID: acct.ID, FamilyID: family, IssuedAt: now,
		ExpiresAt: now.Add(s.cfg.RefreshTTL.D())}
	if rt.ClientIPCT, err = s.sealIP(ctx, acct.ID, RefreshIPAAD(refreshHash), meta.ClientIP); err != nil {
		return nil, rpc.Internal(err)
	}
	if family == 0 {
		if rt.FamilyID, err = s.ids.Next(); err != nil {
			return nil, rpc.Internal(err)
		}
		if err := s.store.InsertRefreshToken(ctx, rt); err != nil {
			return nil, rpc.Internal(err)
		}
	}
	return s.finishPair(acct, rt, refresh)
}

func (s *Service) finishPair(acct *Account, rt *RefreshToken, refresh string) (*TokenPair, error) {
	access, exp, err := s.issuer.Issue(acct.ID, acct.Tag(), strconv.FormatInt(rt.FamilyID, 10), scopesFor(acct))
	if err != nil {
		return nil, rpc.Internal(err)
	}
	return &TokenPair{AccountID: acct.ID, Tag: acct.Tag(), TokenType: "Bearer", AccessToken: access, AccessExpiresAt: exp,
		RefreshToken: refresh, RefreshExpiresAt: rt.ExpiresAt}, nil
}

// Refresh rotates a refresh token. Presenting an already-used token revokes its whole family
// (the thief and the victim both have to log in again).
func (s *Service) Refresh(ctx context.Context, meta Meta, req *RefreshRequest) (*TokenPair, error) {
	res, result, err := s.refresh(ctx, meta, req)
	s.m.refreshes.WithLabelValues(result).Inc()
	return res, err
}

func (s *Service) refresh(ctx context.Context, meta Meta, req *RefreshRequest) (*TokenPair, string, error) {
	if err := s.limit(ctx, "refresh:ip:"+meta.ClientIP, s.cfg.RefreshPerIP, "refresh"); err != nil {
		return nil, "rate_limited", err
	}
	if !strings.HasPrefix(req.RefreshToken, refreshTokenPrefix) {
		return nil, "invalid", rpc.Errorf(rpc.CodeUnauthenticated, "invalid refresh token")
	}
	now := s.clk.Now().UTC()
	next, nextHash, err := newOpaqueToken(refreshTokenPrefix, 32)
	if err != nil {
		return nil, "error", rpc.Internal(err)
	}
	// The successor's IP is sealed under the family's account DEK, so the owner is looked up
	// first; the rotation re-checks it inside its transaction.
	oldHash := hashOpaque(req.RefreshToken)
	owner, err := s.store.RefreshTokenOwner(ctx, oldHash)
	if errors.Is(err, ErrTokenInvalid) {
		return nil, "invalid", rpc.Errorf(rpc.CodeUnauthenticated, "invalid or expired refresh token")
	}
	if err != nil {
		return nil, "error", rpc.Internal(err)
	}
	rt := &RefreshToken{Hash: nextHash, AccountID: owner, IssuedAt: now, ExpiresAt: now.Add(s.cfg.RefreshTTL.D())}
	if rt.ClientIPCT, err = s.sealIP(ctx, owner, RefreshIPAAD(nextHash), meta.ClientIP); err != nil {
		return nil, "error", rpc.Internal(err)
	}
	_, err = s.store.RotateRefreshToken(ctx, oldHash, rt, now, NewAudit(now, 0, 0, ActionRefreshReuse, nil))
	switch {
	case errors.Is(err, ErrTokenReused):
		s.log.WarnContext(ctx, "refresh token reuse detected; family revoked", "account", owner)
		s.recordIP(ctx, owner, ActionRefreshReuse, meta.ClientIP, now)
		return nil, "reused", rpc.Errorf(rpc.CodeUnauthenticated, "refresh token was already used; log in again")
	case errors.Is(err, ErrTokenInvalid):
		return nil, "invalid", rpc.Errorf(rpc.CodeUnauthenticated, "invalid or expired refresh token")
	case err != nil:
		return nil, "error", rpc.Internal(err)
	}
	acct, err := s.store.AccountByID(ctx, rt.AccountID)
	if err != nil {
		return nil, "error", rpc.Internal(err)
	}
	if acct.BannedAt(now) {
		_, _ = s.store.RevokeFamilyOf(ctx, nextHash, now, nil)
		return nil, "banned", bannedError(acct)
	}
	pair, err := s.finishPair(acct, rt, next)
	if err != nil {
		return nil, "error", err
	}
	return pair, "ok", nil
}

// Logout revokes the refresh-token family. Unknown tokens succeed (logout is idempotent).
func (s *Service) Logout(ctx context.Context, meta Meta, req *LogoutRequest) (*Empty, error) {
	now := s.clk.Now().UTC()
	t, err := s.store.RevokeFamilyOf(ctx, hashOpaque(req.RefreshToken), now, NewAudit(now, 0, 0, ActionLogout, nil))
	if err != nil && !errors.Is(err, ErrTokenInvalid) {
		return nil, rpc.Internal(err)
	}
	if err == nil {
		s.recordIP(ctx, t.AccountID, ActionLogout, meta.ClientIP, now)
	}
	return &Empty{}, nil
}

// GetAccount returns the caller's account.
func (s *Service) GetAccount(ctx context.Context, p *authn.Principal) (*AccountInfo, error) {
	acct, err := s.store.AccountByID(ctx, p.AccountID)
	if errors.Is(err, ErrNotFound) {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "account no longer exists")
	}
	if err != nil {
		return nil, rpc.Internal(err)
	}
	key, err := s.store.SubjectKey(ctx, acct.ID)
	if err != nil {
		return nil, rpc.Internal(err)
	}
	email, err := s.pii.DecryptEmail(acct, key)
	if err != nil && !errors.Is(err, ErrShredded) {
		return nil, rpc.Internal(err)
	}
	return &AccountInfo{AccountID: acct.ID, Email: email, Tag: acct.Tag(), CreatedAt: acct.CreatedAt, LastLoginAt: acct.LastLoginAt}, nil
}

type launchCodeRecord struct {
	AccountID int64 `json:"a,string"`
	FamilyID  int64 `json:"f,string"`
}

// CreateLaunchCode issues a 60-second one-time code for the authenticated caller. The code is
// bound to the refresh-token family behind the caller's access token, and only while that
// family is live: a logged-out (or reuse-revoked) family cannot mint codes, and a short-lived
// access token alone can never be turned into a fresh 30-day refresh family.
func (s *Service) CreateLaunchCode(ctx context.Context, p *authn.Principal) (*LaunchCode, error) {
	family, err := strconv.ParseInt(p.Family, 10, 64)
	if err != nil || family <= 0 {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "access token is not bound to a login")
	}
	acct, err := s.store.ActiveFamily(ctx, family, s.clk.Now().UTC())
	if errors.Is(err, ErrTokenInvalid) || (err == nil && acct != p.AccountID) {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "login has ended; log in again")
	}
	if err != nil {
		return nil, rpc.Internal(err)
	}
	if err := s.CheckAccount(ctx, p.AccountID); err != nil {
		return nil, err
	}
	code, h, err := newOpaqueToken(launchCodePrefix, 24)
	if err != nil {
		return nil, rpc.Internal(err)
	}
	rec, _ := json.Marshal(launchCodeRecord{AccountID: p.AccountID, FamilyID: family})
	ttl := s.cfg.LaunchCodeTTL.D()
	if err := s.cache.Set(ctx, launchCodeKeyPrefix+hex.EncodeToString(h), rec, ttl).Err(); err != nil {
		return nil, rpc.Errorf(rpc.CodeUnavailable, "launch codes unavailable")
	}
	return &LaunchCode{Code: code, ExpiresAt: s.clk.Now().UTC().Add(ttl)}, nil
}

// ExchangeLaunchCode redeems a launch code exactly once for a new token pair. The new refresh
// token joins the launcher's family as a second branch, so revoking that family (logout, or
// reuse detection) also ends the game client's login.
func (s *Service) ExchangeLaunchCode(ctx context.Context, meta Meta, req *ExchangeLaunchCodeRequest) (*TokenPair, error) {
	if err := s.limit(ctx, "refresh:ip:"+meta.ClientIP, s.cfg.RefreshPerIP, "launch code"); err != nil {
		return nil, err
	}
	if !strings.HasPrefix(req.Code, launchCodePrefix) {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "invalid launch code")
	}
	raw, err := s.cache.GetDel(ctx, launchCodeKeyPrefix+hex.EncodeToString(hashOpaque(req.Code))).Bytes()
	if errors.Is(err, redis.Nil) {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "invalid or expired launch code")
	}
	if err != nil {
		return nil, rpc.Errorf(rpc.CodeUnavailable, "launch codes unavailable")
	}
	var rec launchCodeRecord
	if err := json.Unmarshal(raw, &rec); err != nil || rec.FamilyID <= 0 {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "invalid launch code")
	}
	acct, err := s.store.AccountByID(ctx, rec.AccountID)
	if err != nil {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "invalid launch code")
	}
	now := s.clk.Now().UTC()
	if acct.BannedAt(now) {
		return nil, bannedError(acct)
	}
	refresh, refreshHash, err := newOpaqueToken(refreshTokenPrefix, 32)
	if err != nil {
		return nil, rpc.Internal(err)
	}
	rt := &RefreshToken{Hash: refreshHash, AccountID: acct.ID, FamilyID: rec.FamilyID, IssuedAt: now,
		ExpiresAt: now.Add(s.cfg.RefreshTTL.D())}
	if rt.ClientIPCT, err = s.sealIP(ctx, acct.ID, RefreshIPAAD(refreshHash), meta.ClientIP); err != nil {
		return nil, rpc.Internal(err)
	}
	switch err := s.store.ExtendFamily(ctx, rt, now); {
	case errors.Is(err, ErrTokenInvalid):
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "login has ended; log in again")
	case err != nil:
		return nil, rpc.Internal(err)
	}
	pair, err := s.finishPair(acct, rt, refresh)
	if err != nil {
		return nil, err
	}
	s.audit(ctx, NewAudit(now, acct.ID, acct.ID, ActionLaunchCode, nil))
	s.recordIP(ctx, acct.ID, ActionLaunchCode, meta.ClientIP, now)
	return pair, nil
}

// CheckAccount returns nil when the account exists and is not banned; the session service
// calls it before minting connect tokens (policy denials happen at issuance, 04 §2.3).
func (s *Service) CheckAccount(ctx context.Context, accountID int64) error {
	acct, err := s.store.AccountByID(ctx, accountID)
	if errors.Is(err, ErrNotFound) {
		return rpc.Errorf(rpc.CodeUnauthenticated, "account no longer exists")
	}
	if err != nil {
		return rpc.Internal(err)
	}
	if acct.BannedAt(s.clk.Now()) {
		return bannedError(acct)
	}
	return nil
}

// Ban suspends an account until `until` (nil lifts a ban) and revokes its refresh tokens.
// Existing access tokens stay valid until they expire (≤ 10 min), but every issuance path
// (login, refresh, launch codes, connect tokens, reconnects) re-checks the ban, and the OnBan
// hook ends the account's live game session at once.
func (s *Service) Ban(ctx context.Context, actor, accountID int64, until *time.Time, reason string) error {
	if until != nil && strings.TrimSpace(reason) == "" {
		return rpc.Errorf(rpc.CodeInvalidArgument, "a ban needs a reason")
	}
	if len(reason) > maxBanReason {
		return rpc.Errorf(rpc.CodeInvalidArgument, "a ban reason is at most %d bytes", maxBanReason)
	}
	now := s.clk.Now().UTC()
	// The reason is GM free text: it is sealed under the account's DEK and kept off the chained
	// audit row, which holds only pseudonymous IDs (05 §1.17, §6.6).
	action, detail := ActionUnban, map[string]any{}
	var reasonCT []byte
	if until != nil {
		action, detail = ActionBan, map[string]any{"until": until.UTC().Format(time.RFC3339)}
		key, err := s.store.SubjectKey(ctx, accountID)
		if errors.Is(err, ErrNotFound) {
			return rpc.Errorf(rpc.CodeNotFound, "no such account")
		}
		if err != nil {
			return rpc.Internal(err)
		}
		if reasonCT, err = s.pii.Seal(key, BanReasonAAD(accountID), reason); err != nil {
			return rpc.Internal(err)
		}
	}
	err := s.store.SetBan(ctx, accountID, until, reasonCT, now, NewAudit(now, actor, accountID, action, detail))
	if errors.Is(err, ErrNotFound) {
		return rpc.Errorf(rpc.CodeNotFound, "no such account")
	}
	if err != nil {
		return rpc.Internal(err)
	}
	if until != nil && until.After(now) && s.onBan != nil {
		s.onBan(ctx, accountID)
	}
	return nil
}

// JWKS returns the verification keys.
func (s *Service) JWKS() authn.JWKS { return s.keys.JWKS() }

// SeedDev creates dev1..dev10 (password "dev", tags devN#0001) if missing. Dev only.
func (s *Service) SeedDev(ctx context.Context) (created int, err error) {
	for i := 1; i <= 10; i++ {
		email := fmt.Sprintf("dev%d@helios.test", i)
		if _, err := s.store.AccountByEmailIndex(ctx, s.pii.EmailIndex(email)); err == nil {
			continue
		}
		// Seeding has no client: no rate limit applies and no IP is recorded.
		_, err := s.register(ctx, Meta{}, &RegisterRequest{Email: email, Handle: fmt.Sprintf("dev%d", i), Password: "dev"}, true)
		if err != nil {
			return created, fmt.Errorf("seed dev%d: %w", i, err)
		}
		created++
	}
	return created, nil
}

// VerifyAudit re-checks the whole audit chain from the store.
func (s *Service) VerifyAudit(ctx context.Context) (int, error) {
	var prev [32]byte
	var after int64
	total := 0
	for {
		batch, err := s.store.ListAudit(ctx, after, 1000)
		if err != nil {
			return total, err
		}
		if len(batch) == 0 {
			return total, nil
		}
		if err := VerifyAuditChain(prev, batch); err != nil {
			return total, err
		}
		prev, after = batch[len(batch)-1].Hash, batch[len(batch)-1].Seq
		total += len(batch)
	}
}
