package stack

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
	"github.com/redis/go-redis/v9"

	"github.com/PageMastr/scifi-test/services/internal/platform"
)

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

func TestDataDirLockIsExclusiveAcrossProcesses(t *testing.T) {
	dir := t.TempDir()
	l, err := LockDataDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	// A second process must see the lock (in-process flock semantics differ per OS, so use a
	// real child: this test binary in helper mode).
	cmd := exec.Command(os.Args[0], "-test.run=TestLockHelper")
	cmd.Env = append(os.Environ(), "HELIOS_LOCK_HELPER="+dir)
	out, _ := cmd.CombinedOutput()
	if !strings.Contains(string(out), "locked-out") {
		t.Fatalf("second process was not locked out: %s", out)
	}
	if err := l.Unlock(); err != nil {
		t.Fatal(err)
	}
	cmd = exec.Command(os.Args[0], "-test.run=TestLockHelper")
	cmd.Env = append(os.Environ(), "HELIOS_LOCK_HELPER="+dir)
	out, _ = cmd.CombinedOutput()
	if !strings.Contains(string(out), "acquired") {
		t.Fatalf("lock not released: %s", out)
	}
	if err := (*DataDirLock)(nil).Unlock(); err != nil {
		t.Fatal("nil unlock")
	}
}

func TestLockHelper(t *testing.T) {
	dir := os.Getenv("HELIOS_LOCK_HELPER")
	if dir == "" {
		t.Skip("helper only")
	}
	l, err := LockDataDir(dir)
	if errors.Is(err, ErrDataDirLocked) {
		if !strings.Contains(err.Error(), "pid") {
			t.Fatalf("holder pid missing: %v", err)
		}
		t.Log("locked-out")
		os.Stdout.WriteString("locked-out\n")
		return
	}
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout.WriteString("acquired\n")
	_ = l.Unlock()
}

func testConfig(t *testing.T) *platform.Config {
	t.Helper()
	cfg := platform.Default()
	cfg.DataDir = t.TempDir()
	cfg.Bus.Listen = "127.0.0.1:0"
	cfg.Cache.Listen = "127.0.0.1:0"
	return cfg
}

func TestEmbeddedBusAndCache(t *testing.T) {
	cfg := testConfig(t)
	ctx := context.Background()
	bus, err := OpenBus(ctx, cfg, BusAuth{FleetPassword: "fleet-secret"}, quiet)
	if err != nil {
		t.Fatal(err)
	}
	if !bus.Embedded() || !strings.HasPrefix(bus.ClientURL, "nats://127.0.0.1:") || strings.HasSuffix(bus.ClientURL, ":4222") {
		t.Fatalf("client url %q (port 0 must mean random, not 4222)", bus.ClientURL)
	}
	if bus.FleetPassword != "fleet-secret" || strings.Contains(bus.ClientURL, "@") {
		t.Fatalf("credentials: %q in %q", bus.FleetPassword, bus.ClientURL)
	}
	if _, err := os.Stat(filepath.Join(cfg.DataDir, "nats")); err != nil {
		t.Fatal("jetstream store dir")
	}
	// A second client over TCP, as nats.c cells and gateways connect, with the fleet credentials
	// in the URL (the external-bus path).
	u, _ := url.Parse(bus.ClientURL)
	u.User = url.UserPassword(FleetUser, bus.FleetPassword)
	ext := *cfg
	ext.Bus.URL = u.String()
	remote, err := OpenBus(ctx, &ext, BusAuth{}, quiet)
	if err != nil {
		t.Fatal(err)
	}
	sub, err := bus.Conn.SubscribeSync("ping")
	if err != nil {
		t.Fatal(err)
	}
	_ = bus.Conn.Flush()
	if err := remote.Conn.Publish("ping", []byte("x")); err != nil {
		t.Fatal(err)
	}
	if _, err := sub.NextMsg(5 * time.Second); err != nil {
		t.Fatalf("tcp client did not reach in-process subscriber: %v", err)
	}
	remote.Close()
	bus.Close()

	cache, err := OpenCache(ctx, cfg, quiet)
	if err != nil {
		t.Fatal(err)
	}
	if !cache.Embedded() {
		t.Fatal("miniredis expected")
	}
	if err := cache.Client.Set(ctx, "k", "v", 300*time.Millisecond).Err(); err != nil {
		t.Fatal(err)
	}
	// TTLs follow the wall clock (driven every 250 ms).
	deadline := time.Now().Add(5 * time.Second)
	for cache.Client.Exists(ctx, "k").Val() == 1 {
		if time.Now().After(deadline) {
			t.Fatal("miniredis TTL never expired")
		}
		time.Sleep(50 * time.Millisecond)
	}
	// miniredis requires its per-start password: other local processes cannot touch sessions.
	anon := redis.NewClient(&redis.Options{Addr: cache.Addr, Protocol: 2})
	if err := anon.Ping(ctx).Err(); err == nil {
		t.Fatal("miniredis accepted an unauthenticated client")
	}
	_ = anon.Close()
	extCache := *cfg
	extCache.Cache.URL = "valkey://:" + cache.password + "@" + cache.Addr + "/0"
	c2, err := OpenCache(ctx, &extCache, quiet)
	if err != nil {
		t.Fatalf("valkey:// url: %v", err)
	}
	c2.Close()
	cache.Close()
}

func TestEmbeddedBusRequiresCredentialsAndConfinesTheFleet(t *testing.T) {
	cfg := testConfig(t)
	bus, err := OpenBus(context.Background(), cfg, BusAuth{}, quiet)
	if err != nil {
		t.Fatal(err)
	}
	defer bus.Close()
	if len(bus.FleetPassword) < 20 {
		t.Fatalf("generated fleet password too short: %q", bus.FleetPassword)
	}
	// Regression: the client port (0.0.0.0 with --lan) accepted anonymous connections.
	if nc, err := nats.Connect(bus.ClientURL); err == nil {
		nc.Close()
		t.Fatal("anonymous client accepted")
	}
	if nc, err := nats.Connect(bus.ClientURL, nats.UserInfo(FleetUser, "wrong")); err == nil {
		nc.Close()
		t.Fatal("wrong fleet password accepted")
	}
	perm := make(chan error, 8)
	fleet, err := nats.Connect(bus.ClientURL, nats.UserInfo(FleetUser, bus.FleetPassword),
		nats.ErrorHandler(func(_ *nats.Conn, _ *nats.Subscription, err error) { perm <- err }))
	if err != nil {
		t.Fatal(err)
	}
	defer fleet.Close()
	// The fleet calls services...
	svc, _ := bus.Conn.Subscribe("rpc.dev.session.SealReconnectTickets", func(m *nats.Msg) { _ = m.Respond([]byte("ok")) })
	defer svc.Unsubscribe()
	_ = bus.Conn.Flush()
	if m, err := fleet.Request("rpc.dev.session.SealReconnectTickets", nil, 5*time.Second); err != nil || string(m.Data) != "ok" {
		t.Fatalf("fleet request: %v", err)
	}
	// ...but cannot serve (impersonate) them, nor publish control messages.
	if _, err := fleet.QueueSubscribe("rpc.dev.session.SealReconnectTickets", "session", func(*nats.Msg) {}); err != nil {
		t.Fatal(err)
	}
	_ = fleet.Flush()
	if err := <-perm; err == nil || !strings.Contains(strings.ToLower(err.Error()), "permissions violation") {
		t.Fatalf("fleet subscribe to rpc: %v", err)
	}
	ctl, _ := bus.Conn.SubscribeSync("ctl.dev.gateway.all.kick")
	_ = bus.Conn.Flush()
	_ = fleet.Publish("ctl.dev.gateway.all.kick", []byte(`{"sessionId":"1"}`))
	_ = fleet.Flush()
	if _, err := ctl.NextMsg(300 * time.Millisecond); err == nil {
		t.Fatal("fleet published a control message")
	}
}

func TestInProcessOnlyBus(t *testing.T) {
	cfg := testConfig(t)
	cfg.Bus.Listen = ""
	bus, err := OpenBus(context.Background(), cfg, BusAuth{}, quiet)
	if err != nil {
		t.Fatal(err)
	}
	defer bus.Close()
	if bus.ClientURL != "" || !bus.Conn.IsConnected() {
		t.Fatalf("in-process only: %q", bus.ClientURL)
	}
}

func TestEmbeddedPathsAndStalePid(t *testing.T) {
	cfg := testConfig(t)
	cfg.DB.CacheDir = filepath.Join(cfg.DataDir, "cache")
	p := EmbeddedPaths(cfg)
	if p.Data != filepath.Join(cfg.DataDir, "pg") || !strings.Contains(p.Binaries, string(PGVersion)) || p.Cache != cfg.DB.CacheDir {
		t.Fatalf("paths %+v", p)
	}
	// A postmaster.pid naming a dead process is left for PostgreSQL to clean up.
	if err := os.MkdirAll(p.Data, 0o700); err != nil {
		t.Fatal(err)
	}
	dead := exec.Command(os.Args[0], "-test.run=^$")
	_ = dead.Run()
	pidLine := []byte(strconv.Itoa(dead.Process.Pid) + "\n/data\n")
	if err := os.WriteFile(filepath.Join(p.Data, "postmaster.pid"), pidLine, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := stopStalePostmaster(p, quiet); err != nil {
		t.Fatalf("dead pid: %v", err)
	}
	// A live PID with no pg_ctl available is reported, not ignored.
	live := []byte(strconv.Itoa(os.Getpid()) + "\n")
	_ = os.WriteFile(filepath.Join(p.Data, "postmaster.pid"), live, 0o600)
	if err := stopStalePostmaster(p, quiet); err == nil {
		t.Fatal("live stale postmaster ignored")
	}
}
