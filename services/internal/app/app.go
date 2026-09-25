// Package app defines the service lifecycle shared by every Helios Go service (05 §8) and the
// runner that starts them in dependency order and stops them in reverse.
package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"sync"
)

// Service is one backend service in the modular monolith. Start must return once the service
// is serving (long-running work goes to goroutines it owns); Stop must release everything Start
// acquired and honour ctx; Health reports readiness of the service's dependencies.
type Service interface {
	Name() string
	Start(ctx context.Context) error
	Stop(ctx context.Context) error
	Health(ctx context.Context) error
}

// Runner starts services in registration order and stops them in reverse. Safe for
// concurrent use.
type Runner struct {
	log *slog.Logger

	mu       sync.Mutex
	services []Service
	started  []Service
}

// NewRunner returns an empty runner.
func NewRunner(log *slog.Logger) *Runner {
	if log == nil {
		log = slog.Default()
	}
	return &Runner{log: log}
}

// Add registers services (before Start).
func (r *Runner) Add(s ...Service) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.services = append(r.services, s...)
}

// Services returns the registered services in start order.
func (r *Runner) Services() []Service {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]Service(nil), r.services...)
}

// Start starts every service. If one fails, the ones already started are stopped (in reverse)
// and the error is returned.
func (r *Runner) Start(ctx context.Context) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, s := range r.services {
		if err := s.Start(ctx); err != nil {
			startErr := fmt.Errorf("start %s: %w", s.Name(), err)
			return errors.Join(startErr, r.stopLocked(ctx))
		}
		r.started = append(r.started, s)
		r.log.Info("service started", "svc", s.Name())
	}
	return nil
}

// Stop stops every started service in reverse order and joins their errors.
func (r *Runner) Stop(ctx context.Context) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.stopLocked(ctx)
}

func (r *Runner) stopLocked(ctx context.Context) error {
	var errs []error
	for i := len(r.started) - 1; i >= 0; i-- {
		s := r.started[i]
		if err := s.Stop(ctx); err != nil {
			errs = append(errs, fmt.Errorf("stop %s: %w", s.Name(), err))
			r.log.Error("service stop failed", "svc", s.Name(), "err", err)
		} else {
			r.log.Info("service stopped", "svc", s.Name())
		}
	}
	r.started = nil
	return errors.Join(errs...)
}
