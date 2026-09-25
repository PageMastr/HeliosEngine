package stack

import (
	"context"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"

	"github.com/alicebob/miniredis/v2"
	"github.com/redis/go-redis/v9"

	"github.com/PageMastr/scifi-test/services/internal/platform"
)

// Cache is a Valkey client, plus the in-process miniredis when one was started.
type Cache struct {
	Client *redis.Client
	Addr   string
	// password of the embedded miniredis (random per start).
	password string
	mini     *miniredis.Miniredis
	stop     chan struct{}
	wg       sync.WaitGroup
}

// OpenCache starts miniredis (the dev stand-in for Valkey, which has no native Windows build)
// when cfg.Cache.URL is "miniredis", otherwise connects to valkey:// / redis:// URLs.
func OpenCache(ctx context.Context, cfg *platform.Config, log *slog.Logger) (*Cache, error) {
	c := &Cache{}
	if cfg.Cache.URL == "miniredis" {
		// miniredis listens on a loopback TCP port; a per-start password keeps other local
		// processes from reading or rewriting sessions and rate-limit buckets.
		password, err := randomSecret()
		if err != nil {
			return nil, err
		}
		m := miniredis.NewMiniRedis()
		m.RequireAuth(password)
		if err := m.StartAddr(cfg.Cache.Listen); err != nil {
			return nil, fmt.Errorf("miniredis on %s: %w", cfg.Cache.Listen, err)
		}
		c.mini, c.Addr, c.password = m, m.Addr(), password
		c.Client = redis.NewClient(&redis.Options{Addr: m.Addr(), Password: password, Protocol: 2})
		// miniredis only expires keys when told time has passed; drive it from the wall clock
		// so TTL semantics (launch codes, tombstones) match Valkey.
		c.stop = make(chan struct{})
		c.wg.Add(1)
		go func() {
			defer c.wg.Done()
			last := time.Now()
			t := time.NewTicker(250 * time.Millisecond)
			defer t.Stop()
			for {
				select {
				case <-c.stop:
					return
				case now := <-t.C:
					m.SetTime(now)
					m.FastForward(now.Sub(last))
					last = now
				}
			}
		}()
		log.Info("miniredis started (Valkey stand-in)", "addr", c.Addr)
		return c, nil
	}
	u := cfg.Cache.URL
	switch {
	case strings.HasPrefix(u, "valkey://"):
		u = "redis://" + strings.TrimPrefix(u, "valkey://")
	case strings.HasPrefix(u, "valkeys://"):
		u = "rediss://" + strings.TrimPrefix(u, "valkeys://")
	}
	opts, err := redis.ParseURL(u)
	if err != nil {
		return nil, fmt.Errorf("cache url: %w", err)
	}
	c.Client, c.Addr = redis.NewClient(opts), opts.Addr
	deadline := time.Now().Add(30 * time.Second)
	for {
		pctx, cancel := context.WithTimeout(ctx, 3*time.Second)
		err := c.Client.Ping(pctx).Err()
		cancel()
		if err == nil {
			return c, nil
		}
		if time.Now().After(deadline) || ctx.Err() != nil {
			_ = c.Client.Close()
			return nil, fmt.Errorf("valkey %s not reachable: %w", opts.Addr, err)
		}
		time.Sleep(500 * time.Millisecond)
	}
}

// Embedded reports whether miniredis runs in-process.
func (c *Cache) Embedded() bool { return c.mini != nil }

// Close closes the client and stops miniredis.
func (c *Cache) Close() {
	if c.Client != nil {
		_ = c.Client.Close()
	}
	if c.mini != nil {
		close(c.stop)
		c.wg.Wait()
		c.mini.Close()
	}
}
