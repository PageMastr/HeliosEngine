package identity

import (
	"bytes"
	"context"
	"errors"
	"sort"
	"sync"
	"time"
)

// MemStore is an in-memory Store for unit tests and tools. It implements the same semantics as
// the PostgreSQL store (the storetest suite runs against both).
type MemStore struct {
	mu       sync.Mutex
	accounts map[int64]*Account
	keys     map[int64]*SubjectKey
	tokens   map[string]*RefreshToken
	logins   []LoginEvent
	audit    []AuditEntry
	head     [32]byte
}

// NewMemStore returns an empty store.
func NewMemStore() *MemStore {
	return &MemStore{accounts: map[int64]*Account{}, keys: map[int64]*SubjectKey{}, tokens: map[string]*RefreshToken{}}
}

func cloneAccount(a *Account) *Account {
	c := *a
	c.EmailCT, c.EmailBidx, c.BanReasonCT = bytes.Clone(a.EmailCT), bytes.Clone(a.EmailBidx), bytes.Clone(a.BanReasonCT)
	if a.BannedUntil != nil {
		t := *a.BannedUntil
		c.BannedUntil = &t
	}
	if a.LastLoginAt != nil {
		t := *a.LastLoginAt
		c.LastLoginAt = &t
	}
	return &c
}

func (m *MemStore) appendAuditLocked(e *AuditEntry) {
	if e == nil {
		return
	}
	e.Seq = int64(len(m.audit)) + 1
	e.PrevHash = m.head
	e.Hash = ChainHash(e.PrevHash, e)
	m.head = e.Hash
	m.audit = append(m.audit, *e)
}

// CreateAccount implements Store.
func (m *MemStore) CreateAccount(_ context.Context, a *Account, key *SubjectKey, audit *AuditEntry) error {
	if key == nil || key.AccountID != a.ID {
		return errors.New("identity: an account needs its own subject key")
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, dup := m.accounts[a.ID]; dup {
		return errors.New("identity: duplicate account id") // PostgreSQL: account_pkey
	}
	for _, x := range m.accounts {
		if bytes.Equal(x.EmailBidx, a.EmailBidx) {
			return ErrEmailTaken
		}
		if x.HandleNorm == a.HandleNorm && x.Discriminator == a.Discriminator {
			return ErrTagTaken
		}
	}
	m.accounts[a.ID] = cloneAccount(a)
	m.keys[a.ID] = &SubjectKey{AccountID: key.AccountID, WrappedDEK: bytes.Clone(key.WrappedDEK), KEKVersion: key.KEKVersion}
	m.appendAuditLocked(audit)
	return nil
}

func (m *MemStore) find(pred func(*Account) bool) (*Account, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, a := range m.accounts {
		if pred(a) {
			return cloneAccount(a), nil
		}
	}
	return nil, ErrNotFound
}

// AccountByID implements Store.
func (m *MemStore) AccountByID(_ context.Context, id int64) (*Account, error) {
	return m.find(func(a *Account) bool { return a.ID == id })
}

// AccountByEmailIndex implements Store.
func (m *MemStore) AccountByEmailIndex(_ context.Context, bidx []byte) (*Account, error) {
	if len(bidx) == 0 {
		return nil, ErrNotFound
	}
	return m.find(func(a *Account) bool { return bytes.Equal(a.EmailBidx, bidx) })
}

// SubjectKey implements Store.
func (m *MemStore) SubjectKey(_ context.Context, accountID int64) (*SubjectKey, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	k, ok := m.keys[accountID]
	if !ok {
		return nil, ErrNotFound
	}
	c := *k
	c.WrappedDEK = bytes.Clone(k.WrappedDEK)
	return &c, nil
}

// AppendLoginEvent implements Store.
func (m *MemStore) AppendLoginEvent(_ context.Context, e *LoginEvent) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, x := range m.logins {
		if x.ID == e.ID {
			return errors.New("identity: duplicate login event id")
		}
	}
	c := *e
	c.ClientIPCT = bytes.Clone(e.ClientIPCT)
	m.logins = append(m.logins, c)
	return nil
}

// LoginHistory implements Store.
func (m *MemStore) LoginHistory(_ context.Context, accountID int64) ([]LoginEvent, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []LoginEvent
	for _, e := range m.logins {
		if e.AccountID == accountID {
			c := e
			c.ClientIPCT = bytes.Clone(e.ClientIPCT)
			out = append(out, c)
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		if !out[i].At.Equal(out[j].At) {
			return out[i].At.Before(out[j].At)
		}
		return out[i].ID < out[j].ID
	})
	return out, nil
}

// PurgeLoginHistory implements Store.
func (m *MemStore) PurgeLoginHistory(_ context.Context, before time.Time) (int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	kept := m.logins[:0]
	var n int64
	for _, e := range m.logins {
		if e.At.Before(before) {
			n++
			continue
		}
		kept = append(kept, e)
	}
	m.logins = kept
	for _, t := range m.tokens {
		if t.ClientIPCT != nil && t.IssuedAt.Before(before) {
			t.ClientIPCT = nil
			n++
		}
	}
	return n, nil
}

// RefreshTokenOwner implements Store.
func (m *MemStore) RefreshTokenOwner(_ context.Context, hash []byte) (int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	t, ok := m.tokens[string(hash)]
	if !ok {
		return 0, ErrTokenInvalid
	}
	return t.AccountID, nil
}

// AccountByTag implements Store.
func (m *MemStore) AccountByTag(_ context.Context, handleNorm string, d int16) (*Account, error) {
	return m.find(func(a *Account) bool { return a.HandleNorm == handleNorm && a.Discriminator == d })
}

// SetPasswordHash implements Store.
func (m *MemStore) SetPasswordHash(_ context.Context, id int64, hash string, now time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[id]
	if !ok {
		return ErrNotFound
	}
	a.PasswordHash, a.UpdatedAt = hash, now
	return nil
}

// RecordLogin implements Store.
func (m *MemStore) RecordLogin(_ context.Context, id int64, now time.Time, audit *AuditEntry) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[id]
	if !ok {
		return ErrNotFound
	}
	t := now
	a.LastLoginAt = &t
	m.appendAuditLocked(audit)
	return nil
}

// SetBan implements Store.
func (m *MemStore) SetBan(_ context.Context, id int64, until *time.Time, reasonCT []byte, now time.Time, audit *AuditEntry) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[id]
	if !ok {
		return ErrNotFound
	}
	if until == nil {
		a.BannedUntil, a.BanReasonCT = nil, nil
	} else {
		u := *until
		a.BannedUntil, a.BanReasonCT = &u, bytes.Clone(reasonCT)
		for _, t := range m.tokens {
			if t.AccountID == id && t.RevokedAt == nil {
				n := now
				t.RevokedAt = &n
			}
		}
	}
	a.UpdatedAt = now
	m.appendAuditLocked(audit)
	return nil
}

// InsertRefreshToken implements Store.
func (m *MemStore) InsertRefreshToken(_ context.Context, t *RefreshToken) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	c := *t
	c.Hash, c.ClientIPCT = bytes.Clone(t.Hash), bytes.Clone(t.ClientIPCT)
	m.tokens[string(t.Hash)] = &c
	return nil
}

// ActiveFamily implements Store.
func (m *MemStore) ActiveFamily(_ context.Context, familyID int64, now time.Time) (int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.activeFamilyLocked(familyID, now)
}

func (m *MemStore) activeFamilyLocked(familyID int64, now time.Time) (int64, error) {
	for _, t := range m.tokens {
		if t.FamilyID == familyID && t.RevokedAt == nil && t.UsedAt == nil && t.ExpiresAt.After(now) {
			return t.AccountID, nil
		}
	}
	return 0, ErrTokenInvalid
}

// ExtendFamily implements Store.
func (m *MemStore) ExtendFamily(_ context.Context, t *RefreshToken, now time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	acct, err := m.activeFamilyLocked(t.FamilyID, now)
	if err != nil || acct != t.AccountID {
		return ErrTokenInvalid
	}
	if a, ok := m.accounts[acct]; !ok || a.BannedAt(now) {
		return ErrTokenInvalid
	}
	c := *t
	c.Hash, c.ClientIPCT = bytes.Clone(t.Hash), bytes.Clone(t.ClientIPCT)
	m.tokens[string(t.Hash)] = &c
	return nil
}

func (m *MemStore) revokeFamilyLocked(family int64, now time.Time) {
	for _, t := range m.tokens {
		if t.FamilyID == family && t.RevokedAt == nil {
			n := now
			t.RevokedAt = &n
		}
	}
}

// RotateRefreshToken implements Store.
func (m *MemStore) RotateRefreshToken(_ context.Context, oldHash []byte, next *RefreshToken, now time.Time, reuseAudit *AuditEntry) (*RefreshToken, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	old, ok := m.tokens[string(oldHash)]
	if !ok || old.RevokedAt != nil {
		return nil, ErrTokenInvalid
	}
	if old.UsedAt != nil {
		m.revokeFamilyLocked(old.FamilyID, now)
		if reuseAudit != nil {
			reuseAudit.Subject = old.AccountID
		}
		m.appendAuditLocked(reuseAudit)
		return nil, ErrTokenReused
	}
	if !old.ExpiresAt.After(now) || (next.AccountID != 0 && next.AccountID != old.AccountID) {
		return nil, ErrTokenInvalid
	}
	n := now
	old.UsedAt = &n
	c := *next
	c.Hash, c.ClientIPCT = bytes.Clone(next.Hash), bytes.Clone(next.ClientIPCT)
	c.FamilyID, c.AccountID = old.FamilyID, old.AccountID
	next.FamilyID, next.AccountID = old.FamilyID, old.AccountID
	m.tokens[string(c.Hash)] = &c
	out := *old
	return &out, nil
}

// RevokeFamilyOf implements Store.
func (m *MemStore) RevokeFamilyOf(_ context.Context, hash []byte, now time.Time, audit *AuditEntry) (*RefreshToken, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	t, ok := m.tokens[string(hash)]
	if !ok {
		return nil, ErrTokenInvalid
	}
	m.revokeFamilyLocked(t.FamilyID, now)
	if audit != nil {
		audit.Subject = t.AccountID
	}
	m.appendAuditLocked(audit)
	out := *t
	return &out, nil
}

// AppendAudit implements Store.
func (m *MemStore) AppendAudit(_ context.Context, e *AuditEntry) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.appendAuditLocked(e)
	return nil
}

// ListAudit implements Store.
func (m *MemStore) ListAudit(_ context.Context, afterSeq int64, limit int) ([]AuditEntry, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	i := sort.Search(len(m.audit), func(i int) bool { return m.audit[i].Seq > afterSeq })
	end := len(m.audit)
	if limit > 0 && i+limit < end {
		end = i + limit
	}
	return append([]AuditEntry(nil), m.audit[i:end]...), nil
}

// Ping implements Store.
func (m *MemStore) Ping(context.Context) error { return nil }
