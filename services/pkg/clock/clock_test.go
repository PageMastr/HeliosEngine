package clock

import (
	"testing"
	"time"
)

func TestFake(t *testing.T) {
	start := time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC)
	f := NewFake(start)
	if !f.Now().Equal(start) {
		t.Fatal("start")
	}
	if got := f.Advance(1500 * time.Millisecond); !got.Equal(start.Add(1500 * time.Millisecond)) {
		t.Fatalf("advance %v", got)
	}
	f.Set(start.Add(-time.Hour))
	if !f.Now().Equal(start.Add(-time.Hour)) {
		t.Fatal("set")
	}
	var c Clock = System{}
	if time.Since(c.Now()) > time.Minute {
		t.Fatal("system clock")
	}
}
