package platform

import (
	"bytes"
	"context"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus/testutil"
)

func env(m map[string]string) func(string) string { return func(k string) string { return m[k] } }

func TestConfigDefaultsValidate(t *testing.T) {
	cfg, rest, err := Load("t", nil, env(nil), io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if len(rest) != 0 || cfg.HTTP.Addr != "127.0.0.1:7700" || cfg.DB.URL != "embedded" || cfg.ConfigFile != "" {
		t.Fatalf("defaults: %+v", cfg)
	}
	if !cfg.Enabled(ServiceIdentity) || !cfg.Enabled(ServiceOrchestrator) {
		t.Fatal("all services enabled by default")
	}
	if id, err := cfg.ProtocolIDValue(); err != nil || id != 0x48454c494f530001 {
		t.Fatalf("protocol id %x %v", id, err)
	}
	if cfg.PGCacheDir() == "" {
		t.Fatal("pg cache dir")
	}
}

func TestConfigPrecedence(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "helios.toml")
	body := `
shard = "eu1"
services = ["identity", "session"]
[http]
addr = "127.0.0.1:9000"
[log]
level = "debug"
[session]
token_expiry = "40s"
gateways = ["10.0.0.1:7777", "10.0.0.2:7777"]
[[orchestrator.zones]]
id = 5
name = "hangar"
[[orchestrator.spawn]]
name = "cell-a"
exe = "helios-cellserver"
args = ["--zone", "hangar"]
restart = "on-failure"
backoff_min = "100ms"
`
	if err := os.WriteFile(file, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	// The file is found through the data dir; env overrides the file; flags override env.
	cfg, rest, err := Load("t", []string{"--data", dir, "--http", "127.0.0.1:9100", "--lan", "run", "extra"},
		env(map[string]string{"HELIOS_HTTP": "127.0.0.1:9050", "HELIOS_LOG_LEVEL": "warn", "HELIOS_SHARD": "us2"}), io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.ConfigFile != file {
		t.Fatalf("config file %q", cfg.ConfigFile)
	}
	if cfg.Shard != "us2" || cfg.Log.Level != "warn" {
		t.Fatalf("env must beat file: %q %q", cfg.Shard, cfg.Log.Level)
	}
	if cfg.HTTP.Addr != "0.0.0.0:9100" {
		t.Fatalf("flag must beat env and --lan must widen loopback: %q", cfg.HTTP.Addr)
	}
	if cfg.Bus.Listen != "0.0.0.0:4222" {
		t.Fatalf("lan nats listen: %q", cfg.Bus.Listen)
	}
	if cfg.Session.TokenExpiry.D() != 40*time.Second || len(cfg.Session.Gateways) != 2 {
		t.Fatalf("session from file: %+v", cfg.Session)
	}
	if len(cfg.Orchestrator.Zones) != 1 || cfg.Orchestrator.Zones[0].Name != "hangar" ||
		cfg.Orchestrator.Spawn[0].BackoffMin.D() != 100*time.Millisecond {
		t.Fatalf("orchestrator from file: %+v", cfg.Orchestrator)
	}
	if cfg.Enabled(ServiceOrchestrator) || !cfg.Enabled(ServiceSession) {
		t.Fatal("services subset")
	}
	if strings.Join(rest, " ") != "run extra" {
		t.Fatalf("rest %v", rest)
	}
	// Explicit --config, and the data-dir alias.
	cfg, _, err = Load("t", []string{"--config", file, "--data-dir", t.TempDir()}, env(nil), io.Discard)
	if err != nil || cfg.Shard != "eu1" {
		t.Fatalf("explicit config: %v", err)
	}
}

func TestConfigErrors(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.toml")
	_ = os.WriteFile(bad, []byte("shard = \"x\"\ntypo_key = 1\n"), 0o600)
	if _, _, err := Load("t", []string{"--config", bad}, env(nil), io.Discard); err == nil || !strings.Contains(err.Error(), "typo_key") {
		t.Fatalf("unknown key must be reported: %v", err)
	}
	if _, _, err := Load("t", []string{"--config", filepath.Join(dir, "missing.toml")}, env(nil), io.Discard); err == nil {
		t.Fatal("missing explicit config accepted")
	}
	cases := [][]string{
		{"--shard", "EU1"},
		{"--shard-index", "32"},
		{"--node", "3"}, // removed: block IDs need no node (05 §1.4.5)
		{"--services", "identity,ledger"},
		{"--db", "mysql://x"},
		{"--cache", "memcached://x"},
		{"--bus", "amqp://x"},
		{"--env", "staging"},
		{"--seed", "dev", "--env", "prod"},
		{"--log-level", "loud"},
		{"--http", "nope"},
		{"--tls-cert", "a.pem"},
		{"--traces", "jaeger"},
		{"--pg-port", "abc"},
		{"--lan=maybe"},
		{"--unknown-flag"},
	}
	for _, args := range cases {
		if _, _, err := Load("t", args, env(nil), io.Discard); err == nil {
			t.Errorf("%v accepted", args)
		}
	}
	if _, _, err := Load("t", nil, env(map[string]string{"HELIOS_SHARD_INDEX": "x"}), io.Discard); err == nil {
		t.Error("bad env accepted")
	}

	cfg := Default()
	cfg.Session.TokenExpiry = Duration(60 * time.Second)
	cfg.Session.Gateways = []string{"gateway.example:7777"} // tokens need literal IPs
	cfg.Orchestrator.Zones = append(cfg.Orchestrator.Zones, cfg.Orchestrator.Zones[0],
		ZoneConfig{ID: 99, Name: strings.Repeat("z", MaxZoneName+1)}, ZoneConfig{ID: 98, Name: "nul\x00"})
	cfg.Orchestrator.Spawn = []SpawnConfig{{Name: "x", Exe: "y", Restart: "sometimes"}}
	cfg.Orchestrator.HeartbeatInterval = Duration(7 * time.Second) // more than half the 12 s liveness TTL
	cfg.Session.ProtocolID = "zz"
	err := cfg.Validate()
	for _, want := range []string{"token_expiry", "gateway.example", "unique", "sometimes", "lease_ttl", "protocol_id",
		"zone 99: names are at most", "zone 98: names are at most"} {
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("Validate should mention %q: %v", want, err)
		}
	}
}

func TestLogger(t *testing.T) {
	var buf bytes.Buffer
	log := NewLogger(LogConfig{Level: "warn", Format: "json"}, "eu1", &buf)
	log.Info("hidden")
	Service(log, "identity").Warn("shown", "k", 1)
	out := buf.String()
	if strings.Contains(out, "hidden") || !strings.Contains(out, `"svc":"identity"`) || !strings.Contains(out, `"shard":"eu1"`) {
		t.Fatalf("log output %q", out)
	}
	for in, want := range map[string]slog.Level{"debug": slog.LevelDebug, "WARNING": slog.LevelWarn, "error": slog.LevelError, "x": slog.LevelInfo} {
		if ParseLevel(in) != want {
			t.Errorf("ParseLevel(%q)", in)
		}
	}
	buf.Reset()
	NewLogger(LogConfig{Level: "info", Format: "text"}, "dev", &buf).Info("hello")
	if !strings.Contains(buf.String(), "msg=hello") {
		t.Fatalf("text %q", buf.String())
	}
}

func TestHealth(t *testing.T) {
	h := NewHealth(time.Second)
	h.AddCheck("db", func(context.Context) error { return nil })
	h.AddCheck("cache", func(context.Context) error { return errors.New("down") })

	w := httptest.NewRecorder()
	h.LiveHandler().ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	if w.Code != 200 {
		t.Fatal("liveness")
	}
	h.SetReady(true)
	w = httptest.NewRecorder()
	h.ReadyHandler().ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/readyz", nil))
	if w.Code != 503 || !strings.Contains(w.Body.String(), `"error":"down"`) {
		t.Fatalf("failing check: %d %s", w.Code, w.Body)
	}
	h.AddCheck("cache", func(context.Context) error { return nil })
	if ok, res := h.Report(context.Background()); !ok || len(res) != 2 || res[0].Name != "cache" {
		t.Fatalf("report %v %+v", ok, res)
	}
	h.SetReady(false) // draining
	if ok, _ := h.Report(context.Background()); ok {
		t.Fatal("not ready while draining")
	}
	// A hung check is bounded by the timeout.
	slow := NewHealth(50 * time.Millisecond)
	slow.SetReady(true)
	slow.AddCheck("hang", func(ctx context.Context) error { <-ctx.Done(); return ctx.Err() })
	if ok, _ := slow.Report(context.Background()); ok {
		t.Fatal("hung check must fail")
	}
}

func TestHTTPMetricsMiddleware(t *testing.T) {
	reg := NewRegistry()
	m := NewHTTPMetrics(reg, "api")
	r := chi.NewRouter()
	r.Use(m.Middleware)
	r.Get("/items/{id}", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(204) })
	for _, p := range []string{"/items/1", "/items/2", "/nope"} {
		r.ServeHTTP(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, p, nil))
	}
	if got := testutil.ToFloat64(m.requests.WithLabelValues("/items/{id}", "204")); got != 2 {
		t.Fatalf("route counter %v", got)
	}
	if got := testutil.ToFloat64(m.requests.WithLabelValues("unmatched", "404")); got != 1 {
		t.Fatalf("unmatched counter %v", got)
	}
	w := httptest.NewRecorder()
	MetricsHandler(reg).ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	if !strings.Contains(w.Body.String(), "helios_http_requests_total") || !strings.Contains(w.Body.String(), "go_goroutines") {
		t.Fatal("metrics exposition")
	}
}

func TestClientIP(t *testing.T) {
	trusted, err := ParseTrustedProxies([]string{"10.0.0.0/8"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := ParseTrustedProxies([]string{"nope"}); err == nil {
		t.Fatal("bad cidr")
	}
	req := func(remote, xff string) *http.Request {
		r := httptest.NewRequest(http.MethodGet, "/", nil)
		r.RemoteAddr = remote
		if xff != "" {
			r.Header.Set("X-Forwarded-For", xff)
		}
		return r
	}
	cases := []struct{ remote, xff, want string }{
		{"203.0.113.5:4000", "", "203.0.113.5"},
		{"203.0.113.5:4000", "1.2.3.4", "203.0.113.5"},                     // untrusted peer: header ignored
		{"10.1.1.1:80", "198.51.100.7", "198.51.100.7"},                    // trusted proxy
		{"10.1.1.1:80", "6.6.6.6, 198.51.100.7, 10.2.2.2", "198.51.100.7"}, // spoofed prefix ignored
		{"10.1.1.1:80", "garbage", "10.1.1.1"},
		{"[::ffff:192.0.2.1]:5", "", "192.0.2.1"},
	}
	for _, c := range cases {
		if got := ClientIP(req(c.remote, c.xff), trusted); got != netip.MustParseAddr(c.want) {
			t.Errorf("%s / %q: got %v want %s", c.remote, c.xff, got, c.want)
		}
	}
	// Regression: a proxy that appends its own X-Forwarded-For line (instead of extending the
	// client's) must not let the client's forged first line decide the address.
	r := req("10.1.1.1:80", "")
	r.Header.Add("X-Forwarded-For", "6.6.6.6")      // forged by the client
	r.Header.Add("X-Forwarded-For", "198.51.100.7") // appended by the trusted proxy
	if got := ClientIP(r, trusted); got != netip.MustParseAddr("198.51.100.7") {
		t.Errorf("multi-line X-Forwarded-For: got %v", got)
	}
}

func TestServerStartShutdown(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	s := NewServer("test", "127.0.0.1:0", http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("hi"))
	}), "", "", log)
	if err := s.Start(); err != nil {
		t.Fatal(err)
	}
	res, err := http.Get(s.URL())
	if err != nil {
		t.Fatal(err)
	}
	b, _ := io.ReadAll(res.Body)
	res.Body.Close()
	if string(b) != "hi" {
		t.Fatalf("body %q", b)
	}
	// A second server on the same port fails synchronously.
	if err := NewServer("dup", s.Addr().String(), http.NotFoundHandler(), "", "", log).Start(); err == nil {
		t.Fatal("port reuse accepted")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := s.Shutdown(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := http.Get(s.URL()); err == nil {
		t.Fatal("still serving after shutdown")
	}
}

func TestTelemetrySetup(t *testing.T) {
	var buf bytes.Buffer
	tel, err := SetupTelemetry(TelemetryConfig{Traces: "stdout", SampleRatio: 1}, "helios-test", "0", &buf)
	if err != nil {
		t.Fatal(err)
	}
	h := TraceHTTP(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {}), "op")
	h.ServeHTTP(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, "/", nil))
	if err := tel.Shutdown(context.Background()); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(buf.String(), "span name=") || !strings.Contains(buf.String(), "trace=") {
		t.Fatalf("span not exported: %q", buf.String())
	}
	if _, err := SetupTelemetry(TelemetryConfig{Traces: "zipkin"}, "x", "0", io.Discard); err == nil {
		t.Fatal("unknown exporter accepted")
	}
	none, err := SetupTelemetry(TelemetryConfig{Traces: "none", SampleRatio: 1}, "x", "0", io.Discard)
	if err != nil || none.Shutdown(context.Background()) != nil {
		t.Fatal("none exporter")
	}
}

// TestExampleConfigLoads keeps deploy/helios.example.toml in sync with the Config struct.
func TestExampleConfigLoads(t *testing.T) {
	cfg, _, err := Load("t", []string{"--config", filepath.Join("..", "..", "deploy", "helios.example.toml")}, env(nil), io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	def := Default()
	if cfg.Identity.Argon2 != def.Identity.Argon2 || cfg.Session.TokenExpiry != def.Session.TokenExpiry ||
		cfg.Orchestrator.Zones[0] != def.Orchestrator.Zones[0] || cfg.HTTP.Addr != def.HTTP.Addr ||
		cfg.Identity.LoginPerAccount != def.Identity.LoginPerAccount {
		t.Fatal("the example documents values that differ from the defaults")
	}
}
