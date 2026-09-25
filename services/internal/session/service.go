// Package session is the Session & connect-token issuer (05 §1.3, 04 §2.3–2.4): it mints
// netcode 1.02 connect tokens for 2–4 gateway instances on the newest shard key, keeps
// `sess:<id>` with its session_epoch in Valkey, seals reconnect tickets for gateways and redeems
// them over HTTPS with an epoch CAS so the old gateway drops the session.
package session

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/netip"
	"sort"
	"strconv"
	"sync"
	"time"

	"github.com/nats-io/nats.go"
	"github.com/prometheus/client_golang/prometheus"

	"github.com/PageMastr/scifi-test/services/internal/platform"
	"github.com/PageMastr/scifi-test/services/pkg/authn"
	"github.com/PageMastr/scifi-test/services/pkg/clock"
	"github.com/PageMastr/scifi-test/services/pkg/connecttoken"
	"github.com/PageMastr/scifi-test/services/pkg/keyring"
	"github.com/PageMastr/scifi-test/services/pkg/ratelimit"
	"github.com/PageMastr/scifi-test/services/pkg/rpc"
)

// MaxTicketBatch bounds one SealReconnectTickets call.
const MaxTicketBatch = 1024

// GatewayInstance is a live gateway as reported by the orchestrator registry.
type GatewayInstance struct {
	ProcessID int64
	Address   netip.AddrPort
	KeyID     uint32
	FreeSlots int
}

// GatewayDirectory lists live gateways (the orchestrator registry in-process).
type GatewayDirectory interface {
	Gateways() []GatewayInstance
}

// AccountChecker refuses banned or deleted accounts (the identity service in-process).
type AccountChecker interface {
	CheckAccount(ctx context.Context, accountID int64) error
}

// ZoneChecker validates a requested zone (the orchestrator directory in-process).
type ZoneChecker interface {
	ZoneExists(zoneID int64) bool
}

// CharacterChecker confirms that an account owns a character before its ID is sealed into a
// connect token: gateways and cells trust the user data, so an unchecked ID would let a client
// play as anyone's character. The character service (Phase 1) implements it.
type CharacterChecker interface {
	CheckCharacter(ctx context.Context, accountID, characterID int64) error
}

// AnyCharacter accepts every positive character ID. Dev only, until the character service
// exists; without a checker the session service refuses character IDs altogether.
type AnyCharacter struct{}

// CheckCharacter implements CharacterChecker.
func (AnyCharacter) CheckCharacter(context.Context, int64, int64) error { return nil }

// Deps configures the service.
type Deps struct {
	Config     platform.SessionConfig
	Shard      string
	ProtocolID uint64
	ShardKeys  *keyring.Ring // netcode private keys, one generation per rotation
	Tickets    *TicketSealer
	Store      *Store
	Accounts   AccountChecker
	Gateways   GatewayDirectory // optional; static Config.Gateways serve only while it lists none
	Zones      ZoneChecker      // optional
	Characters CharacterChecker // optional; nil refuses non-zero character IDs
	Limiter    *ratelimit.Limiter
	// Rand supplies session IDs (crypto/rand when nil). Session IDs are random 63-bit values,
	// not minted IDs: the session service never mints (05 §1.4.5), and unguessable IDs keep
	// session numbers out of reach of anyone watching the ID sequence.
	Rand    io.Reader
	NATS    *nats.Conn // optional in unit tests; needed for gateway control messages
	Clock   clock.Clock
	Log     *slog.Logger
	Metrics prometheus.Registerer
}

// Service implements the session API. Safe for concurrent use.
type Service struct {
	d   Deps
	log *slog.Logger
	m   *metrics

	mu  sync.Mutex
	sub *nats.Subscription
}

type metrics struct {
	issued   *prometheus.CounterVec
	latency  prometheus.Histogram
	sealed   prometheus.Counter
	failures *prometheus.CounterVec
}

func newMetrics(reg prometheus.Registerer) *metrics {
	m := &metrics{
		issued: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "helios_session_tokens_issued_total",
			Help: "Connect tokens minted, by kind (create, reconnect)."}, []string{"kind"}),
		latency: prometheus.NewHistogram(prometheus.HistogramOpts{Name: "helios_session_token_issue_seconds",
			Help: "Connect-token issue latency (SLO p99 < 100 ms, 05 §6.2).", Buckets: prometheus.ExponentialBuckets(0.0005, 2, 12)}),
		sealed: prometheus.NewCounter(prometheus.CounterOpts{Name: "helios_session_tickets_sealed_total",
			Help: "Reconnect tickets sealed for gateways."}),
		failures: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "helios_session_reconnect_failures_total",
			Help: "Refused reconnects by reason."}, []string{"reason"}),
	}
	if reg != nil {
		reg.MustRegister(m.issued, m.latency, m.sealed, m.failures)
	}
	return m
}

// New builds the service.
func New(d Deps) (*Service, error) {
	if d.ShardKeys == nil || d.Tickets == nil || d.Store == nil || d.Accounts == nil || d.Limiter == nil || d.Shard == "" {
		return nil, errors.New("session: missing dependency")
	}
	if d.Rand == nil {
		d.Rand = rand.Reader
	}
	if d.Clock == nil {
		d.Clock = clock.System{}
	}
	if d.Log == nil {
		d.Log = slog.Default()
	}
	return &Service{d: d, log: d.Log, m: newMetrics(d.Metrics)}, nil
}

// Name implements app.Service.
func (s *Service) Name() string { return platform.ServiceSession }

// SealSubject is the NATS subject gateways call every 60 s.
func SealSubject(shard string) string { return "rpc." + shard + ".session.SealReconnectTickets" }

// ControlSubject is where gateways listen for session control (kick, epoch changes).
func ControlSubject(shard, verb string) string { return "ctl." + shard + ".gateway.all." + verb }

// Start implements app.Service.
func (s *Service) Start(context.Context) error {
	if s.d.NATS == nil {
		return nil
	}
	sub, err := rpc.NATSHandle(s.d.NATS, SealSubject(s.d.Shard), "session", s.log, s.SealReconnectTickets)
	if err != nil {
		return err
	}
	s.mu.Lock()
	s.sub = sub
	s.mu.Unlock()
	return nil
}

// Stop implements app.Service.
func (s *Service) Stop(context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sub != nil {
		err := s.sub.Drain()
		s.sub = nil
		return err
	}
	return nil
}

// Health implements app.Service.
func (s *Service) Health(ctx context.Context) error { return s.d.Store.Ping(ctx) }

// --- API messages ---

// CreateSessionRequest asks for a connect token for the authenticated account.
type CreateSessionRequest struct {
	CharacterID int64 `json:"characterId,string,omitempty"`
	ZoneID      int64 `json:"zoneId,string,omitempty"`
}

// ConnectInfo carries a connect token and everything the client needs with it.
type ConnectInfo struct {
	SessionID                uint64    `json:"sessionId,string"`
	SessionEpoch             uint64    `json:"sessionEpoch,string"`
	ConnectToken             string    `json:"connectToken"` // base64 (standard) of the 2048-byte netcode token
	ProtocolID               uint64    `json:"protocolId,string"`
	ExpiresAt                time.Time `json:"expiresAt"`
	TimeoutSeconds           int       `json:"timeoutSeconds"`
	Gateways                 []string  `json:"gateways"`
	ReconnectTicket          string    `json:"reconnectTicket"`
	ReconnectTicketExpiresAt time.Time `json:"reconnectTicketExpiresAt"`
}

// ReconnectRequest redeems a reconnect ticket.
type ReconnectRequest struct {
	Ticket string `json:"ticket"`
}

// SealRequest is the gateways' batched ticket request.
type SealRequest struct {
	Sessions []SealItem `json:"sessions"`
}

// SealItem names one session the gateway serves and the zone it is in now.
type SealItem struct {
	SessionID uint64 `json:"sessionId,string"`
	ZoneID    int64  `json:"zoneId,string,omitempty"`
	// Epoch is the session_epoch of the connection the gateway serves (from the connect token's
	// user data or the last session_epoch control message); 0 = unknown. A record at a higher
	// epoch means the session moved to a newer connection, and the gateway's copy is reported
	// missing.
	Epoch uint64 `json:"epoch,string,omitempty"`
	// Ticket is the latest reconnect ticket this gateway holds for the session. It is only used
	// as evidence to rebuild a record that was lost with Valkey (05 §3.4: Valkey is never the
	// truth), so a Valkey restart does not make every gateway drop every player.
	Ticket string `json:"ticket,omitempty"`
}

// SealResponse carries sealed tickets; ended, superseded or unknown sessions (with no valid
// ticket to rebuild them from) are listed in Missing so the gateway can drop them.
type SealResponse struct {
	Tickets []SealedTicket `json:"tickets"`
	Missing []string       `json:"missing"`
}

// SealedTicket is one ticket for the gateway to send on CONTROL.
type SealedTicket struct {
	SessionID uint64    `json:"sessionId,string"`
	Ticket    string    `json:"ticket"`
	ExpiresAt time.Time `json:"expiresAt"`
}

// Empty is an empty message.
type Empty struct{}

// ControlMessage is published on ctl.<shard>.gateway.all.{kick,session_epoch}.
type ControlMessage struct {
	SessionID uint64 `json:"sessionId,string"`
	Epoch     uint64 `json:"epoch,string,omitempty"`
	Reason    string `json:"reason,omitempty"`
}

// --- gateway selection ---

// pickGateways chooses the key generation and 1..MaxGateways instances for a token: the newest
// key generation that has a live instance with free slots (older-key instances drain, 05 §1.3),
// most free slots first. Only while no gateway is registered at all does it fall back to the
// static list on the current key (a dev box before its gateway started); registered gateways
// that are all full or on unknown keys mean "unavailable", never a token for an address that
// may not exist.
func (s *Service) pickGateways() (connecttoken.Key, []netip.AddrPort, error) {
	max := s.d.Config.MaxGateways
	if s.d.Gateways != nil {
		live := s.d.Gateways.Gateways()
		byKey := map[uint32][]GatewayInstance{}
		for _, g := range live {
			if g.FreeSlots <= 0 {
				continue
			}
			if _, ok := s.d.ShardKeys.Get(g.KeyID); ok {
				byKey[g.KeyID] = append(byKey[g.KeyID], g)
			}
		}
		var best uint32
		for id := range byKey {
			if id > best {
				best = id
			}
		}
		if best != 0 {
			gws := byKey[best]
			sort.SliceStable(gws, func(i, j int) bool {
				if gws[i].FreeSlots != gws[j].FreeSlots {
					return gws[i].FreeSlots > gws[j].FreeSlots
				}
				return gws[i].ProcessID < gws[j].ProcessID
			})
			if len(gws) > max {
				gws = gws[:max]
			}
			addrs := make([]netip.AddrPort, len(gws))
			for i, g := range gws {
				addrs[i] = g.Address
			}
			k, _ := s.d.ShardKeys.Get(best)
			return toKey(k), addrs, nil
		}
		if len(live) > 0 {
			return connecttoken.Key{}, nil, rpc.Errorf(rpc.CodeUnavailable, "all gateways are full; retry shortly")
		}
	}
	if len(s.d.Config.Gateways) == 0 {
		return connecttoken.Key{}, nil, rpc.Errorf(rpc.CodeUnavailable, "no gateway available; retry shortly")
	}
	addrs := make([]netip.AddrPort, 0, len(s.d.Config.Gateways))
	for _, a := range s.d.Config.Gateways {
		ap, err := netip.ParseAddrPort(a)
		if err != nil {
			return connecttoken.Key{}, nil, rpc.Internal(err)
		}
		addrs = append(addrs, ap)
		if len(addrs) == max {
			break
		}
	}
	return toKey(s.d.ShardKeys.Current()), addrs, nil
}

func toKey(e keyring.Entry) connecttoken.Key {
	var k connecttoken.Key
	copy(k[:], e.Secret)
	return k
}

// newSessionID draws a random non-zero 63-bit session ID (netcode client_id).
func (s *Service) newSessionID() (uint64, error) {
	var b [8]byte
	for {
		if _, err := io.ReadFull(s.d.Rand, b[:]); err != nil {
			return 0, err
		}
		if id := binary.LittleEndian.Uint64(b[:]) &^ (1 << 63); id != 0 {
			return id, nil
		}
	}
}

// mint builds the token for sess at its epoch; extra flags (FlagReconnect) are added to the
// session's own policy flags.
func (s *Service) mint(sess *Session, extra uint8) (*connecttoken.Token, []netip.AddrPort, error) {
	key, addrs, err := s.pickGateways()
	if err != nil {
		return nil, nil, err
	}
	return s.mintFor(sess, extra, key, addrs)
}

// mintFor builds the token for sess on already chosen gateways.
func (s *Service) mintFor(sess *Session, extra uint8, key connecttoken.Key, addrs []netip.AddrPort) (*connecttoken.Token, []netip.AddrPort, error) {
	ud := connecttoken.UserData{
		Flags: sess.Flags | extra, AccountID: uint64(sess.AccountID), CharacterID: uint64(sess.CharacterID), SessionEpoch: sess.Epoch,
		ContentBuild: sess.ContentBuild, ZoneID: uint64(sess.ZoneID),
	}
	tok, err := connecttoken.Generate(connecttoken.Params{
		ProtocolID:      s.d.ProtocolID,
		ClientID:        sess.ID,
		PublicAddresses: addrs,
		CreateTime:      s.d.Clock.Now(),
		ExpireSeconds:   int32(s.d.Config.TokenExpiry.D() / time.Second),
		TimeoutSeconds:  int32(s.d.Config.Timeout.D() / time.Second),
		UserData:        ud.Marshal(),
		PrivateKey:      key,
	}, nil)
	if err != nil {
		return nil, nil, rpc.Internal(err)
	}
	return tok, addrs, nil
}

func (s *Service) info(sess *Session, tok *connecttoken.Token, addrs []netip.AddrPort) (*ConnectInfo, error) {
	raw, err := tok.Marshal()
	if err != nil {
		return nil, rpc.Internal(err)
	}
	now := s.d.Clock.Now().UTC()
	t := &Ticket{SessionID: sess.ID, AccountID: sess.AccountID, CharacterID: sess.CharacterID, ZoneID: sess.ZoneID,
		Epoch: sess.Epoch, ContentBuild: sess.ContentBuild, Flags: sess.Flags, IssuedAt: now,
		ExpiresAt: now.Add(s.d.Config.TicketTTL.D())}
	ticket, err := s.d.Tickets.Seal(t)
	if err != nil {
		return nil, rpc.Internal(err)
	}
	gws := make([]string, len(addrs))
	for i, a := range addrs {
		gws[i] = a.String()
	}
	return &ConnectInfo{SessionID: sess.ID, SessionEpoch: sess.Epoch, ConnectToken: base64.StdEncoding.EncodeToString(raw),
		ProtocolID: tok.ProtocolID, ExpiresAt: tok.ExpiresAt(), TimeoutSeconds: int(tok.TimeoutSeconds), Gateways: gws,
		ReconnectTicket: ticket, ReconnectTicketExpiresAt: t.ExpiresAt}, nil
}

func (s *Service) control(verb string, msg ControlMessage) {
	if s.d.NATS == nil {
		return
	}
	data, _ := json.Marshal(msg)
	if err := s.d.NATS.Publish(ControlSubject(s.d.Shard, verb), data); err != nil {
		s.log.Warn("gateway control publish failed", "verb", verb, "err", err)
	}
}

func (s *Service) limit(ctx context.Context, key string, l platform.RateLimit) error {
	res, err := s.d.Limiter.Allow(ctx, key, ratelimit.Limit{Rate: l.Rate, Per: l.Per.D(), Burst: l.Burst})
	if err != nil {
		return rpc.Internal(err)
	}
	if !res.Allowed {
		return &rpc.Error{Code: rpc.CodeResourceExhausted, Message: "too many session requests; retry later", RetryAfter: res.RetryAfter}
	}
	return nil
}

// --- API ---

// CreateSession starts a session for the caller and returns its first connect token. An
// account has one session per shard: a new one supersedes (and kicks) the previous.
func (s *Service) CreateSession(ctx context.Context, p *authn.Principal, req *CreateSessionRequest) (*ConnectInfo, error) {
	start := time.Now()
	if !p.HasScope(authn.ScopeGame) {
		return nil, rpc.Errorf(rpc.CodePermissionDenied, "token lacks the game scope")
	}
	if err := s.limit(ctx, "session:acct:"+strconv.FormatInt(p.AccountID, 10), s.d.Config.CreatePerAccount); err != nil {
		return nil, err
	}
	if err := s.d.Accounts.CheckAccount(ctx, p.AccountID); err != nil {
		return nil, err
	}
	if req.ZoneID < 0 || (req.ZoneID != 0 && s.d.Zones != nil && !s.d.Zones.ZoneExists(req.ZoneID)) {
		return nil, rpc.Errorf(rpc.CodeInvalidArgument, "unknown zone %d", req.ZoneID)
	}
	if req.CharacterID < 0 {
		return nil, rpc.Errorf(rpc.CodeInvalidArgument, "invalid character id")
	}
	if req.CharacterID != 0 {
		if s.d.Characters == nil {
			return nil, rpc.Errorf(rpc.CodeFailedPrecondition, "characters are not available on this shard yet")
		}
		if err := s.d.Characters.CheckCharacter(ctx, p.AccountID, req.CharacterID); err != nil {
			return nil, err
		}
	}
	sess := &Session{AccountID: p.AccountID, CharacterID: req.CharacterID, ZoneID: req.ZoneID,
		ContentBuild: s.d.Config.ContentBuild, Epoch: 1, CreatedAt: s.d.Clock.Now().UTC()}
	if p.HasScope(authn.ScopeBot) {
		sess.Flags |= connecttoken.FlagBot
	}
	// Mint before storing: a failure here (no gateway) must not supersede the live session.
	var (
		prev  uint64
		tok   *connecttoken.Token
		addrs []netip.AddrPort
	)
	for attempt := 0; ; attempt++ {
		id, err := s.newSessionID()
		if err != nil {
			return nil, rpc.Internal(err)
		}
		sess.ID = id
		if tok, addrs, err = s.mint(sess, 0); err != nil {
			return nil, err
		}
		prev, err = s.d.Store.Create(ctx, sess, s.d.Config.SessionTTL.D(), s.d.Config.TicketTTL.D())
		if errors.Is(err, ErrIDCollision) && attempt < 3 {
			continue
		}
		if err != nil {
			return nil, rpc.Errorf(rpc.CodeUnavailable, "session store unavailable")
		}
		break
	}
	if prev != 0 {
		s.control("kick", ControlMessage{SessionID: prev, Reason: "superseded"})
	}
	out, err := s.info(sess, tok, addrs)
	if err != nil {
		return nil, err
	}
	s.m.issued.WithLabelValues("create").Inc()
	s.m.latency.Observe(time.Since(start).Seconds())
	s.log.InfoContext(ctx, "session created", "session", sess.ID, "account", p.AccountID, "gateways", out.Gateways)
	return out, nil
}

// Reconnect redeems a ticket for a fresh token without queueing (R07-P0-3). The epoch CAS
// makes each ticket single-use and tells the previous gateway to drop the session.
func (s *Service) Reconnect(ctx context.Context, clientIP string, req *ReconnectRequest) (*ConnectInfo, error) {
	start := time.Now()
	if err := s.limit(ctx, "session:reconnect:ip:"+clientIP, s.d.Config.ReconnectPerIP); err != nil {
		return nil, err
	}
	t, err := s.d.Tickets.Open(req.Ticket, s.d.Clock.Now())
	if err != nil {
		reason := "invalid"
		if errors.Is(err, ErrTicketExpired) {
			reason = "expired"
		}
		s.m.failures.WithLabelValues(reason).Inc()
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "reconnect ticket %s; log in again", reason)
	}
	if err := s.d.Accounts.CheckAccount(ctx, t.AccountID); err != nil {
		s.m.failures.WithLabelValues("account").Inc()
		return nil, err
	}
	// Choose gateways before the epoch CAS consumes the ticket: "no gateway right now" must
	// leave the ticket redeemable.
	key, gwAddrs, err := s.pickGateways()
	if err != nil {
		return nil, err
	}
	sess := &Session{ID: t.SessionID, AccountID: t.AccountID, CharacterID: t.CharacterID, ZoneID: t.ZoneID,
		ContentBuild: t.ContentBuild, Flags: t.Flags, CreatedAt: t.IssuedAt}
	epoch, err := s.d.Store.AdvanceEpoch(ctx, t.Epoch, sess, s.d.Config.SessionTTL.D())
	switch {
	case errors.Is(err, ErrEpochMismatch):
		s.m.failures.WithLabelValues("superseded").Inc()
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "reconnect ticket already used; log in again")
	case errors.Is(err, ErrEnded), errors.Is(err, ErrSuperseded):
		s.m.failures.WithLabelValues("ended").Inc()
		return nil, rpc.Errorf(rpc.CodeUnauthenticated, "session has ended; log in again")
	case err != nil:
		return nil, rpc.Errorf(rpc.CodeUnavailable, "session store unavailable")
	}
	sess.Epoch = epoch
	tok, addrs, err := s.mintFor(sess, connecttoken.FlagReconnect, key, gwAddrs)
	if err != nil {
		return nil, err
	}
	s.control("session_epoch", ControlMessage{SessionID: sess.ID, Epoch: epoch, Reason: "reconnect"})
	out, err := s.info(sess, tok, addrs)
	if err != nil {
		return nil, err
	}
	s.m.issued.WithLabelValues("reconnect").Inc()
	s.m.latency.Observe(time.Since(start).Seconds())
	return out, nil
}

// SealReconnectTickets is called by gateways every 60 s with the sessions they serve. It also
// slides each session's TTL, so a connected player never expires.
func (s *Service) SealReconnectTickets(ctx context.Context, req *SealRequest) (*SealResponse, error) {
	if len(req.Sessions) > MaxTicketBatch {
		return nil, rpc.Errorf(rpc.CodeInvalidArgument, "at most %d sessions per call", MaxTicketBatch)
	}
	out := &SealResponse{Tickets: []SealedTicket{}, Missing: []string{}}
	now := s.d.Clock.Now().UTC()
	for _, item := range req.Sessions {
		sess, err := s.d.Store.Get(ctx, item.SessionID)
		if errors.Is(err, ErrNotFound) && item.Ticket != "" {
			sess, err = s.restoreFromTicket(ctx, item, now)
		}
		if errors.Is(err, ErrNotFound) || errors.Is(err, ErrSuperseded) || (err == nil && sess.Ended) ||
			(err == nil && item.Epoch != 0 && sess.Epoch > item.Epoch) {
			out.Missing = append(out.Missing, strconv.FormatUint(item.SessionID, 10))
			continue
		}
		if err != nil {
			return nil, rpc.Errorf(rpc.CodeUnavailable, "session store unavailable")
		}
		if item.ZoneID != 0 {
			sess.ZoneID = item.ZoneID
		}
		if err := s.d.Store.Touch(ctx, sess, s.d.Config.SessionTTL.D()); err != nil {
			return nil, rpc.Errorf(rpc.CodeUnavailable, "session store unavailable")
		}
		t := &Ticket{SessionID: sess.ID, AccountID: sess.AccountID, CharacterID: sess.CharacterID, ZoneID: sess.ZoneID,
			Epoch: sess.Epoch, ContentBuild: sess.ContentBuild, Flags: sess.Flags, IssuedAt: now,
			ExpiresAt: now.Add(s.d.Config.TicketTTL.D())}
		sealed, err := s.d.Tickets.Seal(t)
		if err != nil {
			return nil, rpc.Internal(err)
		}
		out.Tickets = append(out.Tickets, SealedTicket{SessionID: sess.ID, Ticket: sealed, ExpiresAt: t.ExpiresAt})
	}
	s.m.sealed.Add(float64(len(out.Tickets)))
	return out, nil
}

// restoreFromTicket rebuilds a session record lost with Valkey from the gateway's latest
// ticket for it. The ticket must be valid, unexpired and for that session; the record is
// recreated at the ticket's epoch unless the account has moved on (ErrSuperseded). Anything else
// reads as ErrNotFound.
func (s *Service) restoreFromTicket(ctx context.Context, item SealItem, now time.Time) (*Session, error) {
	t, err := s.d.Tickets.Open(item.Ticket, now)
	if err != nil || t.SessionID != item.SessionID {
		return nil, ErrNotFound
	}
	if err := s.d.Accounts.CheckAccount(ctx, t.AccountID); err != nil {
		return nil, ErrNotFound
	}
	restored := &Session{ID: t.SessionID, AccountID: t.AccountID, CharacterID: t.CharacterID, ZoneID: t.ZoneID,
		ContentBuild: t.ContentBuild, Epoch: t.Epoch, Flags: t.Flags, CreatedAt: t.IssuedAt}
	if err := s.d.Store.Restore(ctx, restored, s.d.Config.SessionTTL.D()); err != nil {
		return nil, err
	}
	s.log.InfoContext(ctx, "session record rebuilt from a gateway ticket", "session", t.SessionID, "epoch", t.Epoch)
	return s.d.Store.Get(ctx, item.SessionID)
}

// EndSession ends the caller's current session (logout from the game) and kicks it.
func (s *Service) EndSession(ctx context.Context, p *authn.Principal) (*Empty, error) {
	if err := s.EndAccount(ctx, p.AccountID, "logout"); err != nil {
		return nil, err
	}
	return &Empty{}, nil
}

// EndAccount ends the account's current session, if any, and tells its gateway to kick it
// (logout, ban). Outstanding reconnect tickets for it stop working at once.
func (s *Service) EndAccount(ctx context.Context, accountID int64, reason string) error {
	id, err := s.d.Store.CurrentFor(ctx, accountID)
	if err != nil {
		return rpc.Errorf(rpc.CodeUnavailable, "session store unavailable")
	}
	if id == 0 {
		return nil
	}
	if _, err := s.d.Store.End(ctx, id, accountID, s.d.Config.TicketTTL.D()); err != nil {
		return rpc.Errorf(rpc.CodeUnavailable, "session store unavailable")
	}
	s.control("kick", ControlMessage{SessionID: id, Reason: reason})
	s.log.InfoContext(ctx, "session ended", "session", id, "account", accountID, "reason", reason)
	return nil
}
