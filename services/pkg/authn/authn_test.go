package authn

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
)

func fixture(t *testing.T) (*Issuer, *Verifier, *clock.Fake) {
	t.Helper()
	fc := clock.NewFake(time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC))
	key, err := GenerateSigningKey(bytes.NewReader(bytes.Repeat([]byte{1}, 32)))
	if err != nil {
		t.Fatal(err)
	}
	iss := NewIssuer(key, "helios-identity", "helios", 10*time.Minute, fc)
	ver := NewVerifier(NewKeySet(iss.PublicKey()), "helios-identity", "helios", fc)
	return iss, ver, fc
}

func TestIssueVerify(t *testing.T) {
	iss, ver, fc := fixture(t)
	tok, exp, err := iss.Issue(1234, "Ada#0042", "fam1", []string{ScopeGame})
	if err != nil {
		t.Fatal(err)
	}
	if !exp.Equal(fc.Now().Add(10 * time.Minute)) {
		t.Fatalf("exp %v", exp)
	}
	p, err := ver.Verify(tok)
	if err != nil {
		t.Fatal(err)
	}
	if p.AccountID != 1234 || p.Handle != "Ada#0042" || p.Family != "fam1" || !p.HasScope(ScopeGame) || p.HasScope(ScopeAdmin) {
		t.Fatalf("principal %+v", p)
	}
	// Header carries the RFC 7638 kid.
	parsed, _, _ := jwt.NewParser().ParseUnverified(tok, &Claims{})
	if parsed.Header["kid"] != iss.KeyID() || parsed.Header["alg"] != "EdDSA" {
		t.Fatalf("header %v", parsed.Header)
	}

	fc.Advance(10*time.Minute + 6*time.Second) // past exp + 5 s leeway
	if _, err := ver.Verify(tok); !errors.Is(err, ErrExpiredToken) {
		t.Fatalf("expired token: %v", err)
	}
}

func TestVerifyRejects(t *testing.T) {
	iss, ver, fc := fixture(t)
	good, _, _ := iss.Issue(1, "", "", nil)

	other, _ := GenerateSigningKey(nil)
	foreign, _, _ := NewIssuer(other, "helios-identity", "helios", time.Minute, fc).Issue(1, "", "", nil)
	wrongIss, _, _ := NewIssuer(iss.key, "someone-else", "helios", time.Minute, fc).Issue(1, "", "", nil)
	wrongAud, _, _ := NewIssuer(iss.key, "helios-identity", "other", time.Minute, fc).Issue(1, "", "", nil)
	badSub, _, _ := iss.Issue(0, "", "", nil)

	// alg=none and HS256-with-public-key confusion must fail.
	none := jwt.NewWithClaims(jwt.SigningMethodNone, jwt.RegisteredClaims{Subject: "1", Issuer: "helios-identity",
		Audience: jwt.ClaimStrings{"helios"}, ExpiresAt: jwt.NewNumericDate(fc.Now().Add(time.Minute))})
	noneTok, _ := none.SignedString(jwt.UnsafeAllowNoneSignatureType)
	hs := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.RegisteredClaims{Subject: "1", Issuer: "helios-identity",
		Audience: jwt.ClaimStrings{"helios"}, ExpiresAt: jwt.NewNumericDate(fc.Now().Add(time.Minute))})
	hs.Header["kid"] = iss.KeyID()
	hsTok, _ := hs.SignedString([]byte(iss.PublicKey()))

	tampered := good[:len(good)-4] + "AAAA"
	for name, tok := range map[string]string{
		"foreign key": foreign, "issuer": wrongIss, "audience": wrongAud, "subject": badSub,
		"alg none": noneTok, "hs256 confusion": hsTok, "tampered": tampered, "garbage": "a.b.c",
	} {
		if _, err := ver.Verify(tok); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	// Not yet valid (clock behind by more than leeway).
	fc.Advance(-time.Minute)
	if _, err := ver.Verify(good); err == nil {
		t.Error("token from the future accepted")
	}
}

func TestJWKSRoundTripAndRemote(t *testing.T) {
	iss, _, fc := fixture(t)
	ks := NewKeySet(iss.PublicKey())
	doc := ks.JWKS()
	if len(doc.Keys) != 1 || doc.Keys[0].KeyID != iss.KeyID() || doc.Keys[0].Algorithm != "EdDSA" {
		t.Fatalf("jwks %+v", doc)
	}
	b, _ := json.Marshal(doc)
	keys, err := ParseJWKS(b)
	if err != nil || !bytes.Equal(keys[iss.KeyID()], iss.PublicKey()) {
		t.Fatalf("parse: %v", err)
	}
	if _, err := ParseJWKS([]byte(`{"keys":[{"kty":"OKP","crv":"Ed25519","x":"!!"}]}`)); err == nil {
		t.Fatal("bad key accepted")
	}
	// Non-Ed25519 entries are skipped, not fatal.
	if k, err := ParseJWKS([]byte(`{"keys":[{"kty":"RSA","n":"x","e":"AQAB"}]}`)); err != nil || len(k) != 0 {
		t.Fatalf("rsa: %v %v", k, err)
	}

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(b)
	}))
	defer srv.Close()
	remote := &RemoteKeys{URL: srv.URL, Keys: NewKeySet()}
	if err := remote.Refresh(context.Background()); err != nil {
		t.Fatal(err)
	}
	tok, _, _ := iss.Issue(77, "", "", nil)
	if p, err := NewVerifier(remote.Keys, "helios-identity", "helios", fc).Verify(tok); err != nil || p.AccountID != 77 {
		t.Fatalf("remote verify: %v", err)
	}
	bad := &RemoteKeys{URL: srv.URL + "/missing", Keys: NewKeySet(), Client: &http.Client{Timeout: time.Second}}
	srv.Config.Handler = http.NotFoundHandler()
	if err := bad.Refresh(context.Background()); err == nil {
		t.Fatal("404 accepted")
	}
}

func TestThumbprintRFC7638(t *testing.T) {
	// RFC 8037 appendix A.3 test vector for Ed25519.
	pub := ed25519.NewKeyFromSeed(mustHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")).Public().(ed25519.PublicKey)
	if got := Thumbprint(pub); got != "kPrK_qmxVWaYVA9wwBF6Iuo3vVzz7TxHCTwXBygrS4k" {
		t.Fatalf("thumbprint %s", got)
	}
}

func mustHex(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		panic(err)
	}
	return b
}

func TestMiddleware(t *testing.T) {
	iss, ver, _ := fixture(t)
	tok, _, _ := iss.Issue(5, "", "", []string{ScopeGame})
	var got *Principal
	h := Middleware(ver, func(w http.ResponseWriter, _ *http.Request, err error) {
		http.Error(w, err.Error(), http.StatusUnauthorized)
	})(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got, _ = FromContext(r.Context())
	}))

	for _, tc := range []struct {
		header string
		status int
		acct   int64
	}{
		{"", 200, 0},
		{"Bearer " + tok, 200, 5},
		{"bearer " + tok, 200, 5},
		{"Bearer nope", 401, 0},
		{"Basic abc", 200, 0},
	} {
		got = nil
		r := httptest.NewRequest(http.MethodPost, "/", nil)
		if tc.header != "" {
			r.Header.Set("Authorization", tc.header)
		}
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != tc.status {
			t.Errorf("%q: status %d", tc.header, w.Code)
		}
		if tc.acct != 0 && (got == nil || got.AccountID != tc.acct) {
			t.Errorf("%q: principal %+v", tc.header, got)
		}
		if tc.acct == 0 && got != nil {
			t.Errorf("%q: unexpected principal", tc.header)
		}
	}
	if _, ok := BearerToken(httptest.NewRequest(http.MethodGet, "/", strings.NewReader(""))); ok {
		t.Fatal("no header")
	}

	// Lenient mode: invalid tokens pass through anonymously with the reason attached.
	var sawErr error
	lenient := Middleware(ver, nil)(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got, _ = FromContext(r.Context())
		sawErr = ErrorFromContext(r.Context())
	}))
	got = nil
	r := httptest.NewRequest(http.MethodPost, "/", nil)
	r.Header.Set("Authorization", "Bearer nope")
	w := httptest.NewRecorder()
	lenient.ServeHTTP(w, r)
	if w.Code != 200 || got != nil || sawErr == nil {
		t.Fatalf("lenient: %d %v %v", w.Code, got, sawErr)
	}
}
