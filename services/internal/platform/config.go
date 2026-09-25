// Package platform holds the cross-cutting plumbing every Helios Go service shares: layered
// configuration (defaults < TOML file < HELIOS_* environment < flags), structured logging (slog),
// OpenTelemetry hooks, health/readiness probes, Prometheus metrics and HTTP servers with graceful
// shutdown.
package platform

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/pelletier/go-toml/v2"
)

// Duration is a time.Duration that reads and writes as a Go duration string ("1m30s") in TOML.
type Duration time.Duration

// D returns the value as a time.Duration.
func (d Duration) D() time.Duration { return time.Duration(d) }

// UnmarshalText parses a duration string.
func (d *Duration) UnmarshalText(b []byte) error {
	v, err := time.ParseDuration(string(b))
	if err != nil {
		return err
	}
	*d = Duration(v)
	return nil
}

// MarshalText renders the duration string.
func (d Duration) MarshalText() ([]byte, error) { return []byte(time.Duration(d).String()), nil }

// RateLimit configures one GCRA bucket (see pkg/ratelimit). Rate 0 disables it.
type RateLimit struct {
	Rate  int      `toml:"rate"`
	Per   Duration `toml:"per"`
	Burst int      `toml:"burst"`
}

// Config is the complete helios-backend configuration.
type Config struct {
	Env        string   `toml:"env"`         // "dev" or "prod"; dev enables seeds and /admin on the ops port
	DataDir    string   `toml:"data_dir"`    // all local state: pg/, nats/, keys/, logs/
	Shard      string   `toml:"shard"`       // subject scope, e.g. "dev" or "eu1" (05 §2.2)
	ShardIndex int      `toml:"shard_index"` // 0..31, the shard field of block IDs (05 §1.4.5)
	Services   []string `toml:"services"`    // "all" or a subset of identity, session, orchestrator
	Seed       string   `toml:"seed"`        // "" or "dev" (dev1..dev10, password "dev")
	LAN        bool     `toml:"lan"`         // bind public listeners on all interfaces
	KeysDir    string   `toml:"keys_dir"`    // key files; "" = <data>/keys (prod: mounted from the secret store)

	Log          LogConfig          `toml:"log"`
	HTTP         HTTPConfig         `toml:"http"`
	Ops          OpsConfig          `toml:"ops"`
	DB           DBConfig           `toml:"db"`
	Bus          BusConfig          `toml:"bus"`
	Cache        CacheConfig        `toml:"cache"`
	Telemetry    TelemetryConfig    `toml:"telemetry"`
	Identity     IdentityConfig     `toml:"identity"`
	Session      SessionConfig      `toml:"session"`
	Orchestrator OrchestratorConfig `toml:"orchestrator"`

	// ConfigFile is the TOML file that was loaded ("" if none). Not read from TOML.
	ConfigFile string `toml:"-"`
}

// LogConfig configures slog.
type LogConfig struct {
	Level  string `toml:"level"`  // debug | info | warn | error
	Format string `toml:"format"` // text | json
}

// HTTPConfig is the public API listener (launcher, client, tools).
type HTTPConfig struct {
	Addr           string   `toml:"addr"`
	TLSCert        string   `toml:"tls_cert"` // PEM files; both set = HTTPS with TLS 1.3
	TLSKey         string   `toml:"tls_key"`
	TrustedProxies []string `toml:"trusted_proxies"` // CIDRs whose X-Forwarded-For is honoured
}

// OpsConfig is the operator listener: /metrics, /healthz, /readyz, pprof.
type OpsConfig struct {
	Addr  string `toml:"addr"`
	Pprof bool   `toml:"pprof"`
}

// DBConfig selects embedded PostgreSQL or an external server.
type DBConfig struct {
	URL      string `toml:"url"`       // "embedded" or postgres://...
	Port     int    `toml:"port"`      // embedded server port (loopback only)
	CacheDir string `toml:"cache_dir"` // downloaded PostgreSQL binaries (shared across data dirs)
	MaxConns int    `toml:"max_conns"`
}

// BusConfig selects the embedded NATS server or an external cluster.
type BusConfig struct {
	URL    string `toml:"url"`    // "embedded" or nats://...
	Listen string `toml:"listen"` // embedded server client port for nats.c cells/gateways; "" = in-process only
}

// CacheConfig selects miniredis or an external Valkey.
type CacheConfig struct {
	URL    string `toml:"url"`    // "miniredis" or valkey:// / redis:// / rediss://
	Listen string `toml:"listen"` // miniredis address (loopback)
}

// TelemetryConfig configures OpenTelemetry.
type TelemetryConfig struct {
	Traces      string  `toml:"traces"` // none | stdout (OTLP arrives with the collector in compose)
	SampleRatio float64 `toml:"sample_ratio"`
}

// Argon2Config holds argon2id cost parameters (05 §1.1: m=64 MiB, t=3, p=1).
type Argon2Config struct {
	MemoryKiB     uint32 `toml:"memory_kib"`
	Iterations    uint32 `toml:"iterations"`
	Parallelism   uint8  `toml:"parallelism"`
	MaxConcurrent int    `toml:"max_concurrent"` // 0 = 2 x GOMAXPROCS
}

// IdentityConfig configures accounts and access tokens.
type IdentityConfig struct {
	Issuer            string       `toml:"issuer"`
	Audience          string       `toml:"audience"`
	AccessTTL         Duration     `toml:"access_ttl"`
	RefreshTTL        Duration     `toml:"refresh_ttl"`
	LaunchCodeTTL     Duration     `toml:"launch_code_ttl"`
	PasswordMinLength int          `toml:"password_min_length"`
	AllowRegistration bool         `toml:"allow_registration"`
	Argon2            Argon2Config `toml:"argon2"`
	LoginPerIP        RateLimit    `toml:"login_per_ip"`
	LoginPerAccount   RateLimit    `toml:"login_per_account"`
	RegisterPerIP     RateLimit    `toml:"register_per_ip"`
	RefreshPerIP      RateLimit    `toml:"refresh_per_ip"`
}

// SessionConfig configures connect tokens and reconnect tickets.
type SessionConfig struct {
	ProtocolID       string    `toml:"protocol_id"` // hex (0x...) or decimal uint64; build-compatibility gate
	TokenExpiry      Duration  `toml:"token_expiry"`
	Timeout          Duration  `toml:"timeout"`
	MaxGateways      int       `toml:"max_gateways"`
	Gateways         []string  `toml:"gateways"`      // static fallback when no gateway registered
	ContentBuild     uint64    `toml:"content_build"` // content-version pin written into tokens
	SessionTTL       Duration  `toml:"session_ttl"`
	TicketTTL        Duration  `toml:"ticket_ttl"`
	CreatePerAccount RateLimit `toml:"create_per_account"`
	ReconnectPerIP   RateLimit `toml:"reconnect_per_ip"`
}

// ZoneConfig declares a zone the orchestrator places (v0: one cell per zone).
type ZoneConfig struct {
	ID   int64  `toml:"id"`
	Name string `toml:"name"`
}

// SpawnConfig declares a child process the local supervisor runs.
type SpawnConfig struct {
	Name        string   `toml:"name"`
	Exe         string   `toml:"exe"`
	Args        []string `toml:"args"`
	Env         []string `toml:"env"`
	Dir         string   `toml:"dir"`
	Restart     string   `toml:"restart"` // always | on-failure | never
	BackoffMin  Duration `toml:"backoff_min"`
	BackoffMax  Duration `toml:"backoff_max"`
	StopTimeout Duration `toml:"stop_timeout"`
}

// OrchestratorConfig configures leases, zones and local supervision.
type OrchestratorConfig struct {
	// LeaseTTL is the liveness TTL after which a silent process loses its zones: 05 §1.4.2's
	// single-signal fallback (12 s). Holders never race it; they fence only when told.
	LeaseTTL          Duration      `toml:"lease_ttl"`
	HeartbeatInterval Duration      `toml:"heartbeat_interval"`
	Zones             []ZoneConfig  `toml:"zones"`
	Spawn             []SpawnConfig `toml:"spawn"`
	SpawnFilter       string        `toml:"spawn_filter"` // "all", "none" or comma-separated names
}

// Default returns the built-in defaults (a Windows dev box running everything on loopback).
func Default() *Config {
	perMin := func(rate, burst int) RateLimit {
		return RateLimit{Rate: rate, Per: Duration(time.Minute), Burst: burst}
	}
	return &Config{
		Env:        "dev",
		DataDir:    filepath.Join(".", "saved", "backend"),
		Shard:      "dev",
		ShardIndex: 0,
		Services:   []string{"all"},
		Log:        LogConfig{Level: "info", Format: "text"},
		HTTP:       HTTPConfig{Addr: "127.0.0.1:7700"},
		Ops:        OpsConfig{Addr: "127.0.0.1:7701", Pprof: true},
		DB:         DBConfig{URL: "embedded", Port: 7702, MaxConns: 16},
		Bus:        BusConfig{URL: "embedded", Listen: "127.0.0.1:4222"},
		Cache:      CacheConfig{URL: "miniredis", Listen: "127.0.0.1:7703"},
		Telemetry:  TelemetryConfig{Traces: "none", SampleRatio: 1},
		Identity: IdentityConfig{
			Issuer:            "helios-identity",
			Audience:          "helios",
			AccessTTL:         Duration(10 * time.Minute),
			RefreshTTL:        Duration(30 * 24 * time.Hour),
			LaunchCodeTTL:     Duration(60 * time.Second),
			PasswordMinLength: 8,
			AllowRegistration: true,
			Argon2:            Argon2Config{MemoryKiB: 64 * 1024, Iterations: 3, Parallelism: 1},
			LoginPerIP:        perMin(30, 10),
			LoginPerAccount:   perMin(6, 5),
			RegisterPerIP:     perMin(10, 5),
			RefreshPerIP:      perMin(60, 20),
		},
		Session: SessionConfig{
			ProtocolID:       "0x48454c494f530001",
			TokenExpiry:      Duration(30 * time.Second),
			Timeout:          Duration(10 * time.Second),
			MaxGateways:      4,
			Gateways:         []string{"127.0.0.1:7777"},
			SessionTTL:       Duration(24 * time.Hour),
			TicketTTL:        Duration(5 * time.Minute),
			CreatePerAccount: perMin(10, 5),
			ReconnectPerIP:   perMin(30, 10),
		},
		Orchestrator: OrchestratorConfig{
			LeaseTTL:          Duration(12 * time.Second),
			HeartbeatInterval: Duration(time.Second),
			Zones:             []ZoneConfig{{ID: 1001, Name: "dev-sandbox"}},
			SpawnFilter:       "all",
		},
	}
}

// setting binds one scalar option to a flag and an environment variable.
type setting struct {
	flag, alias, env, usage string
	boolean                 bool
	apply                   func(c *Config, v string) error
}

func str(dst func(*Config) *string) func(*Config, string) error {
	return func(c *Config, v string) error { *dst(c) = v; return nil }
}

func integer(dst func(*Config) *int) func(*Config, string) error {
	return func(c *Config, v string) error {
		n, err := strconv.Atoi(v)
		if err != nil {
			return fmt.Errorf("not an integer: %q", v)
		}
		*dst(c) = n
		return nil
	}
}

func boolean(dst func(*Config) *bool) func(*Config, string) error {
	return func(c *Config, v string) error {
		b, err := strconv.ParseBool(v)
		if err != nil {
			return fmt.Errorf("not a boolean: %q", v)
		}
		*dst(c) = b
		return nil
	}
}

var settings = []setting{
	{flag: "data", alias: "data-dir", env: "HELIOS_DATA", usage: "data directory (pg/, nats/, keys/, logs/)",
		apply: str(func(c *Config) *string { return &c.DataDir })},
	{flag: "keys-dir", env: "HELIOS_KEYS_DIR", usage: "directory of key files (default <data>/keys)",
		apply: str(func(c *Config) *string { return &c.KeysDir })},
	{flag: "env", env: "HELIOS_ENV", usage: "environment: dev or prod",
		apply: str(func(c *Config) *string { return &c.Env })},
	{flag: "shard", env: "HELIOS_SHARD", usage: "shard name used in NATS subjects (e.g. dev, eu1)",
		apply: str(func(c *Config) *string { return &c.Shard })},
	{flag: "shard-index", env: "HELIOS_SHARD_INDEX", usage: "shard index 0..31 (the shard field of block IDs)",
		apply: integer(func(c *Config) *int { return &c.ShardIndex })},
	{flag: "services", env: "HELIOS_SERVICES", usage: "services to run: all or identity,session,orchestrator",
		apply: func(c *Config, v string) error { c.Services = splitList(v); return nil }},
	{flag: "seed", env: "HELIOS_SEED", usage: "seed data: dev (accounts dev1..dev10, password dev) or empty",
		apply: str(func(c *Config) *string { return &c.Seed })},
	{flag: "lan", env: "HELIOS_LAN", boolean: true, usage: "listen on all interfaces instead of loopback",
		apply: boolean(func(c *Config) *bool { return &c.LAN })},
	{flag: "log-level", env: "HELIOS_LOG_LEVEL", usage: "log level: debug, info, warn, error",
		apply: str(func(c *Config) *string { return &c.Log.Level })},
	{flag: "log-format", env: "HELIOS_LOG_FORMAT", usage: "log format: text or json",
		apply: str(func(c *Config) *string { return &c.Log.Format })},
	{flag: "http", env: "HELIOS_HTTP", usage: "public API listen address",
		apply: str(func(c *Config) *string { return &c.HTTP.Addr })},
	{flag: "tls-cert", env: "HELIOS_TLS_CERT", usage: "PEM certificate for HTTPS on the public API",
		apply: str(func(c *Config) *string { return &c.HTTP.TLSCert })},
	{flag: "tls-key", env: "HELIOS_TLS_KEY", usage: "PEM private key for HTTPS on the public API",
		apply: str(func(c *Config) *string { return &c.HTTP.TLSKey })},
	{flag: "ops", env: "HELIOS_OPS", usage: "operator listen address (/metrics, /healthz, /readyz, pprof)",
		apply: str(func(c *Config) *string { return &c.Ops.Addr })},
	{flag: "db", env: "HELIOS_DB", usage: "database: embedded or postgres://user:pass@host:port/db",
		apply: str(func(c *Config) *string { return &c.DB.URL })},
	{flag: "pg-port", env: "HELIOS_PG_PORT", usage: "embedded PostgreSQL port (loopback)",
		apply: integer(func(c *Config) *int { return &c.DB.Port })},
	{flag: "pg-cache", env: "HELIOS_PG_CACHE", usage: "cache directory for downloaded PostgreSQL binaries",
		apply: str(func(c *Config) *string { return &c.DB.CacheDir })},
	{flag: "bus", env: "HELIOS_BUS", usage: "message bus: embedded or nats://host:port",
		apply: str(func(c *Config) *string { return &c.Bus.URL })},
	{flag: "nats", env: "HELIOS_NATS", usage: "embedded NATS client listen address for cells/gateways (empty = in-process only)",
		apply: str(func(c *Config) *string { return &c.Bus.Listen })},
	{flag: "cache", env: "HELIOS_CACHE", usage: "cache: miniredis or valkey://host:port (redis://, rediss:// also accepted)",
		apply: str(func(c *Config) *string { return &c.Cache.URL })},
	{flag: "cache-listen", env: "HELIOS_CACHE_LISTEN", usage: "miniredis listen address",
		apply: str(func(c *Config) *string { return &c.Cache.Listen })},
	{flag: "traces", env: "HELIOS_TRACES", usage: "trace exporter: none or stdout",
		apply: str(func(c *Config) *string { return &c.Telemetry.Traces })},
	{flag: "spawn", env: "HELIOS_SPAWN", usage: "supervised processes to start: all, none or comma-separated names",
		apply: str(func(c *Config) *string { return &c.Orchestrator.SpawnFilter })},
}

// flagValue records the raw value of one flag so it can be applied after file and env.
type flagValue struct {
	s       *setting
	value   string
	set     bool
	boolean bool
}

// flag.Value implementation.
func (f *flagValue) String() string     { return f.value }
func (f *flagValue) Set(v string) error { f.value, f.set = v, true; return nil }
func (f *flagValue) IsBoolFlag() bool   { return f.boolean }

// Load builds the configuration from defaults, an optional TOML file, HELIOS_* environment
// variables and command-line flags, in that order of precedence, then validates it. It returns
// the positional arguments left after the flags. getenv is os.Getenv in production.
func Load(name string, args []string, getenv func(string) string, output io.Writer) (*Config, []string, error) {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	if output != nil {
		fs.SetOutput(output)
	}
	configPath := fs.String("config", "", "TOML config file (default: <data>/helios.toml when it exists; env HELIOS_CONFIG)")
	values := make([]*flagValue, len(settings))
	for i := range settings {
		s := &settings[i]
		v := &flagValue{s: s, boolean: s.boolean}
		values[i] = v
		fs.Var(v, s.flag, s.usage+" (env "+s.env+")")
		if s.alias != "" {
			fs.Var(v, s.alias, "alias of -"+s.flag)
		}
	}
	if err := fs.Parse(args); err != nil {
		return nil, nil, err
	}

	cfg := Default()

	// Resolve the data directory early: it locates the default config file.
	dataDir := cfg.DataDir
	if v := getenv("HELIOS_DATA"); v != "" {
		dataDir = v
	}
	for _, v := range values {
		if v.s.flag == "data" && v.set {
			dataDir = v.value
		}
	}
	path := *configPath
	if path == "" {
		path = getenv("HELIOS_CONFIG")
	}
	explicit := path != ""
	if !explicit {
		path = filepath.Join(dataDir, "helios.toml")
	}
	if b, err := os.ReadFile(path); err == nil {
		dec := toml.NewDecoder(bytes.NewReader(b))
		dec.DisallowUnknownFields()
		if err := dec.Decode(cfg); err != nil {
			var sm *toml.StrictMissingError
			if errors.As(err, &sm) {
				return nil, nil, fmt.Errorf("config %s: unknown keys:\n%s", path, sm.String())
			}
			return nil, nil, fmt.Errorf("config %s: %w", path, err)
		}
		cfg.ConfigFile = path
	} else if explicit || !errors.Is(err, os.ErrNotExist) {
		return nil, nil, fmt.Errorf("config: %w", err)
	}

	for _, s := range settings {
		if v := getenv(s.env); v != "" {
			if err := s.apply(cfg, v); err != nil {
				return nil, nil, fmt.Errorf("env %s: %w", s.env, err)
			}
		}
	}
	for _, v := range values {
		if v.set {
			val := v.value
			if v.boolean && val == "" {
				val = "true"
			}
			if err := v.s.apply(cfg, val); err != nil {
				return nil, nil, fmt.Errorf("flag -%s: %w", v.s.flag, err)
			}
		}
	}
	cfg.applyLAN()
	if err := cfg.Validate(); err != nil {
		return nil, nil, err
	}
	return cfg, fs.Args(), nil
}

// applyLAN widens loopback listeners to all interfaces when LAN mode is on (05 §5 --lan).
func (c *Config) applyLAN() {
	if !c.LAN {
		return
	}
	widen := func(addr string) string {
		host, port, err := net.SplitHostPort(addr)
		if err != nil || (host != "127.0.0.1" && host != "localhost" && host != "::1") {
			return addr
		}
		return net.JoinHostPort("0.0.0.0", port)
	}
	c.HTTP.Addr = widen(c.HTTP.Addr)
	c.Bus.Listen = widen(c.Bus.Listen)
}

// Known service names.
const (
	ServiceIdentity     = "identity"
	ServiceSession      = "session"
	ServiceOrchestrator = "orchestrator"
)

var allServices = []string{ServiceIdentity, ServiceSession, ServiceOrchestrator}

// Enabled reports whether service name should run in this process.
func (c *Config) Enabled(name string) bool {
	return slices.Contains(c.Services, "all") || slices.Contains(c.Services, name)
}

// ProtocolIDValue parses Session.ProtocolID.
func (c *Config) ProtocolIDValue() (uint64, error) {
	v, err := strconv.ParseUint(strings.TrimSpace(c.Session.ProtocolID), 0, 64)
	if err != nil {
		return 0, fmt.Errorf("session.protocol_id %q: %w", c.Session.ProtocolID, err)
	}
	return v, nil
}

// PGCacheDir returns the directory for downloaded PostgreSQL binaries: DB.CacheDir, or
// %LOCALAPPDATA%\helios\pg-bin on Windows and ~/.cache/helios/pg-bin elsewhere (05 §3.5).
func (c *Config) PGCacheDir() string {
	if c.DB.CacheDir != "" {
		return c.DB.CacheDir
	}
	if dir, err := os.UserCacheDir(); err == nil {
		return filepath.Join(dir, "helios", "pg-bin")
	}
	return filepath.Join(c.DataDir, "pg-bin")
}

// Validate checks ranges and cross-field rules.
func (c *Config) Validate() error {
	var errs []error
	bad := func(format string, args ...any) { errs = append(errs, fmt.Errorf(format, args...)) }

	if c.Env != "dev" && c.Env != "prod" {
		bad("env must be dev or prod, got %q", c.Env)
	}
	if c.DataDir == "" {
		bad("data directory must not be empty")
	}
	if !validToken(c.Shard) {
		bad("shard %q must be a lowercase NATS token [a-z0-9_-]", c.Shard)
	}
	if c.ShardIndex < 0 || c.ShardIndex > 31 {
		bad("shard_index %d out of range 0..31", c.ShardIndex)
	}
	if len(c.Services) == 0 {
		bad("services must not be empty")
	}
	for _, s := range c.Services {
		if s != "all" && !slices.Contains(allServices, s) {
			bad("unknown service %q (known: all, %s)", s, strings.Join(allServices, ", "))
		}
	}
	if c.Seed != "" && c.Seed != "dev" {
		bad("seed must be empty or dev, got %q", c.Seed)
	}
	if c.Seed != "" && c.Env != "dev" {
		bad("seed data is only allowed with env=dev")
	}
	switch c.Log.Level {
	case "debug", "info", "warn", "error":
	default:
		bad("log level %q (want debug, info, warn, error)", c.Log.Level)
	}
	if c.Log.Format != "text" && c.Log.Format != "json" {
		bad("log format %q (want text or json)", c.Log.Format)
	}
	for name, addr := range map[string]string{"http": c.HTTP.Addr, "ops": c.Ops.Addr} {
		if _, _, err := net.SplitHostPort(addr); err != nil {
			bad("%s address %q: %v", name, addr, err)
		}
	}
	if (c.HTTP.TLSCert == "") != (c.HTTP.TLSKey == "") {
		bad("tls_cert and tls_key must be set together")
	}
	for _, p := range c.HTTP.TrustedProxies {
		if _, _, err := net.ParseCIDR(p); err != nil {
			bad("trusted proxy %q: %v", p, err)
		}
	}
	if c.DB.URL != "embedded" && !strings.HasPrefix(c.DB.URL, "postgres://") && !strings.HasPrefix(c.DB.URL, "postgresql://") {
		bad("db must be embedded or a postgres:// URL")
	}
	if c.DB.URL == "embedded" && (c.DB.Port <= 0 || c.DB.Port > 65535) {
		bad("embedded pg port %d out of range", c.DB.Port)
	}
	if c.Bus.URL != "embedded" && !strings.HasPrefix(c.Bus.URL, "nats://") && !strings.HasPrefix(c.Bus.URL, "tls://") {
		bad("bus must be embedded or a nats:// URL")
	}
	if c.Cache.URL != "miniredis" && !hasAnyPrefix(c.Cache.URL, "valkey://", "valkeys://", "redis://", "rediss://") {
		bad("cache must be miniredis or a valkey:// / redis:// URL")
	}
	if c.Telemetry.Traces != "none" && c.Telemetry.Traces != "stdout" {
		bad("traces must be none or stdout")
	}
	if c.Telemetry.SampleRatio < 0 || c.Telemetry.SampleRatio > 1 {
		bad("sample_ratio must be within 0..1")
	}

	id := c.Identity
	if id.AccessTTL.D() < time.Minute || id.AccessTTL.D() > time.Hour {
		bad("identity.access_ttl %v must be within 1m..1h", id.AccessTTL.D())
	}
	if id.RefreshTTL.D() < id.AccessTTL.D() {
		bad("identity.refresh_ttl must be at least access_ttl")
	}
	if id.LaunchCodeTTL.D() < 5*time.Second || id.LaunchCodeTTL.D() > 5*time.Minute {
		bad("identity.launch_code_ttl must be within 5s..5m")
	}
	if id.PasswordMinLength < 1 || id.PasswordMinLength > 128 {
		bad("identity.password_min_length out of range 1..128")
	}
	if id.Argon2.MemoryKiB < 8 || id.Argon2.Iterations < 1 || id.Argon2.Parallelism < 1 {
		bad("identity.argon2 parameters too small")
	}

	s := c.Session
	if _, err := c.ProtocolIDValue(); err != nil {
		errs = append(errs, err)
	}
	// 04 §2.3: expiry ≤ 45 s. netcode gateways must set max_connect_token_lifetime to the same value.
	if s.TokenExpiry.D() < 5*time.Second || s.TokenExpiry.D() > 45*time.Second {
		bad("session.token_expiry %v must be within 5s..45s", s.TokenExpiry.D())
	}
	if s.Timeout.D() < time.Second || s.Timeout.D() > time.Minute {
		bad("session.timeout %v must be within 1s..60s", s.Timeout.D())
	}
	if s.MaxGateways < 1 || s.MaxGateways > 32 {
		bad("session.max_gateways must be within 1..32")
	}
	for _, g := range s.Gateways {
		if _, err := netip.ParseAddrPort(g); err != nil {
			bad("session gateway %q: %v", g, err)
		}
	}
	if s.TicketTTL.D() < time.Minute || s.TicketTTL.D() > time.Hour {
		bad("session.ticket_ttl must be within 1m..1h")
	}
	if s.SessionTTL.D() < s.TicketTTL.D() {
		bad("session.session_ttl must be at least ticket_ttl")
	}

	o := c.Orchestrator
	if o.LeaseTTL.D() < 500*time.Millisecond || o.HeartbeatInterval.D() <= 0 || o.HeartbeatInterval.D()*2 > o.LeaseTTL.D() {
		bad("orchestrator lease_ttl must be ≥ 500ms and ≥ 2 x heartbeat_interval")
	}
	zoneIDs, zoneNames := map[int64]bool{}, map[string]bool{}
	for _, z := range o.Zones {
		if z.ID <= 0 || z.Name == "" || zoneIDs[z.ID] || zoneNames[z.Name] {
			bad("zone %d %q: ids must be positive and ids/names unique", z.ID, z.Name)
		}
		zoneIDs[z.ID], zoneNames[z.Name] = true, true
	}
	spawnNames := map[string]bool{}
	for _, sp := range o.Spawn {
		if sp.Name == "" || sp.Exe == "" || spawnNames[sp.Name] {
			bad("spawn entries need a unique name and an exe (%q)", sp.Name)
		}
		spawnNames[sp.Name] = true
		switch sp.Restart {
		case "", "always", "on-failure", "never":
		default:
			bad("spawn %s: restart %q (want always, on-failure, never)", sp.Name, sp.Restart)
		}
	}
	return errors.Join(errs...)
}

func validToken(s string) bool {
	if s == "" {
		return false
	}
	for _, r := range s {
		if !(r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '_' || r == '-') {
			return false
		}
	}
	return true
}

func hasAnyPrefix(s string, prefixes ...string) bool {
	for _, p := range prefixes {
		if strings.HasPrefix(s, p) {
			return true
		}
	}
	return false
}

func splitList(v string) []string {
	var out []string
	for _, p := range strings.Split(v, ",") {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}
