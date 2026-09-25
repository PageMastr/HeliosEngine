package session

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/nats-io/nats.go"
	"github.com/prometheus/client_golang/prometheus"

	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/authn"
	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/connecttoken"
	"github.com/PageMastr/scifi-test/services/pkg/keyring"
	"github.com/PageMastr/scifi-test/services/pkg/ratelimit"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
	"github.com/PageMastr/scifi-test/services/pkg/testkit"
)

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

type fakeAccounts struct {
	mu     sync.Mutex
	banned map[int64]bool
}

func (f *fakeAccounts) CheckAccount(_ context.Context, id int64) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.banned[id] {
		return rpc.Errorf(rpc.CodePermissionDenied, "account is suspended")
	}
	return nil
}

type fakeGateways struct {
	mu  sync.Mutex
	gws []GatewayInstance
}

func (f *fakeGateways) Gateways() []GatewayInstance {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]GatewayInstance(nil), f.gws...)
}

func (f *fakeGateways) set(g ...GatewayInstance) { f.mu.Lock(); f.gws = g; f.mu.Unlock() }

type zoneSet map[int64]bool

func (z zoneSet) ZoneExists(id int64) bool { return z[id] }

type sfix struct {
	svc      *Service
	clk      *clock.Fake
	redis    *testkit.Redis
	bus      *testkit.NATS
	keys     *keyring.Ring
	accounts *fakeAccounts
	gws      *fakeGateways
	ctl      chan *nats.Msg
	chars    CharacterChecker
}

// ownedCharacters maps account -> the one character it owns.
type ownedCharacters map[int64]int64

func (o ownedCharacters) CheckCharacter(_ context.Context, account, character int64) error {
	if o[account] != character {
		return rpc.Errorf(rpc.CodePermissionDenied, "character %d is not yours", character)
	}
	return nil
}

func newSvc(t *testing.T, mutate ...func(*platform.SessionConfig)) *sfix {
	t.Helper()
	cfg := platform.Default().Session
	for _, m := range mutate {
		m(&cfg)
	}
	f := &sfix{clk: clock.NewFake(t0), redis: testkit.StartRedis(t), bus: testkit.StartNATS(t), keys: newRing(t),
		accounts: &fakeAccounts{banned: map[int64]bool{}}, gws: &fakeGateways{}, ctl: make(chan *nats.Msg, 64),
		chars: ownedCharacters{42: 7, 43: 8}}
	svc, err := New(Deps{Config: cfg, Shard: "t1", ProtocolID: 0x48454c494f530001, ShardKeys: f.keys,
		Tickets: NewTicketSealer(newRing(t)), Store: NewStore(f.redis.Client), Accounts: f.accounts, Gateways: f.gws,
		Zones: zoneSet{1001: true}, Characters: f.chars, Limiter: ratelimit.New(f.redis.Client, f.clk, "rl:", quiet),
		NATS: f.bus.Conn, Clock: f.clk, Log: quiet, Metrics: prometheus.NewRegistry()})
	if err != nil {
		t.Fatal(err)
	}
	if err := svc.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = svc.Stop(context.Background()) })
	if _, err := f.bus.Conn.ChanSubscribe("ctl.t1.gateway.all.>", f.ctl); err != nil {
		t.Fatal(err)
	}
	f.svc = svc
	return f
}

func player(id int64) *authn.Principal {
	return &authn.Principal{AccountID: id, Scope: []string{authn.ScopeGame}}
}

// open decodes and decrypts a token the way a gateway would.
func (f *sfix) open(t *testing.T, info *ConnectInfo, keyID uint32) (*connecttoken.Token, *connecttoken.PrivateData, connecttoken.UserData) {
	t.Helper()
	raw, err := base64.StdEncoding.DecodeString(info.ConnectToken)
	if err != nil {
		t.Fatal(err)
	}
	tok, err := connecttoken.Parse(raw)
	if err != nil {
		t.Fatal(err)
	}
	e, ok := f.keys.Get(keyID)
	if !ok {
		t.Fatalf("no key %d", keyID)
	}
	var key connecttoken.Key
	copy(key[:], e.Secret)
	priv, err := tok.Open(&key)
	if err != nil {
		t.Fatalf("token does not open with key %d: %v", keyID, err)
	}
	ud, err := connecttoken.UnmarshalUserData(&priv.UserData)
	if err != nil {
		t.Fatal(err)
	}
	return tok, priv, ud
}

func (f *sfix) nextControl(t *testing.T, verb string) ControlMessage {
	t.Helper()
	deadline := time.After(5 * time.Second)
	for {
		select {
		case m := <-f.ctl:
			if strings.HasSuffix(m.Subject, "."+verb) {
				var c ControlMessage
				if err := json.Unmarshal(m.Data, &c); err != nil {
					t.Fatal(err)
				}
				return c
			}
		case <-deadline:
			t.Fatalf("no %s control message", verb)
		}
	}
}

func TestCreateSessionMintsNetcodeToken(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	info, err := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{CharacterID: 7, ZoneID: 1001})
	if err != nil {
		t.Fatal(err)
	}
	tok, priv, ud := f.open(t, info, 1)
	if priv.ClientID != info.SessionID || info.SessionEpoch != 1 || tok.ProtocolID != 0x48454c494f530001 {
		t.Fatalf("header: %+v", info)
	}
	if tok.ExpireTimestamp-tok.CreateTimestamp != 30 || tok.TimeoutSeconds != 10 || info.TimeoutSeconds != 10 ||
		!info.ExpiresAt.Equal(t0.Add(30*time.Second)) {
		t.Fatalf("lifetime: %d %d", tok.ExpireTimestamp-tok.CreateTimestamp, tok.TimeoutSeconds)
	}
	if len(info.Gateways) != 1 || info.Gateways[0] != "127.0.0.1:7777" || tok.ServerAddresses[0].String() != "127.0.0.1:7777" {
		t.Fatalf("static gateway fallback: %v", info.Gateways)
	}
	if ud.AccountID != 42 || ud.CharacterID != 7 || ud.ZoneID != 1001 || ud.SessionEpoch != 1 || ud.Flags != 0 {
		t.Fatalf("user data %+v", ud)
	}
	if info.ReconnectTicket == "" || !info.ReconnectTicketExpiresAt.Equal(t0.Add(5*time.Minute)) {
		t.Fatalf("ticket: %+v", info)
	}

	_, err = f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{ZoneID: 5})
	if rpc.CodeOf(err) != rpc.CodeInvalidArgument {
		t.Fatalf("unknown zone: %v", err)
	}
	_, err = f.svc.CreateSession(ctx, &authn.Principal{AccountID: 42}, &CreateSessionRequest{})
	if rpc.CodeOf(err) != rpc.CodePermissionDenied {
		t.Fatalf("scope: %v", err)
	}
	bot := &authn.Principal{AccountID: 43, Scope: []string{authn.ScopeGame, authn.ScopeBot}}
	bi, _ := f.svc.CreateSession(ctx, bot, &CreateSessionRequest{})
	if _, _, bud := f.open(t, bi, 1); bud.Flags&connecttoken.FlagBot == 0 {
		t.Fatal("bot flag missing")
	}
	f.accounts.banned[44] = true
	_, err = f.svc.CreateSession(ctx, player(44), &CreateSessionRequest{})
	if rpc.CodeOf(err) != rpc.CodePermissionDenied {
		t.Fatalf("banned: %v", err)
	}
	// Session IDs are random, positive 63-bit values (netcode client_id).
	if info.SessionID == 0 || info.SessionID >= 1<<63 || bi.SessionID == info.SessionID {
		t.Fatalf("session ids %d %d", info.SessionID, bi.SessionID)
	}
}

func TestCharacterIDsAreChecked(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	// Regression: the character ID went into the token unchecked, and gateways and cells trust
	// the token's user data, so any player could claim any character.
	_, err := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{CharacterID: 8})
	if rpc.CodeOf(err) != rpc.CodePermissionDenied {
		t.Fatalf("someone else's character: %v", err)
	}
	if _, err := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{CharacterID: -7}); rpc.CodeOf(err) != rpc.CodeInvalidArgument {
		t.Fatalf("negative character: %v", err)
	}
	if _, err := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{ZoneID: -1}); rpc.CodeOf(err) != rpc.CodeInvalidArgument {
		t.Fatalf("negative zone: %v", err)
	}
	// Without a character service, character IDs are refused rather than trusted.
	g := newSvc(t)
	g.svc.d.Characters = nil
	if _, err := g.svc.CreateSession(ctx, player(42), &CreateSessionRequest{CharacterID: 7}); rpc.CodeOf(err) != rpc.CodeFailedPrecondition {
		t.Fatalf("no character service: %v", err)
	}
	if _, err := g.svc.CreateSession(ctx, player(42), &CreateSessionRequest{}); err != nil {
		t.Fatalf("character-less session: %v", err)
	}
}

func TestBotFlagSurvivesReconnect(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	bot := &authn.Principal{AccountID: 43, Scope: []string{authn.ScopeGame, authn.ScopeBot}}
	first, err := f.svc.CreateSession(ctx, bot, &CreateSessionRequest{})
	if err != nil {
		t.Fatal(err)
	}
	// Regression: tickets did not carry the policy flags, so a bot-scoped session came back
	// from Reconnect as an ordinary player session.
	again, err := f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: first.ReconnectTicket})
	if err != nil {
		t.Fatal(err)
	}
	if _, _, ud := f.open(t, again, 1); ud.Flags&connecttoken.FlagBot == 0 || ud.Flags&connecttoken.FlagReconnect == 0 {
		t.Fatalf("flags after reconnect: %#x", ud.Flags)
	}
	// Also through a gateway-sealed ticket and a Valkey loss (the session is rebuilt from it).
	gw := f.bus.Connect(t, "gateway")
	rctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	res, err := rpc.NATSRequest[SealRequest, SealResponse](rctx, gw, SealSubject("t1"),
		&SealRequest{Sessions: []SealItem{{SessionID: first.SessionID}}})
	if err != nil || len(res.Tickets) != 1 {
		t.Fatalf("seal: %+v %v", res, err)
	}
	f.redis.Mini.FlushAll()
	third, err := f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: res.Tickets[0].Ticket})
	if err != nil {
		t.Fatal(err)
	}
	if _, _, ud := f.open(t, third, 1); ud.Flags&connecttoken.FlagBot == 0 {
		t.Fatalf("bot flag lost through a gateway ticket: %#x", ud.Flags)
	}
}

func TestFullGatewaysNeitherFallBackNorKick(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	live, err := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{})
	if err != nil {
		t.Fatal(err)
	}
	// Registered gateways exist but none has a free slot: no token for the static dev address.
	f.gws.set(GatewayInstance{ProcessID: 1, Address: netip.MustParseAddrPort("10.0.0.1:7777"), KeyID: 1, FreeSlots: 0})
	if _, err := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{}); rpc.CodeOf(err) != rpc.CodeUnavailable {
		t.Fatalf("full gateways: %v", err)
	}
	// The failed attempt must not have superseded the live session...
	if id, _ := f.svc.d.Store.CurrentFor(ctx, 42); id != live.SessionID {
		t.Fatalf("failed CreateSession replaced the live session: %d", id)
	}
	select {
	case m := <-f.ctl:
		t.Fatalf("failed CreateSession kicked someone: %s", m.Data)
	case <-time.After(50 * time.Millisecond):
	}
	// ...and a reconnect that cannot get a gateway keeps its ticket redeemable.
	if _, err := f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: live.ReconnectTicket}); rpc.CodeOf(err) != rpc.CodeUnavailable {
		t.Fatalf("reconnect without gateways: %v", err)
	}
	f.gws.set(GatewayInstance{ProcessID: 1, Address: netip.MustParseAddrPort("10.0.0.1:7777"), KeyID: 1, FreeSlots: 5})
	again, err := f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: live.ReconnectTicket})
	if err != nil || again.SessionEpoch != 2 || again.Gateways[0] != "10.0.0.1:7777" {
		t.Fatalf("ticket after the gateway came back: %+v %v", again, err)
	}
}

func TestSupersedeIsAtomicAndEndAccountKicks(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	st := f.svc.d.Store
	a := &Session{ID: 1001, AccountID: 5, Epoch: 1, CreatedAt: t0}
	if prev, err := st.Create(ctx, a, time.Hour, 5*time.Minute); err != nil || prev != 0 {
		t.Fatalf("create a: %d %v", prev, err)
	}
	if _, err := st.Create(ctx, &Session{ID: 1001, AccountID: 6, CreatedAt: t0}, time.Hour, time.Minute); !errors.Is(err, ErrIDCollision) {
		t.Fatalf("id collision: %v", err)
	}
	b := &Session{ID: 1002, AccountID: 5, Epoch: 1, CreatedAt: t0}
	prev, err := st.Create(ctx, b, time.Hour, 5*time.Minute)
	if err != nil || prev != 1001 {
		t.Fatalf("create b: %d %v", prev, err)
	}
	// The superseded session is ended by the same script, not by a later call that could fail.
	if got, _ := st.Get(ctx, 1001); got == nil || !got.Ended {
		t.Fatalf("superseded session not ended atomically: %+v", got)
	}
	if _, err := st.AdvanceEpoch(ctx, 1, a, time.Hour); !errors.Is(err, ErrEnded) {
		t.Fatalf("superseded reconnect: %v", err)
	}
	// A session that is live but no longer the account's current one cannot be reconnected.
	c := &Session{ID: 1003, AccountID: 7, Epoch: 1, CreatedAt: t0}
	_, _ = st.Create(ctx, c, time.Hour, time.Minute)
	_ = f.redis.Client.Set(ctx, "sess:acct:7", "999", time.Hour).Err()
	if _, err := st.AdvanceEpoch(ctx, 1, c, time.Hour); !errors.Is(err, ErrSuperseded) {
		t.Fatalf("stale session reconnect: %v", err)
	}

	info, _ := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{})
	if err := f.svc.EndAccount(ctx, 42, "banned"); err != nil {
		t.Fatal(err)
	}
	if m := f.nextControl(t, "kick"); m.SessionID != info.SessionID || m.Reason != "banned" {
		t.Fatalf("ban kick: %+v", m)
	}
	if _, err := f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: info.ReconnectTicket}); rpc.CodeOf(err) != rpc.CodeUnauthenticated {
		t.Fatalf("reconnect after ban kick: %v", err)
	}
	if err := f.svc.EndAccount(ctx, 42, "banned"); err != nil {
		t.Fatal("ending an account without a session must succeed")
	}
}

func TestGatewaySelectionAndKeyDraining(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	gw := func(pid int64, addr string, key uint32, free int) GatewayInstance {
		return GatewayInstance{ProcessID: pid, Address: netip.MustParseAddrPort(addr), KeyID: key, FreeSlots: free}
	}
	// Only key-1 instances: the newest generation with capacity is key 1.
	f.gws.set(gw(1, "10.0.0.1:7777", 1, 10), gw(2, "10.0.0.2:7777", 1, 50), gw(3, "10.0.0.3:7777", 1, 0))
	info, _ := f.svc.CreateSession(ctx, player(1), &CreateSessionRequest{})
	if strings.Join(info.Gateways, ",") != "10.0.0.2:7777,10.0.0.1:7777" {
		t.Fatalf("most free slots first, full ones skipped: %v", info.Gateways)
	}
	f.open(t, info, 1)

	// Rotation: key 2 appears and one gateway restarts on it. New tokens go only to key-2
	// instances, sealed with key 2; key-1 instances drain.
	if _, err := f.keys.Add(nil, t0); err != nil {
		t.Fatal(err)
	}
	f.gws.set(gw(1, "10.0.0.1:7777", 1, 10), gw(2, "10.0.0.2:7777", 1, 50), gw(4, "10.0.0.4:7777", 2, 5),
		gw(5, "10.0.0.5:7777", 99, 500)) // unknown key generation: never used
	info, _ = f.svc.CreateSession(ctx, player(2), &CreateSessionRequest{})
	if strings.Join(info.Gateways, ",") != "10.0.0.4:7777" {
		t.Fatalf("drain policy: %v", info.Gateways)
	}
	f.open(t, info, 2)

	// Cap at MaxGateways.
	var many []GatewayInstance
	for i := 0; i < 6; i++ {
		many = append(many, gw(int64(10+i), "10.0.1."+string(rune('1'+i))+":7777", 2, 100-i))
	}
	f.gws.set(many...)
	info, _ = f.svc.CreateSession(ctx, player(3), &CreateSessionRequest{})
	if len(info.Gateways) != 4 || info.Gateways[0] != "10.0.1.1:7777" {
		t.Fatalf("cap: %v", info.Gateways)
	}

	// No registered gateway and no static fallback: unavailable.
	g := newSvc(t, func(c *platform.SessionConfig) { c.Gateways = nil })
	_, err := g.svc.CreateSession(ctx, player(1), &CreateSessionRequest{})
	if rpc.CodeOf(err) != rpc.CodeUnavailable {
		t.Fatalf("no gateways: %v", err)
	}
}

func TestReconnectFlow(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	first, err := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{CharacterID: 7, ZoneID: 1001})
	if err != nil {
		t.Fatal(err)
	}
	f.clk.Advance(time.Minute)
	second, err := f.svc.Reconnect(ctx, "198.51.100.1", &ReconnectRequest{Ticket: first.ReconnectTicket})
	if err != nil {
		t.Fatal(err)
	}
	if second.SessionID != first.SessionID || second.SessionEpoch != 2 {
		t.Fatalf("reconnect: %+v", second)
	}
	_, priv, ud := f.open(t, second, 1)
	if priv.ClientID != first.SessionID || ud.SessionEpoch != 2 || ud.Flags&connecttoken.FlagReconnect == 0 || ud.CharacterID != 7 {
		t.Fatalf("reconnect token user data %+v", ud)
	}
	if c := f.nextControl(t, "session_epoch"); c.SessionID != first.SessionID || c.Epoch != 2 {
		t.Fatalf("old gateway not told: %+v", c)
	}
	// Tickets are single-use.
	_, err = f.svc.Reconnect(ctx, "198.51.100.1", &ReconnectRequest{Ticket: first.ReconnectTicket})
	if rpc.CodeOf(err) != rpc.CodeUnauthenticated {
		t.Fatalf("replayed ticket: %v", err)
	}
	// The newest ticket works; an expired one does not.
	third, err := f.svc.Reconnect(ctx, "198.51.100.1", &ReconnectRequest{Ticket: second.ReconnectTicket})
	if err != nil || third.SessionEpoch != 3 {
		t.Fatalf("chained reconnect: %+v %v", third, err)
	}
	f.clk.Advance(6 * time.Minute)
	_, err = f.svc.Reconnect(ctx, "198.51.100.1", &ReconnectRequest{Ticket: third.ReconnectTicket})
	if rpc.CodeOf(err) != rpc.CodeUnauthenticated || !strings.Contains(err.Error(), "expired") {
		t.Fatalf("expired ticket: %v", err)
	}
	_, err = f.svc.Reconnect(ctx, "198.51.100.1", &ReconnectRequest{Ticket: "garbage"})
	if rpc.CodeOf(err) != rpc.CodeUnauthenticated {
		t.Fatalf("garbage ticket: %v", err)
	}
}

func TestReconnectAfterValkeyLossAndBan(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	first, _ := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{ZoneID: 1001})
	f.redis.Mini.FlushAll() // Valkey is never the truth: the ticket rebuilds the session
	second, err := f.svc.Reconnect(ctx, "198.51.100.2", &ReconnectRequest{Ticket: first.ReconnectTicket})
	if err != nil || second.SessionEpoch != 2 {
		t.Fatalf("rebuild: %+v %v", second, err)
	}
	f.accounts.banned[42] = true
	_, err = f.svc.Reconnect(ctx, "198.51.100.2", &ReconnectRequest{Ticket: second.ReconnectTicket})
	if rpc.CodeOf(err) != rpc.CodePermissionDenied {
		t.Fatalf("banned reconnect: %v", err)
	}
}

func TestSupersedeAndEndSession(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	a, _ := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{})
	b, _ := f.svc.CreateSession(ctx, player(42), &CreateSessionRequest{})
	if c := f.nextControl(t, "kick"); c.SessionID != a.SessionID || c.Reason != "superseded" {
		t.Fatalf("kick: %+v", c)
	}
	_, err := f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: a.ReconnectTicket})
	if rpc.CodeOf(err) != rpc.CodeUnauthenticated {
		t.Fatalf("superseded session reconnect: %v", err)
	}
	if _, err := f.svc.EndSession(ctx, player(42)); err != nil {
		t.Fatal(err)
	}
	if c := f.nextControl(t, "kick"); c.SessionID != b.SessionID || c.Reason != "logout" {
		t.Fatalf("logout kick: %+v", c)
	}
	_, err = f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: b.ReconnectTicket})
	if rpc.CodeOf(err) != rpc.CodeUnauthenticated {
		t.Fatalf("ended session reconnect: %v", err)
	}
	if _, err := f.svc.EndSession(ctx, player(42)); err != nil {
		t.Fatal("EndSession without a session must succeed")
	}
}

func TestSealReconnectTicketsOverNATS(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	a, _ := f.svc.CreateSession(ctx, player(1), &CreateSessionRequest{ZoneID: 1001})
	b, _ := f.svc.CreateSession(ctx, player(2), &CreateSessionRequest{})
	gw := f.bus.Connect(t, "gateway")
	rctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	res, err := rpc.NATSRequest[SealRequest, SealResponse](rctx, gw, SealSubject("t1"), &SealRequest{Sessions: []SealItem{
		{SessionID: a.SessionID, ZoneID: 2002}, {SessionID: b.SessionID}, {SessionID: 123456},
	}})
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Tickets) != 2 || len(res.Missing) != 1 || res.Missing[0] != "123456" {
		t.Fatalf("seal: %+v", res)
	}
	// The gateway-sealed ticket carries the zone the gateway reported and redeems normally.
	got, err := f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: res.Tickets[0].Ticket})
	if err != nil {
		t.Fatal(err)
	}
	if _, _, ud := f.open(t, got, 1); ud.ZoneID != 2002 || ud.SessionEpoch != 2 {
		t.Fatalf("zone from gateway: %+v", ud)
	}
	// Stale copy: the session moved to epoch 2 (the reconnect above); a gateway still serving
	// epoch 1 is told to drop it.
	stale, err := rpc.NATSRequest[SealRequest, SealResponse](rctx, gw, SealSubject("t1"),
		&SealRequest{Sessions: []SealItem{{SessionID: a.SessionID, Epoch: 1}, {SessionID: a.SessionID, Epoch: 2}}})
	if err != nil || len(stale.Missing) != 1 || len(stale.Tickets) != 1 {
		t.Fatalf("stale epoch: %+v %v", stale, err)
	}
	big := make([]SealItem, MaxTicketBatch+1)
	_, err = rpc.NATSRequest[SealRequest, SealResponse](rctx, gw, SealSubject("t1"), &SealRequest{Sessions: big})
	if rpc.CodeOf(err) != rpc.CodeInvalidArgument {
		t.Fatalf("batch cap: %v", err)
	}
}

func TestRateLimitAndHTTP(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	var err error
	for i := 0; i < 6; i++ {
		_, err = f.svc.CreateSession(ctx, player(9), &CreateSessionRequest{})
	}
	if e := rpc.AsError(err); e == nil || e.Code != rpc.CodeResourceExhausted {
		t.Fatalf("per-account limit: %v", err)
	}

	r := chi.NewRouter()
	r.Use(func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
			if req.Header.Get("Authorization") == "Bearer ok" {
				req = req.WithContext(authn.WithPrincipal(req.Context(), player(77)))
			}
			next.ServeHTTP(w, req)
		})
	})
	f.svc.Mount(r, nil)
	call := func(method, auth, body string) (int, map[string]any) {
		req := httptest.NewRequest(http.MethodPost, ServicePath+method, strings.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		if auth != "" {
			req.Header.Set("Authorization", auth)
		}
		w := httptest.NewRecorder()
		r.ServeHTTP(w, req)
		var m map[string]any
		_ = json.Unmarshal(w.Body.Bytes(), &m)
		return w.Code, m
	}
	st, m := call("CreateSession", "", `{}`)
	if st != 401 {
		t.Fatalf("unauthenticated: %d %v", st, m)
	}
	st, m = call("CreateSession", "Bearer ok", `{"zoneId":"1001"}`)
	if st != 200 || m["connectToken"] == nil || m["sessionId"] == nil {
		t.Fatalf("create: %d %v", st, m)
	}
	st, m = call("Reconnect", "", `{"ticket":"`+m["reconnectTicket"].(string)+`"}`)
	if st != 200 || m["sessionEpoch"] != "2" {
		t.Fatalf("reconnect: %d %v", st, m)
	}
	if st, _ = call("EndSession", "Bearer ok", `{}`); st != 200 {
		t.Fatalf("end: %d", st)
	}
}

func TestSealRebuildsRecordsLostWithValkey(t *testing.T) {
	f := newSvc(t)
	ctx := context.Background()
	a, _ := f.svc.CreateSession(ctx, player(1), &CreateSessionRequest{ZoneID: 1001})
	b, _ := f.svc.CreateSession(ctx, player(2), &CreateSessionRequest{})
	c, _ := f.svc.CreateSession(ctx, player(3), &CreateSessionRequest{})
	gw := f.bus.Connect(t, "gateway")
	rctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	seal := func(items ...SealItem) *SealResponse {
		t.Helper()
		res, err := rpc.NATSRequest[SealRequest, SealResponse](rctx, gw, SealSubject("t1"), &SealRequest{Sessions: items})
		if err != nil {
			t.Fatal(err)
		}
		return res
	}
	first := seal(SealItem{SessionID: a.SessionID, Epoch: 1}, SealItem{SessionID: b.SessionID, Epoch: 1},
		SealItem{SessionID: c.SessionID, Epoch: 1})
	if len(first.Tickets) != 3 {
		t.Fatalf("seal: %+v", first)
	}

	// Valkey restarts empty. Regression: every session came back "missing", so each gateway
	// would drop every player at its next 60 s seal. With their latest tickets as evidence the
	// records are rebuilt; a session without evidence, or with a forged or foreign ticket, is not.
	f.redis.Mini.FlushAll()
	f.clk.Advance(time.Minute)
	res := seal(
		SealItem{SessionID: a.SessionID, Epoch: 1, Ticket: first.Tickets[0].Ticket},
		SealItem{SessionID: b.SessionID, Epoch: 1},                                  // no evidence
		SealItem{SessionID: c.SessionID, Epoch: 1, Ticket: first.Tickets[0].Ticket}, // someone else's ticket
		SealItem{SessionID: 777, Ticket: "garbage"},
	)
	if len(res.Tickets) != 1 || res.Tickets[0].SessionID != a.SessionID || len(res.Missing) != 3 {
		t.Fatalf("rebuild: %+v", res)
	}
	got, err := f.svc.d.Store.Get(ctx, a.SessionID)
	if err != nil || got.AccountID != 1 || got.ZoneID != 1001 || got.Epoch != 1 {
		t.Fatalf("rebuilt record: %+v %v", got, err)
	}
	if cur, _ := f.svc.d.Store.CurrentFor(ctx, 1); cur != a.SessionID {
		t.Fatalf("account pointer not rebuilt: %d", cur)
	}
	// The rebuilt session reconnects normally with the fresh ticket.
	again, err := f.svc.Reconnect(ctx, "x", &ReconnectRequest{Ticket: res.Tickets[0].Ticket})
	if err != nil || again.SessionEpoch != 2 {
		t.Fatalf("reconnect after rebuild: %+v %v", again, err)
	}
	// A ticket does not resurrect a session whose account has moved on, nor a banned account's.
	f.redis.Mini.FlushAll()
	newer, _ := f.svc.CreateSession(ctx, player(1), &CreateSessionRequest{})
	f.accounts.mu.Lock()
	f.accounts.banned[3] = true
	f.accounts.mu.Unlock()
	res = seal(SealItem{SessionID: a.SessionID, Ticket: res.Tickets[0].Ticket}, SealItem{SessionID: c.SessionID, Ticket: first.Tickets[2].Ticket})
	if len(res.Tickets) != 0 || len(res.Missing) != 2 {
		t.Fatalf("superseded or banned rebuild: %+v (newer session %d)", res, newer.SessionID)
	}
}
