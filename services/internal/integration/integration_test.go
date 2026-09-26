//go:build integration

// Package integration boots the real all-in-one backend (embedded PostgreSQL 18, embedded NATS
// with JetStream, miniredis) and drives it end to end: register → login → connect token →
// token validation (Go, and the vendored netcode C library when HELIOS_NETCODE_INTEROP=1),
// gateway/cell registration over NATS, reconnect tickets, refresh rotation, bans, and the
// PostgreSQL store conformance suites.
//
// Run (needs network on first use to download PostgreSQL binaries from Maven Central, cached in
// the user cache dir or $HELIOS_PG_CACHE; embedded PostgreSQL refuses to run as root):
//
//	go test -tags integration ./internal/integration/ -v
package integration

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/jackc/pgx/v5/stdlib"
	"github.com/nats-io/nats.go"

	"github.com/PageMastr/scifi-test/services/internal/backend"
	"github.com/PageMastr/scifi-test/services/internal/identity"
	identitystoretest "github.com/PageMastr/scifi-test/services/internal/identity/storetest"
	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
	orchestratorstoretest "github.com/PageMastr/scifi-test/services/internal/orchestrator/storetest"
	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/internal/session"
	infra "github.com/PageMastr/scifi-test/services/internal/stack"
	"github.com/PageMastr/scifi-test/services/migrations"
	"github.com/PageMastr/scifi-test/services/pkg/connecttoken"
	"github.com/PageMastr/scifi-test/services/pkg/connecttoken/cinterop"
	"github.com/PageMastr/scifi-test/services/pkg/idgen"
	"github.com/PageMastr/scifi-test/services/pkg/keyring"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

const shard = "it"

var (
	stack      *backend.Backend
	skipReason string
	apiURL     string
	opsURL     string
)

func freePort() int {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

func TestMain(m *testing.M) {
	if runtime.GOOS != "windows" && os.Geteuid() == 0 {
		skipReason = "embedded PostgreSQL refuses to run as root; run the integration tests as a normal user"
		os.Exit(m.Run())
	}
	dir, err := os.MkdirTemp("", "helios-it-")
	if err != nil {
		panic(err)
	}
	cfg := platform.Default()
	cfg.DataDir = dir
	cfg.Shard = shard
	cfg.HTTP.Addr, cfg.Ops.Addr = "127.0.0.1:0", "127.0.0.1:0"
	cfg.Bus.Listen, cfg.Cache.Listen = "127.0.0.1:0", "127.0.0.1:0"
	cfg.DB.Port = freePort()
	cfg.DB.CacheDir = os.Getenv("HELIOS_PG_CACHE")
	cfg.Seed = "dev"
	cfg.Orchestrator.LeaseTTL = platform.Duration(time.Second)
	cfg.Orchestrator.HeartbeatInterval = platform.Duration(250 * time.Millisecond)
	level := slog.LevelWarn
	if os.Getenv("HELIOS_TEST_LOG") != "" {
		level = slog.LevelDebug
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	start := time.Now()
	stack, err = backend.Start(ctx, cfg, log, backend.Options{Version: "it",
		HashParams: &identity.HashParams{MemoryKiB: 1024, Iterations: 1, Parallelism: 1}})
	cancel()
	if err != nil {
		fmt.Fprintln(os.Stderr, "integration: backend failed to start:", err)
		os.Exit(1)
	}
	fmt.Fprintf(os.Stderr, "integration: all-in-one backend up in %s (api %s)\n", time.Since(start).Round(time.Millisecond), stack.API.URL())
	apiURL, opsURL = stack.API.URL(), stack.Ops.URL()
	code := m.Run()
	sctx, scancel := context.WithTimeout(context.Background(), 30*time.Second)
	if err := stack.Shutdown(sctx); err != nil {
		fmt.Fprintln(os.Stderr, "integration: shutdown:", err)
		code = 1
	}
	scancel()
	_ = os.RemoveAll(dir)
	os.Exit(code)
}

func need(t *testing.T) {
	t.Helper()
	if skipReason != "" {
		t.Skip(skipReason)
	}
}

// call POSTs a Connect-style JSON request and decodes the response into out.
func call(t *testing.T, base, path, bearer string, in, out any) int {
	t.Helper()
	body, _ := json.Marshal(in)
	req, _ := http.NewRequest(http.MethodPost, base+path, bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	if bearer != "" {
		req.Header.Set("Authorization", "Bearer "+bearer)
	}
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	b, _ := io.ReadAll(res.Body)
	if res.StatusCode == http.StatusOK && out != nil {
		if err := json.Unmarshal(b, out); err != nil {
			t.Fatalf("%s: %v: %s", path, err, b)
		}
	}
	if res.StatusCode != http.StatusOK {
		t.Logf("%s -> %d %s", path, res.StatusCode, b)
	}
	return res.StatusCode
}

func login(t *testing.T, loginName, password string) *identity.TokenPair {
	t.Helper()
	var pair identity.TokenPair
	if st := call(t, apiURL, identity.ServicePath+"Login", "", identity.LoginRequest{Login: loginName, Password: password}, &pair); st != 200 {
		t.Fatalf("login %s: %d", loginName, st)
	}
	return &pair
}

// openToken validates a connect token the way a gateway does: parse, then decrypt the private
// part with the shard key from <data>/keys.
func openToken(t *testing.T, info *session.ConnectInfo) (*connecttoken.Token, *connecttoken.PrivateData, connecttoken.UserData, []byte) {
	t.Helper()
	raw, err := base64.StdEncoding.DecodeString(info.ConnectToken)
	if err != nil || len(raw) != connecttoken.TokenBytes {
		t.Fatalf("token encoding: %d bytes, %v", len(raw), err)
	}
	tok, err := connecttoken.Parse(raw)
	if err != nil {
		t.Fatal(err)
	}
	ring, err := keyring.Load(backend.KeyPath(stack.Cfg, backend.KeyFileNetcode))
	if err != nil {
		t.Fatal(err)
	}
	var key connecttoken.Key
	copy(key[:], ring.Current().Secret)
	priv, err := tok.Open(&key)
	if err != nil {
		t.Fatalf("gateway-side decrypt failed: %v", err)
	}
	ud, err := connecttoken.UnmarshalUserData(&priv.UserData)
	if err != nil {
		t.Fatal(err)
	}
	return tok, priv, ud, key[:]
}

func TestRegisterLoginConnectToken(t *testing.T) {
	need(t)
	var reg identity.RegisterResponse
	if st := call(t, apiURL, identity.ServicePath+"Register", "", identity.RegisterRequest{
		Email: "pilot@example.com", Handle: "Pilot", Password: "stellar cartography"}, &reg); st != 200 {
		t.Fatalf("register: %d", st)
	}
	pair := login(t, "pilot@example.com", "stellar cartography")
	if pair.AccountID != reg.AccountID {
		t.Fatalf("login account %d != %d", pair.AccountID, reg.AccountID)
	}
	var info session.ConnectInfo
	if st := call(t, apiURL, session.ServicePath+"CreateSession", pair.AccessToken,
		session.CreateSessionRequest{CharacterID: 555, ZoneID: 1001}, &info); st != 200 {
		t.Fatalf("CreateSession: %d", st)
	}
	tok, priv, ud, key := openToken(t, &info)
	if priv.ClientID != info.SessionID || ud.AccountID != uint64(reg.AccountID) || ud.CharacterID != 555 ||
		ud.ZoneID != 1001 || ud.SessionEpoch != 1 {
		t.Fatalf("token content: client %d, %+v", priv.ClientID, ud)
	}
	if tok.ExpireTimestamp-tok.CreateTimestamp != 30 || tok.TimeoutSeconds != 10 || tok.ProtocolID != 0x48454c494f530001 {
		t.Fatalf("token header %+v", tok)
	}
	if len(info.Gateways) != 1 || info.Gateways[0] != "127.0.0.1:7777" {
		t.Fatalf("gateways %v (static fallback expected: no gateway registered yet)", info.Gateways)
	}

	if !cinterop.Enabled() {
		t.Log("set HELIOS_NETCODE_INTEROP=1 to also validate the token with the vendored netcode C library")
		return
	}
	harness := cinterop.Build(t)
	dir := t.TempDir()
	raw, _ := base64.StdEncoding.DecodeString(info.ConnectToken)
	tokenPath, keyPath := filepath.Join(dir, "token.bin"), filepath.Join(dir, "key.hex")
	_ = os.WriteFile(tokenPath, raw, 0o600)
	_ = os.WriteFile(keyPath, []byte(hex.EncodeToString(key)), 0o600)
	var verified struct {
		ClientID string `json:"client_id"`
		UserData string `json:"user_data"`
	}
	if err := json.Unmarshal(cinterop.Run(t, harness, "verify", tokenPath, keyPath), &verified); err != nil {
		t.Fatal(err)
	}
	if verified.ClientID != fmt.Sprintf("%016x", info.SessionID) || verified.UserData != hex.EncodeToString(priv.UserData[:]) {
		t.Fatalf("netcode C read different content: %+v", verified)
	}
	var hs struct {
		Connected bool   `json:"connected"`
		ClientID  string `json:"client_id"`
	}
	if err := json.Unmarshal(cinterop.Run(t, harness, "connect", tokenPath, keyPath, "127.0.0.1:7777"), &hs); err != nil || !hs.Connected {
		t.Fatalf("netcode handshake with a backend-minted token failed: %+v %v", hs, err)
	}
}

func TestGatewayCellReconnectOverNATS(t *testing.T) {
	need(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	// Gateways and cells reach the bus over TCP with the fleet credentials, like nats.c processes.
	if anon, err := nats.Connect(stack.Bus.ClientURL, nats.Name("it-anonymous")); err == nil {
		anon.Close()
		t.Fatal("the backend's NATS port accepted an anonymous client")
	}
	fleet := nats.UserInfo(infra.FleetUser, stack.Bus.FleetPassword)
	gwConn, err := nats.Connect(stack.Bus.ClientURL, nats.Name("it-gateway"), fleet)
	if err != nil {
		t.Fatal(err)
	}
	defer gwConn.Close()
	cellConn, err := nats.Connect(stack.Bus.ClientURL, nats.Name("it-cell"), fleet)
	if err != nil {
		t.Fatal(err)
	}
	defer cellConn.Close()

	ring, _ := keyring.Load(backend.KeyPath(stack.Cfg, backend.KeyFileNetcode))
	gw := orchestrator.NewAgent(gwConn, shard, orchestrator.ProcessInfo{Name: "gw-1", Kind: orchestrator.KindGateway,
		Address: "127.0.0.1:47777", KeyID: ring.Current().ID, Capacity: 128}, nil)
	gw.Load = func() orchestrator.Load { return orchestrator.Load{FreeSlots: 120} }
	cell := orchestrator.NewAgent(cellConn, shard, orchestrator.ProcessInfo{Name: "cell-a", Kind: orchestrator.KindCell,
		Address: "127.0.0.1:47800"}, nil)
	var assigned atomic.Int64
	cell.OnAssignments = func(a []orchestrator.Assignment) {
		if len(a) > 0 {
			assigned.Store(a[0].ZoneID)
		} else {
			assigned.Store(0)
		}
	}
	gwDone, cellDone := make(chan struct{}), make(chan struct{})
	go func() { _ = gw.Run(ctx); close(gwDone) }()
	cellCtx, stopCell := context.WithCancel(ctx)
	go func() { _ = cell.Run(cellCtx); close(cellDone) }()
	eventually(t, "cell owns zone 1001", func() bool { return assigned.Load() == 1001 && gw.Current() != nil })

	// The cell got ID blocks from the shard's PostgreSQL id_alloc row and mints block IDs with them.
	creg := cell.Current()
	if creg == nil || len(creg.IDBlocks) != 2 || creg.IDShard != stack.Cfg.ShardIndex {
		t.Fatalf("cell id blocks: %+v", creg)
	}
	// The zone's region_lease row names the cell under the generation it was told.
	var holder, gen int64
	if err := stack.PG.Pool.QueryRow(ctx, `SELECT holder_proc, lease_gen FROM svc_orch.region_lease WHERE region_id = $1`,
		orchestrator.WholeRegion(1001)).Scan(&holder, &gen); err != nil || holder != creg.ProcessID || gen != creg.Assignments[0].LeaseGen {
		t.Fatalf("region_lease: holder %d gen %d (assignment %+v) %v", holder, gen, creg.Assignments, err)
	}
	minter, err := idgen.NewMinter(creg.IDShard, cell, idgen.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer minter.Close()
	id, err := minter.Next() // the minter refills through AllocateIdBlocks over NATS
	if err != nil {
		t.Fatal(err)
	}
	if p := idgen.Decode(id); time.Since(p.Time) > time.Minute || p.Shard != creg.IDShard {
		t.Fatalf("cell-minted id %d decodes to %+v", id, p)
	}

	rctx, rcancel := context.WithTimeout(ctx, 5*time.Second)
	defer rcancel()
	route, err := rpc.NATSRequest[orchestrator.ResolveZoneRequest, orchestrator.Route](rctx, gwConn,
		orchestrator.Subject(shard, orchestrator.MethodResolveZone), &orchestrator.ResolveZoneRequest{ZoneID: 1001})
	if err != nil || route.Process != "cell-a" || route.Address != "127.0.0.1:47800" {
		t.Fatalf("world directory: %+v %v", route, err)
	}

	// Tokens now point at the registered gateway.
	pair := login(t, "dev1#0001", "dev")
	var info session.ConnectInfo
	if st := call(t, apiURL, session.ServicePath+"CreateSession", pair.AccessToken, session.CreateSessionRequest{ZoneID: 1001}, &info); st != 200 {
		t.Fatalf("CreateSession: %d", st)
	}
	if len(info.Gateways) != 1 || info.Gateways[0] != "127.0.0.1:47777" {
		t.Fatalf("token gateways %v", info.Gateways)
	}
	openToken(t, &info)

	// The gateway seals a ticket (60 s cadence in production), the client redeems it over HTTPS,
	// and the gateway is told the session moved to epoch 2.
	epochs, err := gwConn.SubscribeSync(session.ControlSubject(shard, "session_epoch"))
	if err != nil {
		t.Fatal(err)
	}
	_ = gwConn.Flush()
	sealed, err := rpc.NATSRequest[session.SealRequest, session.SealResponse](rctx, gwConn, session.SealSubject(shard),
		&session.SealRequest{Sessions: []session.SealItem{{SessionID: info.SessionID, ZoneID: 1001}}})
	if err != nil || len(sealed.Tickets) != 1 {
		t.Fatalf("seal: %+v %v", sealed, err)
	}
	var again session.ConnectInfo
	if st := call(t, apiURL, session.ServicePath+"Reconnect", "", session.ReconnectRequest{Ticket: sealed.Tickets[0].Ticket}, &again); st != 200 {
		t.Fatalf("reconnect: %d", st)
	}
	_, priv, ud, _ := openToken(t, &again)
	if priv.ClientID != info.SessionID || ud.SessionEpoch != 2 || ud.Flags&connecttoken.FlagReconnect == 0 {
		t.Fatalf("reconnect token %+v", ud)
	}
	msg, err := epochs.NextMsg(5 * time.Second)
	if err != nil || !strings.Contains(string(msg.Data), `"epoch":"2"`) {
		t.Fatalf("epoch notification: %v %v", msg, err)
	}
	if st := call(t, apiURL, session.ServicePath+"Reconnect", "", session.ReconnectRequest{Ticket: sealed.Tickets[0].Ticket}, nil); st != 401 {
		t.Fatalf("replayed ticket must be refused: %d", st)
	}

	// Cell shutdown hands the zone back immediately.
	stopCell()
	<-cellDone
	eventually(t, "zone released", func() bool {
		_, err := stack.Orchestrator.Registry().ResolveZone(1001, "")
		return rpc.CodeOf(err) == rpc.CodeUnavailable
	})
	cancel()
	<-gwDone
}

func TestBlockIDsAndOrchestratorLeadership(t *testing.T) {
	need(t)
	// Account IDs are block IDs (05 §1.4.5) from this shard's id_alloc row.
	var reg identity.RegisterResponse
	if st := call(t, apiURL, identity.ServicePath+"Register", "", identity.RegisterRequest{
		Email: "blocks@example.com", Handle: "Blocks", Password: "stellar cartography"}, &reg); st != 200 {
		t.Fatalf("register: %d", st)
	}
	if p := idgen.Decode(reg.AccountID); p.Shard != stack.Cfg.ShardIndex || time.Since(p.Time) > time.Hour || time.Until(p.Time) > time.Minute {
		t.Fatalf("account id %d decodes to %+v", reg.AccountID, p)
	}

	// The backend leads its shard in PostgreSQL...
	var list orchestrator.ListProcessesResponse
	if st := call(t, opsURL, orchestrator.ServicePath+orchestrator.MethodListProcesses, "", struct{}{}, &list); st != 200 ||
		!list.Leader || list.Term < 1 {
		t.Fatalf("leadership: %d %+v", st, list)
	}
	var holder string
	var term int64
	if err := stack.PG.Pool.QueryRow(context.Background(), `SELECT holder, term FROM svc_orch.orch_leader WHERE shard = $1`,
		shard).Scan(&holder, &term); err != nil || !strings.HasSuffix(holder, stack.Cfg.DataDir) || term != list.Term {
		t.Fatalf("orch_leader row: %q %d %v", holder, term, err)
	}
	// ...so a second orchestrator on the same database stays on standby, and writes fenced with a
	// stale term touch nothing.
	second, err := orchestrator.New(orchestrator.Deps{Config: stack.Cfg.Orchestrator, Shard: shard,
		Store: orchestrator.NewPGStore(stack.PG.Pool), NATS: stack.Bus.Conn, Holder: "another-host:/data",
		Log: slog.New(slog.NewTextHandler(io.Discard, nil))})
	if err != nil {
		t.Fatal(err)
	}
	if err := second.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	if ok, _ := second.Leader(); ok {
		t.Fatal("two orchestrator leaders on one shard")
	}
	if err := second.Stop(context.Background()); err != nil {
		t.Fatal(err)
	}
	pg := orchestrator.NewPGStore(stack.PG.Pool)
	if _, err := pg.AssignRegion(context.Background(), orchestrator.Fence{Shard: shard, Term: term - 1}, orchestrator.WholeRegion(1001), 1,
		time.Now()); !errors.Is(err, orchestrator.ErrNotLeader) {
		t.Fatalf("stale-term write: %v", err)
	}
}

func TestLaunchCodeFamilyAndBanKick(t *testing.T) {
	need(t)
	launcher := login(t, "dev5@helios.test", "dev")
	var code identity.LaunchCode
	if st := call(t, apiURL, identity.ServicePath+"CreateLaunchCode", launcher.AccessToken, struct{}{}, &code); st != 200 {
		t.Fatalf("launch code: %d", st)
	}
	var game identity.TokenPair
	if st := call(t, apiURL, identity.ServicePath+"ExchangeLaunchCode", "", identity.ExchangeLaunchCodeRequest{Code: code.Code}, &game); st != 200 {
		t.Fatalf("exchange: %d", st)
	}
	// Logging out of the launcher ends the game client's login too, and the launcher's still
	// valid access token can no longer mint launch codes.
	if st := call(t, apiURL, identity.ServicePath+"Logout", "", identity.LogoutRequest{RefreshToken: launcher.RefreshToken}, nil); st != 200 {
		t.Fatalf("logout: %d", st)
	}
	if st := call(t, apiURL, identity.ServicePath+"Refresh", "", identity.RefreshRequest{RefreshToken: game.RefreshToken}, nil); st != 401 {
		t.Fatalf("game refresh after launcher logout: %d", st)
	}
	if st := call(t, apiURL, identity.ServicePath+"CreateLaunchCode", launcher.AccessToken, struct{}{}, nil); st != 401 {
		t.Fatalf("launch code after logout: %d", st)
	}

	// A ban kicks the live game session at once.
	kicks, err := stack.Bus.Conn.SubscribeSync(session.ControlSubject(shard, "kick"))
	if err != nil {
		t.Fatal(err)
	}
	defer kicks.Unsubscribe()
	_ = stack.Bus.Conn.Flush()
	p := login(t, "dev6@helios.test", "dev")
	var info session.ConnectInfo
	if st := call(t, apiURL, session.ServicePath+"CreateSession", p.AccessToken, session.CreateSessionRequest{}, &info); st != 200 {
		t.Fatalf("CreateSession: %d", st)
	}
	until := time.Now().Add(time.Hour)
	if st := call(t, opsURL, "/admin/identity/Ban", "", identity.BanRequest{AccountID: p.AccountID, Until: &until, Reason: "it"}, nil); st != 200 {
		t.Fatalf("ban: %d", st)
	}
	for {
		m, err := kicks.NextMsg(5 * time.Second)
		if err != nil {
			t.Fatalf("no kick after ban: %v", err)
		}
		if strings.Contains(string(m.Data), fmt.Sprintf(`"sessionId":"%d"`, info.SessionID)) {
			if !strings.Contains(string(m.Data), `"reason":"banned"`) {
				t.Fatalf("kick %s", m.Data)
			}
			break
		}
	}
	if st := call(t, apiURL, session.ServicePath+"Reconnect", "", session.ReconnectRequest{Ticket: info.ReconnectTicket}, nil); st != 401 && st != 403 {
		t.Fatalf("reconnect after ban: %d", st)
	}
}

func TestRefreshLogoutBanAudit(t *testing.T) {
	need(t)
	pair := login(t, "dev2@helios.test", "dev")
	var next identity.TokenPair
	if st := call(t, apiURL, identity.ServicePath+"Refresh", "", identity.RefreshRequest{RefreshToken: pair.RefreshToken}, &next); st != 200 {
		t.Fatalf("refresh: %d", st)
	}
	if st := call(t, apiURL, identity.ServicePath+"Refresh", "", identity.RefreshRequest{RefreshToken: pair.RefreshToken}, nil); st != 401 {
		t.Fatalf("reuse: %d", st)
	}
	if st := call(t, apiURL, identity.ServicePath+"Refresh", "", identity.RefreshRequest{RefreshToken: next.RefreshToken}, nil); st != 401 {
		t.Fatalf("family must be revoked after reuse: %d", st)
	}
	p3 := login(t, "dev2@helios.test", "dev")
	if st := call(t, apiURL, identity.ServicePath+"Logout", "", identity.LogoutRequest{RefreshToken: p3.RefreshToken}, nil); st != 200 {
		t.Fatalf("logout: %d", st)
	}
	var launch identity.LaunchCode
	p4 := login(t, "dev3@helios.test", "dev")
	if st := call(t, apiURL, identity.ServicePath+"CreateLaunchCode", p4.AccessToken, struct{}{}, &launch); st != 200 {
		t.Fatalf("launch code: %d", st)
	}
	if st := call(t, apiURL, identity.ServicePath+"ExchangeLaunchCode", "", identity.ExchangeLaunchCodeRequest{Code: launch.Code}, nil); st != 200 {
		t.Fatalf("exchange: %d", st)
	}
	// Ban through the (dev) ops listener, then the account can neither log in nor get tokens.
	until := time.Now().Add(time.Hour)
	if st := call(t, opsURL, "/admin/identity/Ban", "", identity.BanRequest{AccountID: p4.AccountID, Until: &until, Reason: "it"}, nil); st != 200 {
		t.Fatalf("ban: %d", st)
	}
	if st := call(t, apiURL, identity.ServicePath+"Login", "", identity.LoginRequest{Login: "dev3#0001", Password: "dev"}, nil); st != 403 {
		t.Fatalf("banned login: %d", st)
	}
	if st := call(t, apiURL, session.ServicePath+"CreateSession", p4.AccessToken, session.CreateSessionRequest{}, nil); st != 403 {
		t.Fatalf("banned session: %d", st)
	}
	var audit identity.VerifyAuditResponse
	if st := call(t, opsURL, "/admin/identity/VerifyAudit", "", struct{}{}, &audit); st != 200 || audit.Entries < 10 {
		t.Fatalf("audit chain in PostgreSQL: %d %+v", st, audit)
	}
	// The append-only guard holds even for the owner role.
	if _, err := stack.PG.Pool.Exec(context.Background(), "DELETE FROM svc_identity.audit_log"); err == nil {
		t.Fatal("audit rows could be deleted")
	}
}

func TestOpsEndpoints(t *testing.T) {
	need(t)
	for path, want := range map[string]string{
		"/healthz": "ok", "/readyz": `"ready":true`, "/metrics": "helios_identity_logins_total",
	} {
		res, err := http.Get(opsURL + path)
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(res.Body)
		res.Body.Close()
		if res.StatusCode != 200 || !strings.Contains(string(b), want) {
			t.Errorf("%s: %d %.200s", path, res.StatusCode, b)
		}
	}
	res, err := http.Get(apiURL + "/.well-known/jwks.json")
	if err != nil || res.StatusCode != 200 {
		t.Fatalf("jwks: %v", err)
	}
	res.Body.Close()
}

// freshDB creates an empty, migrated database on the embedded server for one store suite run.
func freshDB(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool := emptyDB(t)
	db := stdlib.OpenDBFromPool(pool)
	if _, err := migrations.Up(context.Background(), db, slog.New(slog.NewTextHandler(io.Discard, nil)), migrations.Options{}); err != nil {
		t.Fatal(err)
	}
	_ = db.Close()
	return pool
}

// emptyDB creates an empty database on the embedded server, dropped at cleanup.
func emptyDB(t *testing.T) *pgxpool.Pool {
	t.Helper()
	ctx := context.Background()
	name := fmt.Sprintf("conf_%d", time.Now().UnixNano())
	if _, err := stack.PG.Pool.Exec(ctx, "CREATE DATABASE "+name); err != nil {
		t.Fatal(err)
	}
	u, _ := url.Parse(stack.PG.URL)
	u.Path = "/" + name
	pool, err := pgxpool.New(ctx, u.String())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		pool.Close()
		_, _ = stack.PG.Pool.Exec(context.Background(), "DROP DATABASE "+name+" WITH (FORCE)")
	})
	return pool
}

func TestPostgresStoreConformance(t *testing.T) {
	need(t)
	identitystoretest.Run(t, func(t *testing.T) identity.Store { return identity.NewPGStore(freshDB(t)) })
	orchestratorstoretest.Run(t, func(t *testing.T) orchestrator.Store { return orchestrator.NewPGStore(freshDB(t)) })
}

func TestMigrationsAreIdempotent(t *testing.T) {
	need(t)
	db := stdlib.OpenDBFromPool(stack.PG.Pool)
	defer db.Close()
	res, err := migrations.Up(context.Background(), db, slog.New(slog.NewTextHandler(io.Discard, nil)), migrations.Options{})
	if err != nil {
		t.Fatal(err)
	}
	for _, r := range res {
		if r.Applied != 0 || r.Version < 1 {
			t.Fatalf("re-run applied migrations: %+v", r)
		}
	}
}

func eventually(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}
