package stack

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"path/filepath"
	"strconv"
	"time"

	"github.com/nats-io/nats-server/v2/server"
	"github.com/nats-io/nats.go"
	"golang.org/x/crypto/bcrypt"

	"github.com/PageMastr/scifi-test/services/internal/platform"
)

// MaxPayload caps NATS messages at 1 MiB (05 §6.5).
const MaxPayload = 1 << 20

// Users of the embedded NATS server. Nothing connects anonymously: with --lan the client port is
// reachable from the network, and an anonymous client could otherwise seal reconnect tickets for
// any session, register fake gateways or publish kicks.
const (
	// BackendUser is the in-process connection of helios-backend itself (all permissions; its
	// password is random per start and never leaves the process).
	BackendUser = "backend"
	// FleetUser is shared by cells and gateways (nats.c). It may call services and use KV, but
	// cannot serve rpc subjects (impersonate a service), publish control or event subjects, write
	// KV buckets or change streams. Per-role users with narrower subject permissions (05 §6.5)
	// replace it when cells and gateways get separate credentials.
	FleetUser = "fleet"
)

// FleetPermissions returns the fleet user's subject permissions.
func FleetPermissions() *server.Permissions {
	return &server.Permissions{
		Publish: &server.SubjectPermission{Deny: []string{
			"ctl.>", "evt.>", "$KV.>",
			"$JS.API.STREAM.CREATE.>", "$JS.API.STREAM.UPDATE.>", "$JS.API.STREAM.DELETE.>",
			"$JS.API.STREAM.PURGE.>", "$JS.API.STREAM.MSG.DELETE.>",
		}},
		Subscribe: &server.SubjectPermission{Deny: []string{"rpc.>"}},
	}
}

// BusAuth carries credentials for the embedded server.
type BusAuth struct {
	// FleetPassword is the fleet user's password (from <keys>/nats-fleet.json); a random one is
	// generated when empty.
	FleetPassword string
}

// Bus is a NATS connection, plus the embedded server when one was started.
type Bus struct {
	Conn *nats.Conn
	// ClientURL is what cells and gateways (nats.c) connect to; empty when the embedded server
	// accepts in-process connections only. It never contains credentials for the embedded
	// server (use FleetUser/FleetPassword); for an external bus it is the configured URL.
	ClientURL string
	// FleetPassword is the embedded server's fleet password ("" for an external bus).
	FleetPassword string
	server        *server.Server
	log           *slog.Logger
}

func randomSecret() (string, error) {
	b := make([]byte, 24)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

// OpenBus starts the embedded NATS server with JetStream (store under <data>/nats) when
// cfg.Bus.URL is "embedded"; Go services then talk to it in-process, and nats.c clients use
// cfg.Bus.Listen with the fleet credentials. Otherwise it connects to the external URL (whose
// userinfo, if any, authenticates).
func OpenBus(ctx context.Context, cfg *platform.Config, auth BusAuth, log *slog.Logger) (*Bus, error) {
	b := &Bus{log: log}
	name := "helios-backend-" + cfg.Shard
	if cfg.Bus.URL != "embedded" {
		nc, err := nats.Connect(cfg.Bus.URL, nats.Name(name), nats.MaxReconnects(-1), nats.ReconnectWait(time.Second),
			nats.RetryOnFailedConnect(true), nats.Timeout(5*time.Second))
		if err != nil {
			return nil, fmt.Errorf("nats connect %s: %w", cfg.Bus.URL, err)
		}
		deadline := time.Now().Add(30 * time.Second)
		for !nc.IsConnected() {
			if time.Now().After(deadline) || ctx.Err() != nil {
				nc.Close()
				return nil, fmt.Errorf("nats %s not reachable", cfg.Bus.URL)
			}
			time.Sleep(100 * time.Millisecond)
		}
		b.Conn, b.ClientURL = nc, cfg.Bus.URL
		return b, nil
	}

	backendPassword, err := randomSecret()
	if err != nil {
		return nil, err
	}
	fleetPassword := auth.FleetPassword
	if fleetPassword == "" {
		if fleetPassword, err = randomSecret(); err != nil {
			return nil, err
		}
	}
	// The server config holds bcrypt hashes, never the passwords themselves.
	backendHash, err := bcrypt.GenerateFromPassword([]byte(backendPassword), bcrypt.DefaultCost)
	if err != nil {
		return nil, err
	}
	fleetHash, err := bcrypt.GenerateFromPassword([]byte(fleetPassword), bcrypt.DefaultCost)
	if err != nil {
		return nil, err
	}
	opts := &server.Options{
		ServerName: name,
		JetStream:  true,
		StoreDir:   filepath.Join(cfg.DataDir, "nats"),
		MaxPayload: MaxPayload,
		NoSigs:     true,
		Users: []*server.User{
			{Username: BackendUser, Password: string(backendHash)},
			{Username: FleetUser, Password: string(fleetHash), Permissions: FleetPermissions()},
		},
	}
	if cfg.Bus.Listen == "" {
		opts.DontListen = true
	} else {
		host, port, err := net.SplitHostPort(cfg.Bus.Listen)
		if err != nil {
			return nil, fmt.Errorf("nats listen %q: %w", cfg.Bus.Listen, err)
		}
		p, err := strconv.Atoi(port)
		if err != nil {
			return nil, fmt.Errorf("nats listen port %q: %w", port, err)
		}
		if p == 0 {
			p = server.RANDOM_PORT // nats treats 0 as "default 4222"
		}
		opts.Host, opts.Port = host, p
	}
	ns, err := server.NewServer(opts)
	if err != nil {
		return nil, fmt.Errorf("nats server: %w", err)
	}
	ns.SetLoggerV2(&natsLogger{log: log.With("component", "nats")}, false, false, false)
	go ns.Start()
	if !ns.ReadyForConnections(15 * time.Second) {
		ns.Shutdown()
		return nil, errors.New("embedded nats did not become ready (port in use?)")
	}
	nc, err := nats.Connect("", nats.InProcessServer(ns), nats.Name(name), nats.UserInfo(BackendUser, backendPassword))
	if err != nil {
		ns.Shutdown()
		return nil, err
	}
	b.Conn, b.server, b.FleetPassword = nc, ns, fleetPassword
	if !opts.DontListen {
		b.ClientURL = ns.ClientURL()
	}
	log.Info("embedded NATS started", "jetstream", opts.StoreDir, "client_url", b.ClientURL)
	return b, nil
}

// Embedded reports whether this process runs the server.
func (b *Bus) Embedded() bool { return b.server != nil }

// Close drains the connection and stops the embedded server.
func (b *Bus) Close() {
	if b.Conn != nil {
		_ = b.Conn.Drain()
		deadline := time.Now().Add(5 * time.Second)
		for !b.Conn.IsClosed() && time.Now().Before(deadline) {
			time.Sleep(10 * time.Millisecond)
		}
		b.Conn.Close()
	}
	if b.server != nil {
		b.server.Shutdown()
		b.server.WaitForShutdown()
	}
}

// natsLogger adapts nats-server logging to slog.
type natsLogger struct{ log *slog.Logger }

// server.Logger implementation; nats notices are debug-level chatter for us.
func (l *natsLogger) Noticef(format string, v ...any) { l.log.Debug(fmt.Sprintf(format, v...)) }
func (l *natsLogger) Warnf(format string, v ...any)   { l.log.Warn(fmt.Sprintf(format, v...)) }
func (l *natsLogger) Fatalf(format string, v ...any)  { l.log.Error(fmt.Sprintf(format, v...)) }
func (l *natsLogger) Errorf(format string, v ...any)  { l.log.Error(fmt.Sprintf(format, v...)) }
func (l *natsLogger) Debugf(format string, v ...any)  { l.log.Debug(fmt.Sprintf(format, v...)) }
func (l *natsLogger) Tracef(format string, v ...any)  { l.log.Debug(fmt.Sprintf(format, v...)) }
