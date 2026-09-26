// Package ratelimit implements GCRA (generic cell rate algorithm) buckets in Valkey with one Lua
// script (05 §3.4, §6.5), so every backend replica shares the same limits. miniredis runs the
// same script in dev and tests.
//
// A Limit allows Burst requests at once and then Rate requests per Per on average. Buckets
// need a single key each and expire on their own once idle.
package ratelimit

import (
	"context"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/PageMastr/scifi-test/services/pkg/clock"
)

// Limit describes one bucket shape.
type Limit struct {
	Rate  int           // requests per Per (sustained)
	Per   time.Duration // window for Rate
	Burst int           // requests allowed back-to-back from an idle bucket (≥ 1)
}

// Enabled reports whether the limit is configured (Rate > 0).
func (l Limit) Enabled() bool { return l.Rate > 0 && l.Per > 0 }

// String renders the limit for logs.
func (l Limit) String() string { return fmt.Sprintf("%d/%s burst %d", l.Rate, l.Per, l.Burst) }

// Result is the outcome of one Allow call.
type Result struct {
	Allowed    bool
	Remaining  int           // further requests allowed right now
	RetryAfter time.Duration // when denied: time until one request would be allowed
}

// gcra: KEYS[1] bucket; ARGV now_us, interval_us (T), burst, cost.
// The stored value is the theoretical arrival time (TAT). A request is allowed when the new TAT
// is at most burst*T ahead of now. Times are passed in (not TIME) so tests are deterministic and
// the script stays replication-safe.
const gcraScript = `
local now = tonumber(ARGV[1])
local interval = tonumber(ARGV[2])
local burst = tonumber(ARGV[3])
local cost = tonumber(ARGV[4])
local tat = tonumber(redis.call('GET', KEYS[1]) or '0')
if tat < now then tat = now end
local new_tat = tat + interval * cost
local limit = interval * burst
local ahead = new_tat - now
if ahead > limit then
  return {0, 0, ahead - limit}
end
local ttl_ms = math.ceil(ahead / 1000)
if ttl_ms < 1 then ttl_ms = 1 end
redis.call('SET', KEYS[1], string.format('%.0f', new_tat), 'PX', ttl_ms)
return {1, math.floor((limit - ahead) / interval), 0}
`

var script = redis.NewScript(gcraScript)

// Limiter evaluates buckets stored under Prefix in Valkey. Safe for concurrent use.
type Limiter struct {
	rdb    redis.Scripter
	clk    clock.Clock
	prefix string
	log    *slog.Logger
	// FailOpen allows requests when Valkey is unreachable (default true): Valkey is never the
	// truth (05 §3.4), and the argon2 semaphore still bounds CPU during an outage.
	FailOpen bool
}

// New returns a limiter. prefix namespaces the keys (e.g. "rl:").
func New(rdb redis.Scripter, clk clock.Clock, prefix string, log *slog.Logger) *Limiter {
	if clk == nil {
		clk = clock.System{}
	}
	if log == nil {
		log = slog.Default()
	}
	return &Limiter{rdb: rdb, clk: clk, prefix: prefix, log: log, FailOpen: true}
}

// Allow charges one request against key under lim.
func (l *Limiter) Allow(ctx context.Context, key string, lim Limit) (Result, error) {
	return l.AllowN(ctx, key, lim, 1)
}

// keyClass is key without its last part, which names the client (an IP address, an account, a
// blind index): logs carry the bucket's class, never whom it limits. Keys are
// "<scope>:<kind>:<id>", and an IPv6 ID holds colons of its own.
func keyClass(key string) string {
	parts := strings.SplitN(key, ":", 3)
	if len(parts) < 3 {
		return parts[0]
	}
	return parts[0] + ":" + parts[1]
}

// AllowN charges n requests.
func (l *Limiter) AllowN(ctx context.Context, key string, lim Limit, n int) (Result, error) {
	if !lim.Enabled() {
		return Result{Allowed: true, Remaining: 1 << 30}, nil
	}
	burst := lim.Burst
	if burst < 1 {
		burst = 1
	}
	interval := lim.Per.Microseconds() / int64(lim.Rate)
	if interval < 1 {
		interval = 1
	}
	now := l.clk.Now().UnixMicro()
	vals, err := script.Run(ctx, l.rdb, []string{l.prefix + key}, now, interval, burst, n).Int64Slice()
	if err != nil {
		if l.FailOpen {
			l.log.WarnContext(ctx, "rate limiter unavailable; failing open", "bucket", keyClass(key), "err", err)
			return Result{Allowed: true}, nil
		}
		return Result{}, err
	}
	if len(vals) != 3 {
		return Result{}, fmt.Errorf("ratelimit: unexpected script reply %v", vals)
	}
	return Result{
		Allowed:    vals[0] == 1,
		Remaining:  int(vals[1]),
		RetryAfter: time.Duration(vals[2]) * time.Microsecond,
	}, nil
}
