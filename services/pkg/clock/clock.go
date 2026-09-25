// Package clock abstracts wall-clock time so domain logic (leases, token expiry, rate limits)
// can be tested deterministically.
package clock

import (
	"sync"
	"time"
)

// Clock returns the current time. Implementations must be safe for concurrent use.
type Clock interface {
	Now() time.Time
}

// System is the real wall clock.
type System struct{}

// Now returns time.Now().
func (System) Now() time.Time { return time.Now() }

// Fake is a manually driven clock for tests. The zero value starts at the Unix epoch; use
// NewFake for a realistic start time.
type Fake struct {
	mu  sync.Mutex
	now time.Time
}

// NewFake returns a fake clock set to t.
func NewFake(t time.Time) *Fake { return &Fake{now: t} }

// Now returns the fake's current time.
func (f *Fake) Now() time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.now
}

// Advance moves the fake forward by d and returns the new time.
func (f *Fake) Advance(d time.Duration) time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.now = f.now.Add(d)
	return f.now
}

// Set moves the fake to t (which may be in the past, to test clock steps).
func (f *Fake) Set(t time.Time) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.now = t
}
