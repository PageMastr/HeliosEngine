package identity

import (
	"context"
	"encoding/json"
	"net/http"
	"net/netip"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/authn"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// ServicePath is the Connect-style route prefix of the identity API.
const ServicePath = "/helios.identity.v1.Identity/"

// HTTPOptions configures the transport.
type HTTPOptions struct {
	TrustedProxies []netip.Prefix
}

// MetaFrom extracts per-request metadata (client IP honouring trusted proxies, user agent).
func MetaFrom(r *http.Request, trusted []netip.Prefix) Meta {
	ip := platform.ClientIP(r, trusted)
	s := ""
	if ip.IsValid() {
		s = ip.String()
	}
	return Meta{ClientIP: s, UserAgent: r.UserAgent()}
}

// RequirePrincipal returns the verified caller or an unauthenticated error.
func RequirePrincipal(ctx context.Context) (*authn.Principal, error) {
	p, ok := authn.FromContext(ctx)
	if ok {
		return p, nil
	}
	if authn.ErrorFromContext(ctx) != nil {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "invalid or expired access token")
	}
	return nil, rpc.Errorf(rpc.CodeUnauthenticated, "missing bearer token")
}

// Mount registers the public routes on r. The bearer middleware must already run on r (the
// backend installs authn.Middleware once for all services).
func (s *Service) Mount(r chi.Router, o HTTPOptions) {
	opts := rpc.UnaryOptions{Logger: s.log}
	meta := func(r *http.Request) Meta { return MetaFrom(r, o.TrustedProxies) }

	r.Post(ServicePath+"Register", rpc.Unary(func(ctx context.Context, hr *http.Request, req *RegisterRequest) (*RegisterResponse, error) {
		return s.Register(ctx, meta(hr), req)
	}, opts))
	r.Post(ServicePath+"Login", rpc.Unary(func(ctx context.Context, hr *http.Request, req *LoginRequest) (*TokenPair, error) {
		return s.Login(ctx, meta(hr), req)
	}, opts))
	r.Post(ServicePath+"Refresh", rpc.Unary(func(ctx context.Context, hr *http.Request, req *RefreshRequest) (*TokenPair, error) {
		return s.Refresh(ctx, meta(hr), req)
	}, opts))
	r.Post(ServicePath+"Logout", rpc.Unary(func(ctx context.Context, hr *http.Request, req *LogoutRequest) (*Empty, error) {
		return s.Logout(ctx, meta(hr), req)
	}, opts))
	r.Post(ServicePath+"GetAccount", rpc.Unary(func(ctx context.Context, _ *http.Request, _ *Empty) (*AccountInfo, error) {
		p, err := RequirePrincipal(ctx)
		if err != nil {
			return nil, err
		}
		return s.GetAccount(ctx, p)
	}, opts))
	r.Post(ServicePath+"CreateLaunchCode", rpc.Unary(func(ctx context.Context, _ *http.Request, _ *Empty) (*LaunchCode, error) {
		p, err := RequirePrincipal(ctx)
		if err != nil {
			return nil, err
		}
		return s.CreateLaunchCode(ctx, p)
	}, opts))
	r.Post(ServicePath+"ExchangeLaunchCode", rpc.Unary(func(ctx context.Context, hr *http.Request, req *ExchangeLaunchCodeRequest) (*TokenPair, error) {
		return s.ExchangeLaunchCode(ctx, meta(hr), req)
	}, opts))
	r.Get("/.well-known/jwks.json", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "public, max-age=300")
		_ = json.NewEncoder(w).Encode(s.JWKS())
	})
}

// BanRequest is the admin ban call.
type BanRequest struct {
	AccountID int64      `json:"accountId,string"`
	Until     *time.Time `json:"until"` // null lifts the ban
	Reason    string     `json:"reason"`
}

// VerifyAuditResponse reports how many chained audit rows verified.
type VerifyAuditResponse struct {
	Entries int `json:"entries"`
}

// MountAdmin registers operator routes (dev ops listener only until the GM API and its RBAC
// arrive in Phase 1, 05 §1.17).
func (s *Service) MountAdmin(r chi.Router) {
	r.Post("/admin/identity/Ban", rpc.Unary(func(ctx context.Context, _ *http.Request, req *BanRequest) (*Empty, error) {
		return &Empty{}, s.Ban(ctx, 0, req.AccountID, req.Until, req.Reason)
	}, rpc.UnaryOptions{Logger: s.log}))
	r.Post("/admin/identity/VerifyAudit", rpc.Unary(func(ctx context.Context, _ *http.Request, _ *Empty) (*VerifyAuditResponse, error) {
		n, err := s.VerifyAudit(ctx)
		if err != nil {
			return nil, rpc.Errorf(rpc.CodeInternal, "audit verification failed: %v", err)
		}
		return &VerifyAuditResponse{Entries: n}, nil
	}, rpc.UnaryOptions{Logger: s.log}))
}
