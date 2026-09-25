// Package authn issues and verifies Helios access tokens: EdDSA (Ed25519) JWTs with a 10-minute
// lifetime (05 §1.1). Every service verifies tokens locally against a JWKS key set, so an
// identity outage blocks only new logins, never calls with a valid token.
//
// Connect tokens (pkg/connecttoken) are a different thing and never use this package.
package authn

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/golang-jwt/jwt/v5"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
)

// Scopes carried in access tokens.
const (
	ScopeGame  = "game"  // play: sessions, connect tokens
	ScopeBot   = "bot"   // load-test bot credential; accepted only by test shards (05 §1.1)
	ScopeAdmin = "admin" // GM/admin API (Phase 1+)
)

// Errors returned by Verify.
var (
	ErrInvalidToken = errors.New("authn: invalid token")
	ErrExpiredToken = errors.New("authn: token expired")
)

// Claims are the JWT claims Helios puts in access tokens.
type Claims struct {
	jwt.RegisteredClaims
	Scope   []string `json:"scp,omitempty"`
	Family  string   `json:"fam,omitempty"` // refresh-token family the token was minted from
	Handle  string   `json:"hnd,omitempty"` // display tag "Name#1234"
	Version int      `json:"ver"`
}

// Principal is the verified caller.
type Principal struct {
	AccountID int64
	Handle    string
	Scope     []string
	Family    string
	TokenID   string
	ExpiresAt time.Time
}

// HasScope reports whether the principal carries scope s.
func (p *Principal) HasScope(s string) bool {
	for _, x := range p.Scope {
		if x == s {
			return true
		}
	}
	return false
}

// SigningKey is an Ed25519 key with its RFC 7638 thumbprint as key ID.
type SigningKey struct {
	ID      string
	Private ed25519.PrivateKey
}

// NewSigningKey wraps priv and derives its key ID.
func NewSigningKey(priv ed25519.PrivateKey) SigningKey {
	return SigningKey{ID: Thumbprint(priv.Public().(ed25519.PublicKey)), Private: priv}
}

// GenerateSigningKey creates a fresh key from rnd (crypto/rand when nil).
func GenerateSigningKey(rnd io.Reader) (SigningKey, error) {
	if rnd == nil {
		rnd = rand.Reader
	}
	_, priv, err := ed25519.GenerateKey(rnd)
	if err != nil {
		return SigningKey{}, err
	}
	return NewSigningKey(priv), nil
}

// Thumbprint is the RFC 7638 JWK thumbprint of an Ed25519 public key (base64url SHA-256 of the
// canonical {"crv","kty","x"} JSON).
func Thumbprint(pub ed25519.PublicKey) string {
	canon := `{"crv":"Ed25519","kty":"OKP","x":"` + base64.RawURLEncoding.EncodeToString(pub) + `"}`
	sum := sha256.Sum256([]byte(canon))
	return base64.RawURLEncoding.EncodeToString(sum[:])
}

// Issuer mints access tokens. Safe for concurrent use.
type Issuer struct {
	key      SigningKey
	issuer   string
	audience string
	ttl      time.Duration
	clk      clock.Clock
}

// NewIssuer returns an issuer signing with key.
func NewIssuer(key SigningKey, issuer, audience string, ttl time.Duration, clk clock.Clock) *Issuer {
	if clk == nil {
		clk = clock.System{}
	}
	return &Issuer{key: key, issuer: issuer, audience: audience, ttl: ttl, clk: clk}
}

// KeyID returns the signing key's ID.
func (i *Issuer) KeyID() string { return i.key.ID }

// PublicKey returns the signing key's public half.
func (i *Issuer) PublicKey() ed25519.PublicKey { return i.key.Private.Public().(ed25519.PublicKey) }

// Issue mints a token for account with the given scopes.
func (i *Issuer) Issue(accountID int64, handle, family string, scope []string) (string, time.Time, error) {
	now := i.clk.Now().Truncate(time.Second)
	exp := now.Add(i.ttl)
	var jti [16]byte
	if _, err := rand.Read(jti[:]); err != nil {
		return "", time.Time{}, err
	}
	claims := Claims{
		RegisteredClaims: jwt.RegisteredClaims{
			Issuer:    i.issuer,
			Subject:   strconv.FormatInt(accountID, 10),
			Audience:  jwt.ClaimStrings{i.audience},
			ExpiresAt: jwt.NewNumericDate(exp),
			NotBefore: jwt.NewNumericDate(now),
			IssuedAt:  jwt.NewNumericDate(now),
			ID:        hex.EncodeToString(jti[:]),
		},
		Scope:   scope,
		Family:  family,
		Handle:  handle,
		Version: 1,
	}
	tok := jwt.NewWithClaims(jwt.SigningMethodEdDSA, claims)
	tok.Header["kid"] = i.key.ID
	s, err := tok.SignedString(i.key.Private)
	if err != nil {
		return "", time.Time{}, err
	}
	return s, exp, nil
}

// KeySet is a set of verification keys by key ID. Safe for concurrent use.
type KeySet struct {
	mu   sync.RWMutex
	keys map[string]ed25519.PublicKey
}

// NewKeySet returns a set containing the given public keys.
func NewKeySet(pubs ...ed25519.PublicKey) *KeySet {
	ks := &KeySet{keys: map[string]ed25519.PublicKey{}}
	for _, p := range pubs {
		ks.keys[Thumbprint(p)] = p
	}
	return ks
}

// Replace swaps the whole set (JWKS refresh).
func (ks *KeySet) Replace(keys map[string]ed25519.PublicKey) {
	ks.mu.Lock()
	ks.keys = keys
	ks.mu.Unlock()
}

// Lookup returns the key for kid.
func (ks *KeySet) Lookup(kid string) (ed25519.PublicKey, bool) {
	ks.mu.RLock()
	defer ks.mu.RUnlock()
	k, ok := ks.keys[kid]
	return k, ok
}

// JWK is one entry of a JWKS document.
type JWK struct {
	KeyType   string `json:"kty"`
	Curve     string `json:"crv"`
	X         string `json:"x"`
	KeyID     string `json:"kid"`
	Use       string `json:"use,omitempty"`
	Algorithm string `json:"alg,omitempty"`
}

// JWKS is a JSON Web Key Set document.
type JWKS struct {
	Keys []JWK `json:"keys"`
}

// JWKS renders the set as a JWKS document (sorted by key ID for stable output).
func (ks *KeySet) JWKS() JWKS {
	ks.mu.RLock()
	defer ks.mu.RUnlock()
	out := JWKS{Keys: make([]JWK, 0, len(ks.keys))}
	for kid, pub := range ks.keys {
		out.Keys = append(out.Keys, JWK{KeyType: "OKP", Curve: "Ed25519", X: base64.RawURLEncoding.EncodeToString(pub),
			KeyID: kid, Use: "sig", Algorithm: "EdDSA"})
	}
	slices.SortFunc(out.Keys, func(a, b JWK) int { return strings.Compare(a.KeyID, b.KeyID) })
	return out
}

// ParseJWKS decodes a JWKS document, keeping only Ed25519 signing keys.
func ParseJWKS(b []byte) (map[string]ed25519.PublicKey, error) {
	var doc JWKS
	if err := json.Unmarshal(b, &doc); err != nil {
		return nil, fmt.Errorf("authn: jwks: %w", err)
	}
	keys := map[string]ed25519.PublicKey{}
	for _, k := range doc.Keys {
		if k.KeyType != "OKP" || k.Curve != "Ed25519" || (k.Use != "" && k.Use != "sig") {
			continue
		}
		x, err := base64.RawURLEncoding.DecodeString(k.X)
		if err != nil || len(x) != ed25519.PublicKeySize {
			return nil, fmt.Errorf("authn: jwks: bad key %q", k.KeyID)
		}
		kid := k.KeyID
		if kid == "" {
			kid = Thumbprint(x)
		}
		keys[kid] = ed25519.PublicKey(x)
	}
	return keys, nil
}

// Verifier checks access tokens. Safe for concurrent use.
type Verifier struct {
	keys     *KeySet
	issuer   string
	audience string
	clk      clock.Clock
	leeway   time.Duration
}

// NewVerifier returns a verifier accepting tokens signed by any key in keys.
func NewVerifier(keys *KeySet, issuer, audience string, clk clock.Clock) *Verifier {
	if clk == nil {
		clk = clock.System{}
	}
	return &Verifier{keys: keys, issuer: issuer, audience: audience, clk: clk, leeway: 5 * time.Second}
}

// Verify validates signature, algorithm, issuer, audience and time claims.
func (v *Verifier) Verify(token string) (*Principal, error) {
	claims := &Claims{}
	parser := jwt.NewParser(
		jwt.WithValidMethods([]string{jwt.SigningMethodEdDSA.Alg()}),
		jwt.WithIssuer(v.issuer),
		jwt.WithAudience(v.audience),
		jwt.WithExpirationRequired(),
		jwt.WithIssuedAt(),
		jwt.WithLeeway(v.leeway),
		jwt.WithTimeFunc(v.clk.Now),
	)
	_, err := parser.ParseWithClaims(token, claims, func(t *jwt.Token) (any, error) {
		kid, _ := t.Header["kid"].(string)
		k, ok := v.keys.Lookup(kid)
		if !ok {
			return nil, fmt.Errorf("unknown key id %q", kid)
		}
		return k, nil
	})
	if err != nil {
		if errors.Is(err, jwt.ErrTokenExpired) {
			return nil, ErrExpiredToken
		}
		return nil, fmt.Errorf("%w: %v", ErrInvalidToken, err)
	}
	id, err := strconv.ParseInt(claims.Subject, 10, 64)
	if err != nil || id <= 0 {
		return nil, ErrInvalidToken
	}
	return &Principal{
		AccountID: id,
		Handle:    claims.Handle,
		Scope:     claims.Scope,
		Family:    claims.Family,
		TokenID:   claims.ID,
		ExpiresAt: claims.ExpiresAt.Time,
	}, nil
}

// RemoteKeys keeps a KeySet in sync with a JWKS URL (for services running outside the
// identity process). Refresh is called on demand; a fetch failure keeps the cached keys.
type RemoteKeys struct {
	URL    string
	Client *http.Client
	Keys   *KeySet
}

// Refresh fetches the JWKS document and replaces the key set.
func (r *RemoteKeys) Refresh(ctx context.Context) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, r.URL, nil)
	if err != nil {
		return err
	}
	c := r.Client
	if c == nil {
		c = &http.Client{Timeout: 5 * time.Second}
	}
	res, err := c.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		return fmt.Errorf("authn: jwks fetch: HTTP %d", res.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(res.Body, 1<<20))
	if err != nil {
		return err
	}
	keys, err := ParseJWKS(body)
	if err != nil {
		return err
	}
	r.Keys.Replace(keys)
	return nil
}

type ctxKey struct{}

type errKey struct{}

// WithPrincipal stores p in ctx.
func WithPrincipal(ctx context.Context, p *Principal) context.Context {
	return context.WithValue(ctx, ctxKey{}, p)
}

// FromContext returns the principal stored by Middleware, if any.
func FromContext(ctx context.Context) (*Principal, bool) {
	p, ok := ctx.Value(ctxKey{}).(*Principal)
	return p, ok
}

// ErrorFromContext returns why a presented bearer token was rejected (lenient middleware).
func ErrorFromContext(ctx context.Context) error {
	err, _ := ctx.Value(errKey{}).(error)
	return err
}

// BearerToken extracts the token from an "Authorization: Bearer ..." header.
func BearerToken(r *http.Request) (string, bool) {
	h := r.Header.Get("Authorization")
	const prefix = "bearer "
	if len(h) <= len(prefix) || !strings.EqualFold(h[:len(prefix)], prefix) {
		return "", false
	}
	return strings.TrimSpace(h[len(prefix):]), true
}

// Middleware verifies the bearer token when present and stores the principal in the request
// context; handlers decide whether authentication is required (see FromContext). With a non-nil
// onError, a present but invalid token is rejected right here. With onError nil the middleware
// is lenient: the request continues anonymously and the reason is available from
// ErrorFromContext, so public calls (Login, Refresh) still work when a client sends a stale
// token and protected calls can explain the rejection.
func Middleware(v *Verifier, onError func(w http.ResponseWriter, r *http.Request, err error)) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			tok, ok := BearerToken(r)
			if !ok {
				next.ServeHTTP(w, r)
				return
			}
			p, err := v.Verify(tok)
			if err != nil {
				if onError != nil {
					onError(w, r, err)
					return
				}
				next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), errKey{}, err)))
				return
			}
			next.ServeHTTP(w, r.WithContext(WithPrincipal(r.Context(), p)))
		})
	}
}
