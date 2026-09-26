package identity

import (
	"errors"
	"fmt"
	"strings"

	"github.com/PageMastr/scifi-test/services/pkg/keyring"
	"github.com/PageMastr/scifi-test/services/pkg/pii"
)

// Where Identity's encrypted columns live; part of every sealed value's AAD (05 §6.6), so a
// value copied to another row or column does not decrypt.
const (
	TableAccount      = "svc_identity.account"
	TableSubjectKey   = "svc_identity.subject_key"
	TableRefreshToken = "svc_identity.refresh_token"
	TableLoginHistory = "svc_identity.login_history"
	ColumnEmailCT     = "email_ct"
	ColumnDEK         = "wrapped_dek"
	ColumnClientIPCT  = "client_ip_ct"
)

// ErrShredded is returned when an account's DEK has been deleted: its PII is gone for good.
var ErrShredded = errors.New("identity: subject key shredded")

// PIIKeys hold what protects Identity's direct PII (05 §6.6): the KEK that wraps each account's
// DEK and the pepper of the e-mail blind index. Safe for concurrent use.
type PIIKeys struct {
	kek   *pii.KEK
	email *pii.BlindIndex
}

// NewPIIKeys builds the keys from the KEK keyring (every generation that still wraps a DEK) and
// the blind-index pepper keyring (its current generation).
func NewPIIKeys(kek, pepper *keyring.Ring) (*PIIKeys, error) {
	k, err := pii.NewKEK(kek)
	if err != nil {
		return nil, err
	}
	b, err := pii.NewBlindIndex(pepper)
	if err != nil {
		return nil, err
	}
	return &PIIKeys{kek: k, email: b}, nil
}

// EmailIndex returns the blind index of an address: HMAC-SHA256(pepper, lower(trim(email))).
func (k *PIIKeys) EmailIndex(email string) []byte { return k.email.Sum(NormalizeEmail(email)) }

// EmailAAD binds an account's email_ct to its row.
func EmailAAD(accountID int64) []byte { return pii.AAD(TableAccount, ColumnEmailCT, accountID) }

func dekAAD(accountID int64) []byte { return pii.AAD(TableSubjectKey, ColumnDEK, accountID) }

// RefreshIPAAD binds a refresh token's client_ip_ct to its row (the token hash).
func RefreshIPAAD(tokenHash []byte) []byte {
	return pii.AADKey(TableRefreshToken, ColumnClientIPCT, tokenHash)
}

// LoginIPAAD binds a login-history row's client_ip_ct to its row.
func LoginIPAAD(eventID int64) []byte { return pii.AAD(TableLoginHistory, ColumnClientIPCT, eventID) }

// Seal encrypts value under the account's DEK (sk) for the location aad. A shredded key yields
// (nil, nil): there is nothing left to encrypt under, so the value is not stored.
func (k *PIIKeys) Seal(sk *SubjectKey, aad []byte, value string) ([]byte, error) {
	dek, err := k.UnwrapSubjectKey(sk)
	if errors.Is(err, ErrShredded) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	defer dek.Clear()
	return pii.Seal(&dek, aad, []byte(value))
}

// Open decrypts a value Seal wrote for the location aad.
func (k *PIIKeys) Open(sk *SubjectKey, aad, sealed []byte) (string, error) {
	dek, err := k.UnwrapSubjectKey(sk)
	if err != nil {
		return "", err
	}
	defer dek.Clear()
	pt, err := pii.Open(&dek, aad, sealed)
	if err != nil {
		return "", err
	}
	return string(pt), nil
}

// NewSubjectKey creates a fresh DEK for an account and returns it with its wrapped form for
// storage. The caller clears the DEK when done.
func (k *PIIKeys) NewSubjectKey(accountID int64) (pii.DEK, *SubjectKey, error) {
	dek, err := pii.NewDEK()
	if err != nil {
		return pii.DEK{}, nil, err
	}
	wrapped, ver, err := k.kek.Wrap(&dek, dekAAD(accountID))
	if err != nil {
		dek.Clear()
		return pii.DEK{}, nil, err
	}
	return dek, &SubjectKey{AccountID: accountID, WrappedDEK: wrapped, KEKVersion: ver}, nil
}

// UnwrapSubjectKey recovers an account's DEK; ErrShredded once it has been deleted.
func (k *PIIKeys) UnwrapSubjectKey(sk *SubjectKey) (pii.DEK, error) {
	if sk == nil || sk.WrappedDEK == nil {
		return pii.DEK{}, ErrShredded
	}
	return k.kek.Unwrap(sk.KEKVersion, sk.WrappedDEK, dekAAD(sk.AccountID))
}

// EncryptEmail prepares a new account's e-mail columns: a fresh subject key, the address as
// entered sealed under it, and its blind index. Register and the migration that encrypted the
// pre-WP-0.15r plain-text rows both use it.
func (k *PIIKeys) EncryptEmail(accountID int64, email string) (ct, bidx []byte, key *SubjectKey, err error) {
	dek, key, err := k.NewSubjectKey(accountID)
	if err != nil {
		return nil, nil, nil, err
	}
	defer dek.Clear()
	if ct, err = pii.Seal(&dek, EmailAAD(accountID), []byte(strings.TrimSpace(email))); err != nil {
		return nil, nil, nil, err
	}
	return ct, k.EmailIndex(email), key, nil
}

// DecryptEmail opens an account's email_ct with its subject key.
func (k *PIIKeys) DecryptEmail(a *Account, sk *SubjectKey) (string, error) {
	if sk == nil || sk.AccountID != a.ID {
		return "", errors.New("identity: subject key of another account")
	}
	dek, err := k.UnwrapSubjectKey(sk)
	if err != nil {
		return "", err
	}
	defer dek.Clear()
	pt, err := pii.Open(&dek, EmailAAD(a.ID), a.EmailCT)
	if err != nil {
		return "", fmt.Errorf("identity: account %d email: %w", a.ID, err)
	}
	return string(pt), nil
}
