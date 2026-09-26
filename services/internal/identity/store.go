package identity

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"golang.org/x/crypto/blake2b"
)

// Account is a player (or bot) account. It holds no plain-text PII: the e-mail address is
// sealed under the account's DEK and found by its blind index (05 §3.2, §6.6 Phase 0 rule).
type Account struct {
	ID int64
	// EmailCT is the address as entered, sealed with EmailAAD under the account's DEK.
	EmailCT []byte
	// EmailBidx is PIIKeys.EmailIndex of the address: the unique login key.
	EmailBidx     []byte
	Handle        string
	HandleNorm    string
	Discriminator int16
	PasswordHash  string
	IsBot         bool
	BannedUntil   *time.Time // nil = not banned
	BanReason     string
	CreatedAt     time.Time
	UpdatedAt     time.Time
	LastLoginAt   *time.Time
}

// Tag is the display tag "Handle#0042".
func (a *Account) Tag() string { return fmt.Sprintf("%s#%04d", a.Handle, a.Discriminator) }

// BannedAt reports whether the account is banned at time now.
func (a *Account) BannedAt(now time.Time) bool {
	return a.BannedUntil != nil && a.BannedUntil.After(now)
}

// SubjectKey is an account's data key (DEK) as stored: wrapped by the KEK generation
// KEKVersion. WrappedDEK is nil once the key has been shredded (erasure, Phase 3), which makes
// every copy of the account's ciphertext unreadable.
type SubjectKey struct {
	AccountID  int64
	WrappedDEK []byte
	KEKVersion int32
}

// RefreshToken is one link of a rotating refresh-token family. Only the SHA-256 of the token
// is stored.
type RefreshToken struct {
	Hash      []byte
	FamilyID  int64
	AccountID int64
	IssuedAt  time.Time
	ExpiresAt time.Time
	UsedAt    *time.Time
	RevokedAt *time.Time
	// ClientIPCT is the issuing client's IP address sealed under the account's DEK and bound to
	// this token row (RefreshIPAAD); nil when unknown or the key is shredded (05 §6.6).
	ClientIPCT []byte
}

// AuditEntry is one row of the hash-chained audit log (05 §1.17). Rows hold only pseudonymous
// IDs: no IP address (that goes to the login history) and no free text outside Detail's
// structured fields. Detail is canonical JSON.
type AuditEntry struct {
	Seq     int64
	At      time.Time
	Actor   int64 // account that acted (0 = system / anonymous)
	Subject int64 // account acted upon (0 = none)
	Action  string
	Detail  string
	// NoteDigest is BLAKE2b-256 of the entry's sealed audit_note, so crypto-shredding the note
	// leaves the chain verifiable (05 §1.17); nil without a note. Phase 0 writes no notes yet.
	NoteDigest []byte
	PrevHash   [32]byte
	Hash       [32]byte
}

// NewAudit builds an entry; detail is marshalled with sorted keys so the stored text is
// canonical and the chain can be re-verified from the table alone.
func NewAudit(at time.Time, actor, subject int64, action string, detail map[string]any) *AuditEntry {
	d := "{}"
	if len(detail) > 0 {
		if b, err := json.Marshal(detail); err == nil {
			d = string(b)
		}
	}
	return &AuditEntry{At: at.UTC().Truncate(time.Microsecond), Actor: actor, Subject: subject, Action: action, Detail: d}
}

// auditRowFormat is the row encoding version ChainHash writes; WP-0.15r introduced 2, which
// dropped the client IP of format 1 and added the note digest.
const auditRowFormat = 2

// ChainHash computes BLAKE2b-256(prev ‖ canonical row) in row format 2: the format byte,
// fixed-width integers, then length-prefixed action, detail and note digest, so no two rows
// share an encoding.
func ChainHash(prev [32]byte, e *AuditEntry) [32]byte {
	h, _ := blake2b.New256(nil)
	h.Write(prev[:])
	h.Write([]byte{auditRowFormat})
	var buf [8]byte
	for _, v := range []int64{e.Seq, e.At.UTC().UnixMicro(), e.Actor, e.Subject} {
		binary.LittleEndian.PutUint64(buf[:], uint64(v))
		h.Write(buf[:])
	}
	for _, s := range [][]byte{[]byte(e.Action), []byte(e.Detail), e.NoteDigest} {
		binary.LittleEndian.PutUint32(buf[:4], uint32(len(s)))
		h.Write(buf[:4])
		h.Write(s)
	}
	var out [32]byte
	copy(out[:], h.Sum(nil))
	return out
}

// LegacyChainHashV1 is the pre-WP-0.15r row format 1, which also covered the client IP. Only
// the migration that verifies the old chain before re-chaining it without IPs uses it.
func LegacyChainHashV1(prev [32]byte, e *AuditEntry, clientIP string) [32]byte {
	h, _ := blake2b.New256(nil)
	h.Write(prev[:])
	var buf [8]byte
	for _, v := range []int64{e.Seq, e.At.UTC().UnixMicro(), e.Actor, e.Subject} {
		binary.LittleEndian.PutUint64(buf[:], uint64(v))
		h.Write(buf[:])
	}
	for _, s := range []string{e.Action, clientIP, e.Detail} {
		binary.LittleEndian.PutUint32(buf[:4], uint32(len(s)))
		h.Write(buf[:4])
		h.Write([]byte(s))
	}
	var out [32]byte
	copy(out[:], h.Sum(nil))
	return out
}

// LoginHistoryRetention is how long login and IP history is kept (05 §6.6 retention schedule).
const LoginHistoryRetention = 90 * 24 * time.Hour

// LoginEvent is one row of the login and IP history: an IP address seen for an account,
// sealed under the account's DEK and bound to the row (LoginIPAAD). Unknown logins have no
// subject, so their IP is never stored.
type LoginEvent struct {
	ID         int64 // block ID
	AccountID  int64
	Action     string // the audit action it accompanies
	At         time.Time
	ClientIPCT []byte
}

// ErrAuditChainBroken is returned by VerifyAuditChain on the first inconsistent row.
var ErrAuditChainBroken = errors.New("identity: audit chain broken")

// VerifyAuditChain checks that entries (consecutive, ascending seq) chain from prev.
func VerifyAuditChain(prev [32]byte, entries []AuditEntry) error {
	for i := range entries {
		e := &entries[i]
		if e.PrevHash != prev || ChainHash(prev, e) != e.Hash {
			return fmt.Errorf("%w at seq %d", ErrAuditChainBroken, e.Seq)
		}
		if i > 0 && e.Seq != entries[i-1].Seq+1 {
			return fmt.Errorf("%w: gap before seq %d", ErrAuditChainBroken, e.Seq)
		}
		prev = e.Hash
	}
	return nil
}

// Store errors.
var (
	ErrNotFound     = errors.New("identity: not found")
	ErrEmailTaken   = errors.New("identity: email already registered")
	ErrTagTaken     = errors.New("identity: handle and discriminator taken")
	ErrTokenInvalid = errors.New("identity: refresh token invalid or expired")
	ErrTokenReused  = errors.New("identity: refresh token reused; family revoked")
)

// Store persists identity state. Every mutating method takes an optional audit entry that is
// appended in the same transaction, so the audit log never disagrees with the data. All
// methods are safe for concurrent use.
type Store interface {
	// CreateAccount inserts a and its subject key in one transaction; ErrEmailTaken (same
	// EmailBidx) / ErrTagTaken on conflicts.
	CreateAccount(ctx context.Context, a *Account, key *SubjectKey, audit *AuditEntry) error
	AccountByID(ctx context.Context, id int64) (*Account, error)
	// AccountByEmailIndex finds an account by its e-mail blind index (login, 05 §6.6).
	AccountByEmailIndex(ctx context.Context, bidx []byte) (*Account, error)
	// SubjectKey returns the account's wrapped DEK; ErrNotFound if it has none.
	SubjectKey(ctx context.Context, accountID int64) (*SubjectKey, error)

	// AppendLoginEvent records one login-history row.
	AppendLoginEvent(ctx context.Context, e *LoginEvent) error
	// LoginHistory returns an account's login-history rows, oldest first.
	LoginHistory(ctx context.Context, accountID int64) ([]LoginEvent, error)
	// PurgeLoginHistory deletes rows older than before and returns how many went.
	PurgeLoginHistory(ctx context.Context, before time.Time) (int64, error)
	AccountByTag(ctx context.Context, handleNorm string, discriminator int16) (*Account, error)
	SetPasswordHash(ctx context.Context, id int64, hash string, now time.Time) error
	RecordLogin(ctx context.Context, id int64, now time.Time, audit *AuditEntry) error
	// SetBan bans until `until` (nil lifts the ban). A ban also revokes every refresh token.
	SetBan(ctx context.Context, id int64, until *time.Time, reason string, now time.Time, audit *AuditEntry) error

	InsertRefreshToken(ctx context.Context, t *RefreshToken) error
	// RefreshTokenOwner returns the account of the token with hash; ErrTokenInvalid if unknown.
	RefreshTokenOwner(ctx context.Context, hash []byte) (int64, error)
	// ActiveFamily returns the account of a refresh-token family that is still live (it holds a
	// token that is unused, unrevoked and unexpired at now); ErrTokenInvalid otherwise.
	ActiveFamily(ctx context.Context, familyID int64, now time.Time) (accountID int64, err error)
	// ExtendFamily inserts t (FamilyID and AccountID set) as a second live branch of an active
	// family, atomically with respect to family revocation and bans: a revoked or expired family,
	// another account's family or a banned account yield ErrTokenInvalid. Launch-code exchange
	// uses it, so logging out of the launcher also logs out the game client it launched.
	ExtendFamily(ctx context.Context, t *RefreshToken, now time.Time) error
	// RotateRefreshToken consumes the token with oldHash and inserts next in the same family
	// (FamilyID and AccountID are filled in; a non-zero next.AccountID must match the family's,
	// or ErrTokenInvalid). A token that was already used revokes the whole
	// family and returns ErrTokenReused (with reuseAudit appended); unknown, expired or
	// revoked tokens return ErrTokenInvalid. It returns the consumed token. Rotation and
	// revocation of one family are serialized, so a token rotated concurrently with a logout
	// never survives it.
	RotateRefreshToken(ctx context.Context, oldHash []byte, next *RefreshToken, now time.Time, reuseAudit *AuditEntry) (*RefreshToken, error)
	// RevokeFamilyOf revokes the family of the token with hash (logout). ErrTokenInvalid if unknown.
	RevokeFamilyOf(ctx context.Context, hash []byte, now time.Time, audit *AuditEntry) (*RefreshToken, error)

	// AppendAudit appends e, filling Seq, PrevHash and Hash.
	AppendAudit(ctx context.Context, e *AuditEntry) error
	// ListAudit returns up to limit entries with seq > afterSeq in ascending order.
	ListAudit(ctx context.Context, afterSeq int64, limit int) ([]AuditEntry, error)

	Ping(ctx context.Context) error
}
