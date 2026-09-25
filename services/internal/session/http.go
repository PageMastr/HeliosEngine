package session

import (
	"context"
	"net/http"
	"net/netip"

	"github.com/go-chi/chi/v5"

	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/authn"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// ServicePath is the Connect-style route prefix of the session API.
const ServicePath = "/helios.session.v1.Session/"

func principal(ctx context.Context) (*authn.Principal, error) {
	p, ok := authn.FromContext(ctx)
	if ok {
		return p, nil
	}
	if authn.ErrorFromContext(ctx) != nil {
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "invalid or expired access token")
	}
	return nil, rpc.Errorf(rpc.CodeUnauthenticated, "missing bearer token")
}

// Mount registers the public routes. The bearer middleware must already run on r.
func (s *Service) Mount(r chi.Router, trusted []netip.Prefix) {
	opts := rpc.UnaryOptions{Logger: s.log}
	r.Post(ServicePath+"CreateSession", rpc.Unary(func(ctx context.Context, _ *http.Request, req *CreateSessionRequest) (*ConnectInfo, error) {
		p, err := principal(ctx)
		if err != nil {
			return nil, err
		}
		return s.CreateSession(ctx, p, req)
	}, opts))
	r.Post(ServicePath+"Reconnect", rpc.Unary(func(ctx context.Context, hr *http.Request, req *ReconnectRequest) (*ConnectInfo, error) {
		ip := platform.ClientIP(hr, trusted)
		key := ""
		if ip.IsValid() {
			key = ip.String()
		}
		return s.Reconnect(ctx, key, req)
	}, opts))
	r.Post(ServicePath+"EndSession", rpc.Unary(func(ctx context.Context, _ *http.Request, _ *Empty) (*Empty, error) {
		p, err := principal(ctx)
		if err != nil {
			return nil, err
		}
		return s.EndSession(ctx, p)
	}, opts))
}
