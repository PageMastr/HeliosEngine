package backend

import (
	"bytes"
	"context"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/internal/stack"
)

// The full stack (embedded PostgreSQL) is exercised by internal/integration; these tests cover
// the wiring decisions that need no database.

func TestSessionRequiresIdentity(t *testing.T) {
	cfg := platform.Default()
	cfg.DataDir = t.TempDir()
	cfg.Services = []string{platform.ServiceSession}
	if _, err := Start(context.Background(), cfg, slog.New(slog.NewTextHandler(io.Discard, nil)), Options{}); err == nil ||
		!strings.Contains(err.Error(), "identity") {
		t.Fatalf("expected a wiring error, got %v", err)
	}
}

func TestStartRefusesLockedDataDir(t *testing.T) {
	cfg := platform.Default()
	cfg.DataDir = t.TempDir()
	lock, err := stack.LockDataDir(cfg.DataDir)
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Unlock()
	// flock is per open file description on Linux, so a second lock from this process fails
	// the same way a second backend would; on Windows LockFileEx behaves the same.
	if _, err := Start(context.Background(), cfg, slog.New(slog.NewTextHandler(io.Discard, nil)), Options{}); err == nil {
		t.Fatal("started on a locked data directory")
	}
}

func TestSpawnSpecsFilter(t *testing.T) {
	cfg := platform.Default()
	cfg.Orchestrator.Spawn = []platform.SpawnConfig{{Name: "gateway", Exe: "gw"}, {Name: "cell-a", Exe: "cell", Args: []string{"-z"}},
		{Name: "cell-b", Exe: "cell"}}
	names := func() []string {
		var out []string
		for _, s := range spawnSpecs(cfg) {
			out = append(out, s.Name)
		}
		return out
	}
	if got := names(); !slices.Equal(got, []string{"gateway", "cell-a", "cell-b"}) {
		t.Fatalf("all: %v", got)
	}
	cfg.Orchestrator.SpawnFilter = "cell-a, gateway"
	if got := names(); !slices.Equal(got, []string{"gateway", "cell-a"}) {
		t.Fatalf("filtered: %v", got)
	}
	cfg.Orchestrator.SpawnFilter = "none"
	if got := names(); len(got) != 0 {
		t.Fatalf("none: %v", got)
	}
}

func TestChildEnv(t *testing.T) {
	cfg := platform.Default()
	cfg.DataDir = "/data"
	b := &Backend{Cfg: cfg, Bus: &stack.Bus{ClientURL: "nats://127.0.0.1:4222", FleetPassword: "pw"}}
	env := strings.Join(b.childEnv(), "\n")
	for _, want := range []string{"HELIOS_SHARD=dev", "HELIOS_NATS_URL=nats://127.0.0.1:4222", "HELIOS_PROTOCOL_ID=0x48454c494f530001",
		"HELIOS_TOKEN_LIFETIME=30", "netcode-shard.json", "HELIOS_NATS_USER=fleet", "HELIOS_NATS_PASSWORD=pw"} {
		if !strings.Contains(env, want) {
			t.Errorf("child env lacks %q:\n%s", want, env)
		}
	}
	// An external bus carries its own credentials in the URL; no fleet user is invented.
	b.Bus = &stack.Bus{ClientURL: "nats://nats:4222"}
	if env := strings.Join(b.childEnv(), "\n"); strings.Contains(env, "HELIOS_NATS_USER") {
		t.Fatalf("external bus: %s", env)
	}
}

func TestFleetPasswordAndLeaderHolder(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	cfg := platform.Default()
	cfg.DataDir = t.TempDir()
	ring, err := LoadKeys(cfg, KeyFileNATS, "nats-fleet", log)
	if err != nil {
		t.Fatal(err)
	}
	// The password is the "secret" string exactly as stored, so non-supervised C++ processes can
	// read it from the key file.
	raw, _ := os.ReadFile(KeyPath(cfg, KeyFileNATS))
	if pw := FleetPassword(ring); len(pw) != 44 || !strings.Contains(string(raw), `"secret": "`+pw+`"`) {
		t.Fatalf("fleet password %q not the stored secret:\n%s", pw, raw)
	}
	a, err := leaderHolder(cfg)
	if err != nil {
		t.Fatal(err)
	}
	other := *cfg
	other.DataDir = t.TempDir()
	b, _ := leaderHolder(&other)
	again, _ := leaderHolder(cfg)
	if a == b || a != again || !strings.Contains(a, ":") {
		t.Fatalf("holders %q %q %q: must be unique per data dir and stable", a, b, again)
	}
}

func TestLoadKeysDevGeneratesProdRequires(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	cfg := platform.Default()
	cfg.DataDir = t.TempDir()
	cfg.KeysDir = filepath.Join(t.TempDir(), "secrets")
	ring, err := LoadKeys(cfg, KeyFileNetcode, "netcode-shard", log)
	if err != nil || ring.Current().ID != 1 {
		t.Fatalf("dev: %v", err)
	}
	if _, err := os.Stat(filepath.Join(cfg.KeysDir, KeyFileNetcode)); err != nil {
		t.Fatal("keys_dir not honoured")
	}
	again, _ := LoadKeys(cfg, KeyFileNetcode, "netcode-shard", log)
	if !bytes.Equal(again.Current().Secret, ring.Current().Secret) {
		t.Fatal("dev key regenerated")
	}
	cfg.Env = "prod"
	if _, err := LoadKeys(cfg, KeyFileTickets, "reconnect-ticket", log); err == nil {
		t.Fatal("prod generated a missing key")
	}
	if _, err := LoadKeys(cfg, KeyFileNetcode, "netcode-shard", log); err != nil {
		t.Fatalf("prod with provisioned key: %v", err)
	}
}

func TestLoadPIIKeys(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	cfg := platform.Default()
	cfg.DataDir = t.TempDir()
	cfg.Env = "prod"
	if _, err := LoadPIIKeys(cfg, log); err == nil {
		t.Fatal("prod generated missing PII keys")
	}
	cfg.Env = "dev"
	k1, err := LoadPIIKeys(cfg, log)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{KeyFileSubjectKEK, KeyFileEmailPepper} {
		if _, err := os.Stat(KeyPath(cfg, name)); err != nil {
			t.Fatalf("%s not written: %v", name, err)
		}
	}
	// A restart reads the same files: the same address has the same blind index.
	k2, _ := LoadPIIKeys(cfg, log)
	if !bytes.Equal(k1.EmailIndex("a@b.io"), k2.EmailIndex("A@B.io")) {
		t.Fatal("PII keys changed across loads")
	}
	cfg.Env = "prod"
	if _, err := LoadPIIKeys(cfg, log); err != nil {
		t.Fatalf("prod with provisioned keys: %v", err)
	}
}
