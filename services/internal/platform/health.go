package platform

import (
	"context"
	"encoding/json"
	"net/http"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

// CheckFunc probes one dependency; it must honour ctx and return quickly.
type CheckFunc func(ctx context.Context) error

// Health serves liveness (/healthz) and readiness (/readyz). Liveness only says the process is
// responsive; readiness runs every registered check and is false during startup and draining,
// so load balancers stop routing before shutdown. Safe for concurrent use.
type Health struct {
	mu      sync.RWMutex
	checks  map[string]CheckFunc
	ready   atomic.Bool
	timeout time.Duration
}

// NewHealth returns a registry whose readiness checks time out after timeout.
func NewHealth(timeout time.Duration) *Health {
	if timeout <= 0 {
		timeout = 2 * time.Second
	}
	return &Health{checks: map[string]CheckFunc{}, timeout: timeout}
}

// AddCheck registers a readiness check under name (replacing an existing one).
func (h *Health) AddCheck(name string, fn CheckFunc) {
	h.mu.Lock()
	h.checks[name] = fn
	h.mu.Unlock()
}

// SetReady flips the readiness gate (true after startup, false when draining).
func (h *Health) SetReady(ready bool) { h.ready.Store(ready) }

// CheckResult is one entry of a readiness report.
type CheckResult struct {
	Name  string `json:"name"`
	OK    bool   `json:"ok"`
	Error string `json:"error,omitempty"`
}

// Report runs all checks concurrently and reports overall readiness.
func (h *Health) Report(ctx context.Context) (bool, []CheckResult) {
	h.mu.RLock()
	names := make([]string, 0, len(h.checks))
	for n := range h.checks {
		names = append(names, n)
	}
	checks := make([]CheckFunc, len(names))
	sort.Strings(names)
	for i, n := range names {
		checks[i] = h.checks[n]
	}
	h.mu.RUnlock()

	ctx, cancel := context.WithTimeout(ctx, h.timeout)
	defer cancel()
	results := make([]CheckResult, len(names))
	var wg sync.WaitGroup
	for i := range names {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			results[i] = CheckResult{Name: names[i], OK: true}
			if err := checks[i](ctx); err != nil {
				results[i] = CheckResult{Name: names[i], Error: err.Error()}
			}
		}(i)
	}
	wg.Wait()
	ok := h.ready.Load()
	for _, r := range results {
		ok = ok && r.OK
	}
	return ok, results
}

// LiveHandler serves /healthz.
func (h *Health) LiveHandler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		_, _ = w.Write([]byte("ok\n"))
	})
}

// ReadyHandler serves /readyz with a JSON report (503 when not ready).
func (h *Health) ReadyHandler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ok, results := h.Report(r.Context())
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		if !ok {
			w.WriteHeader(http.StatusServiceUnavailable)
		}
		_ = json.NewEncoder(w).Encode(struct {
			Ready  bool          `json:"ready"`
			Checks []CheckResult `json:"checks"`
		}{ok, results})
	})
}
