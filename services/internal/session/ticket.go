package session

import (
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"io"
	"time"

	"golang.org/x/crypto/chacha20poly1305"

	"github.com/PageMastr/scifi-test/services/pkg/keyring"
)

// Ticket is a reconnect ticket (04 §2.4, 05 §1.3): proof that the holder owned session
// SessionID at SessionEpoch, redeemable once for a fresh connect token without the login queue.
type Ticket struct {
	SessionID    uint64
	AccountID    int64
	CharacterID  int64
	ZoneID       int64
	Epoch        uint64
	ContentBuild uint64
	Flags        uint8 // connecttoken policy flags that must survive a reconnect (FlagBot)
	IssuedAt     time.Time
	ExpiresAt    time.Time
}

// Wire format v2 (little-endian), base64url without padding on the wire. Tickets are opaque to
// clients and gateways; only the session service seals and opens them.
//
//	[0]      version (2)
//	[1:5]    key generation ID
//	[5:29]   XChaCha20 nonce
//	[29:]    sealed plaintext (72 B) + 16 B tag, AD = "HELIOS-RT1" ‖ bytes[0:5]
//
// Plaintext: session_id, account_id, character_id, zone_id, epoch, content_build (u64 each),
// issued_at, expires_at (unix seconds, i64), flags (u8), 7 reserved zero bytes.
const (
	ticketVersion   = 2
	ticketHeader    = 5
	ticketPlain     = 72
	ticketSealedLen = ticketHeader + chacha20poly1305.NonceSizeX + ticketPlain + chacha20poly1305.Overhead
)

var ticketAD = []byte("HELIOS-RT1")

// Ticket errors.
var (
	ErrTicketInvalid = errors.New("session: invalid reconnect ticket")
	ErrTicketExpired = errors.New("session: reconnect ticket expired")
)

// TicketSealer seals with the newest key of its ring and opens with any key in it, so tickets
// survive a key rotation for their 5-minute life. Safe for concurrent use.
type TicketSealer struct {
	ring *keyring.Ring
	rnd  io.Reader
}

// NewTicketSealer wraps a keyring ("reconnect-ticket").
func NewTicketSealer(ring *keyring.Ring) *TicketSealer {
	return &TicketSealer{ring: ring, rnd: rand.Reader}
}

// Seal encodes and encrypts t.
func (s *TicketSealer) Seal(t *Ticket) (string, error) {
	key := s.ring.Current()
	aead, err := chacha20poly1305.NewX(key.Secret)
	if err != nil {
		return "", err
	}
	out := make([]byte, ticketHeader+chacha20poly1305.NonceSizeX, ticketSealedLen)
	out[0] = ticketVersion
	binary.LittleEndian.PutUint32(out[1:5], key.ID)
	if _, err := io.ReadFull(s.rnd, out[ticketHeader:]); err != nil {
		return "", err
	}
	plain := make([]byte, 0, ticketPlain)
	le := binary.LittleEndian
	plain = le.AppendUint64(plain, t.SessionID)
	plain = le.AppendUint64(plain, uint64(t.AccountID))
	plain = le.AppendUint64(plain, uint64(t.CharacterID))
	plain = le.AppendUint64(plain, uint64(t.ZoneID))
	plain = le.AppendUint64(plain, t.Epoch)
	plain = le.AppendUint64(plain, t.ContentBuild)
	plain = le.AppendUint64(plain, uint64(t.IssuedAt.Unix()))
	plain = le.AppendUint64(plain, uint64(t.ExpiresAt.Unix()))
	plain = append(plain, t.Flags, 0, 0, 0, 0, 0, 0, 0)
	nonce := append([]byte(nil), out[ticketHeader:]...)
	out = aead.Seal(out, nonce, plain, append(append([]byte(nil), ticketAD...), out[:ticketHeader]...))
	return base64.RawURLEncoding.EncodeToString(out), nil
}

// Open authenticates and decodes a ticket and checks its expiry against now.
func (s *TicketSealer) Open(token string, now time.Time) (*Ticket, error) {
	raw, err := base64.RawURLEncoding.DecodeString(token)
	if err != nil || len(raw) != ticketSealedLen || raw[0] != ticketVersion {
		return nil, ErrTicketInvalid
	}
	key, ok := s.ring.Get(binary.LittleEndian.Uint32(raw[1:5]))
	if !ok {
		return nil, ErrTicketInvalid
	}
	aead, err := chacha20poly1305.NewX(key.Secret)
	if err != nil {
		return nil, err
	}
	nonce := raw[ticketHeader : ticketHeader+chacha20poly1305.NonceSizeX]
	plain, err := aead.Open(nil, nonce, raw[ticketHeader+chacha20poly1305.NonceSizeX:],
		append(append([]byte(nil), ticketAD...), raw[:ticketHeader]...))
	if err != nil {
		return nil, ErrTicketInvalid
	}
	le := binary.LittleEndian
	t := &Ticket{
		SessionID:    le.Uint64(plain[0:]),
		AccountID:    int64(le.Uint64(plain[8:])),
		CharacterID:  int64(le.Uint64(plain[16:])),
		ZoneID:       int64(le.Uint64(plain[24:])),
		Epoch:        le.Uint64(plain[32:]),
		ContentBuild: le.Uint64(plain[40:]),
		IssuedAt:     time.Unix(int64(le.Uint64(plain[48:])), 0).UTC(),
		ExpiresAt:    time.Unix(int64(le.Uint64(plain[56:])), 0).UTC(),
		Flags:        plain[64],
	}
	if !now.Before(t.ExpiresAt) {
		return nil, ErrTicketExpired
	}
	return t, nil
}
