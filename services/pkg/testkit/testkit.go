// Package testkit starts in-process infrastructure for tests (05 §8): an embedded NATS server
// with JetStream and a miniredis instance standing in for Valkey. Nothing here needs Docker or
// network access, so these helpers run in `go test ./...` on Windows and Linux.
//
// Embedded PostgreSQL is deliberately not here: it downloads binaries on first use, so only the
// integration-tagged tests in internal/integration start it.
package testkit

import (
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	"github.com/nats-io/nats-server/v2/server"
	"github.com/nats-io/nats.go"
	"github.com/redis/go-redis/v9"
)

// NATS is a running embedded server plus an in-process client connection.
type NATS struct {
	Server *server.Server
	Conn   *nats.Conn
}

// StartNATS boots an embedded NATS server with JetStream (store in t.TempDir()) that accepts
// only in-process connections, and returns a connected client. Both stop at test cleanup.
func StartNATS(t testing.TB) *NATS {
	t.Helper()
	opts := &server.Options{
		ServerName: "testkit",
		DontListen: true,
		JetStream:  true,
		StoreDir:   t.TempDir(),
		NoLog:      true,
		NoSigs:     true,
	}
	ns, err := server.NewServer(opts)
	if err != nil {
		t.Fatalf("testkit: nats server: %v", err)
	}
	go ns.Start()
	if !ns.ReadyForConnections(10 * time.Second) {
		ns.Shutdown()
		t.Fatal("testkit: nats server not ready")
	}
	nc, err := nats.Connect("", nats.InProcessServer(ns), nats.Name("testkit"))
	if err != nil {
		ns.Shutdown()
		t.Fatalf("testkit: nats connect: %v", err)
	}
	t.Cleanup(func() {
		nc.Close()
		ns.Shutdown()
		ns.WaitForShutdown()
	})
	return &NATS{Server: ns, Conn: nc}
}

// Connect opens an additional in-process client (e.g. to play a cell or gateway).
func (n *NATS) Connect(t testing.TB, name string) *nats.Conn {
	t.Helper()
	nc, err := nats.Connect("", nats.InProcessServer(n.Server), nats.Name(name))
	if err != nil {
		t.Fatalf("testkit: nats connect %s: %v", name, err)
	}
	t.Cleanup(nc.Close)
	return nc
}

// Redis is a running miniredis plus a go-redis client pointed at it.
type Redis struct {
	Mini   *miniredis.Miniredis
	Client *redis.Client
}

// StartRedis starts miniredis on a loopback port. Note that miniredis TTLs only advance when
// the test calls Mini.FastForward; they do not follow the wall clock.
func StartRedis(t testing.TB) *Redis {
	t.Helper()
	mr := miniredis.NewMiniRedis()
	if err := mr.Start(); err != nil {
		t.Fatalf("testkit: miniredis: %v", err)
	}
	rc := redis.NewClient(&redis.Options{Addr: mr.Addr(), Protocol: 2})
	t.Cleanup(func() {
		_ = rc.Close()
		mr.Close()
	})
	return &Redis{Mini: mr, Client: rc}
}
