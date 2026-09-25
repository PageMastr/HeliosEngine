package platform

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"net/netip"
	"strings"
	"time"
)

// Server is an HTTP(S) listener with production timeouts and graceful shutdown.
type Server struct {
	name     string
	srv      *http.Server
	ln       net.Listener
	certFile string
	keyFile  string
	log      *slog.Logger
	done     chan error
}

// NewServer prepares a listener on addr. When certFile and keyFile are set it serves HTTPS with
// TLS 1.3 only (05 §6.5); otherwise plain HTTP, which is meant for loopback dev use.
func NewServer(name, addr string, h http.Handler, certFile, keyFile string, log *slog.Logger) *Server {
	return &Server{
		name: name,
		srv: &http.Server{
			Addr:              addr,
			Handler:           h,
			ReadHeaderTimeout: 5 * time.Second,
			ReadTimeout:       15 * time.Second,
			WriteTimeout:      30 * time.Second,
			IdleTimeout:       120 * time.Second,
			MaxHeaderBytes:    64 << 10,
			TLSConfig:         &tls.Config{MinVersion: tls.VersionTLS13},
			ErrorLog:          slog.NewLogLogger(log.Handler(), slog.LevelWarn),
		},
		certFile: certFile,
		keyFile:  keyFile,
		log:      log,
		done:     make(chan error, 1),
	}
}

// Start binds the listener and serves in the background. Binding errors are returned
// synchronously so startup fails fast on a busy port.
func (s *Server) Start() error {
	ln, err := net.Listen("tcp", s.srv.Addr)
	if err != nil {
		return fmt.Errorf("%s listener on %s: %w", s.name, s.srv.Addr, err)
	}
	s.ln = ln
	scheme := "http"
	if s.certFile != "" {
		scheme = "https"
	}
	s.log.Info("listening", "listener", s.name, "url", scheme+"://"+ln.Addr().String())
	go func() {
		var err error
		if s.certFile != "" {
			err = s.srv.ServeTLS(ln, s.certFile, s.keyFile)
		} else {
			err = s.srv.Serve(ln)
		}
		if errors.Is(err, http.ErrServerClosed) {
			err = nil
		}
		s.done <- err
	}()
	return nil
}

// Addr returns the bound address (useful with port 0).
func (s *Server) Addr() net.Addr { return s.ln.Addr() }

// URL returns the base URL of the bound listener.
func (s *Server) URL() string {
	scheme := "http"
	if s.certFile != "" {
		scheme = "https"
	}
	return scheme + "://" + s.ln.Addr().String()
}

// Shutdown stops accepting connections and waits for in-flight requests until ctx expires.
func (s *Server) Shutdown(ctx context.Context) error {
	if s.ln == nil {
		return nil
	}
	err := s.srv.Shutdown(ctx)
	if serveErr := <-s.done; serveErr != nil && err == nil {
		err = serveErr
	}
	return err
}

// ParseTrustedProxies parses CIDR strings.
func ParseTrustedProxies(cidrs []string) ([]netip.Prefix, error) {
	out := make([]netip.Prefix, 0, len(cidrs))
	for _, c := range cidrs {
		p, err := netip.ParsePrefix(c)
		if err != nil {
			return nil, err
		}
		out = append(out, p.Masked())
	}
	return out, nil
}

// ClientIP returns the caller's address for rate limiting and audit. X-Forwarded-For is only
// honoured when the direct peer is a trusted proxy; the chain is then walked from the right,
// skipping trusted hops, so a client cannot spoof its address by prepending entries. Every
// X-Forwarded-For header line counts, in order: a proxy that appends its own line (instead of
// extending the client's) must not let the client's forged first line win.
func ClientIP(r *http.Request, trusted []netip.Prefix) netip.Addr {
	peer := remoteAddr(r.RemoteAddr)
	if !peer.IsValid() || !isTrusted(peer, trusted) {
		return peer
	}
	hops := strings.Split(strings.Join(r.Header.Values("X-Forwarded-For"), ","), ",")
	for i := len(hops) - 1; i >= 0; i-- {
		a, err := netip.ParseAddr(strings.TrimSpace(hops[i]))
		if err != nil {
			break
		}
		a = a.Unmap()
		if !isTrusted(a, trusted) {
			return a
		}
		peer = a
	}
	return peer
}

func remoteAddr(s string) netip.Addr {
	if ap, err := netip.ParseAddrPort(s); err == nil {
		return ap.Addr().Unmap()
	}
	if a, err := netip.ParseAddr(s); err == nil {
		return a.Unmap()
	}
	return netip.Addr{}
}

func isTrusted(a netip.Addr, trusted []netip.Prefix) bool {
	for _, p := range trusted {
		if p.Contains(a) {
			return true
		}
	}
	return false
}
