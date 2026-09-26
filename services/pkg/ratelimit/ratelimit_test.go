package ratelimit

import (
	"bytes"
	"context"
	"log/slog"
	"strings"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/testkit"
)

func TestGCRABurstThenSustainedRate(t *testing.T) {
	r := testkit.StartRedis(t)
	fc := clock.NewFake(time.Date(2026, 9, 1, 0, 0, 0, 0, time.UTC))
	l := New(r.Client, fc, "rl:", nil)
	ctx := context.Background()
	lim := Limit{Rate: 6, Per: time.Minute, Burst: 3} // one per 10 s, 3 at once

	for i := 0; i < 3; i++ {
		res, err := l.Allow(ctx, "k", lim)
		if err != nil || !res.Allowed {
			t.Fatalf("burst request %d denied: %+v %v", i, res, err)
		}
		if res.Remaining != 2-i {
			t.Fatalf("remaining after %d = %d", i, res.Remaining)
		}
	}
	res, _ := l.Allow(ctx, "k", lim)
	if res.Allowed || res.RetryAfter != 10*time.Second {
		t.Fatalf("4th request: %+v", res)
	}
	// Denied requests do not consume capacity.
	fc.Advance(9 * time.Second)
	if res, _ := l.Allow(ctx, "k", lim); res.Allowed || res.RetryAfter != time.Second {
		t.Fatalf("after 9s: %+v", res)
	}
	fc.Advance(time.Second)
	if res, _ := l.Allow(ctx, "k", lim); !res.Allowed || res.Remaining != 0 {
		t.Fatalf("after 10s: %+v", res)
	}
	// A long idle period refills to the burst, never beyond.
	fc.Advance(time.Hour)
	for i := 0; i < 3; i++ {
		if res, _ := l.Allow(ctx, "k", lim); !res.Allowed {
			t.Fatalf("refill request %d denied", i)
		}
	}
	if res, _ := l.Allow(ctx, "k", lim); res.Allowed {
		t.Fatal("bucket overfilled")
	}
	// Keys are independent.
	if res, _ := l.Allow(ctx, "other", lim); !res.Allowed {
		t.Fatal("independent key denied")
	}
}

func TestAllowNAndDisabled(t *testing.T) {
	r := testkit.StartRedis(t)
	fc := clock.NewFake(time.Date(2026, 9, 1, 0, 0, 0, 0, time.UTC))
	l := New(r.Client, fc, "rl:", nil)
	ctx := context.Background()
	lim := Limit{Rate: 10, Per: time.Second, Burst: 5}
	if res, _ := l.AllowN(ctx, "n", lim, 5); !res.Allowed {
		t.Fatal("cost 5 within burst denied")
	}
	if res, _ := l.AllowN(ctx, "n", lim, 1); res.Allowed {
		t.Fatal("over burst allowed")
	}
	if res, _ := l.Allow(ctx, "n", Limit{}); !res.Allowed {
		t.Fatal("disabled limit must allow")
	}
	if (Limit{}).Enabled() || !lim.Enabled() || lim.String() == "" {
		t.Fatal("Enabled/String")
	}
}

func TestFailOpenAndClosed(t *testing.T) {
	dead := redis.NewClient(&redis.Options{Addr: "127.0.0.1:1", DialTimeout: 100 * time.Millisecond, MaxRetries: -1})
	defer dead.Close()
	var logs bytes.Buffer
	l := New(dead, nil, "rl:", slog.New(slog.NewTextHandler(&logs, nil)))
	ctx := context.Background()
	lim := Limit{Rate: 1, Per: time.Second, Burst: 1}
	if res, err := l.Allow(ctx, "login:ip:2001:db8::7", lim); err != nil || !res.Allowed {
		t.Fatalf("fail-open: %+v %v", res, err)
	}
	// The log names the bucket's class, not the client IP it limits.
	if out := logs.String(); !strings.Contains(out, "bucket=login:ip") || strings.Contains(out, "2001:db8") {
		t.Fatalf("fail-open log: %s", out)
	}
	l.FailOpen = false
	if _, err := l.Allow(ctx, "k", lim); err == nil {
		t.Fatal("fail-closed must surface the error")
	}
}
