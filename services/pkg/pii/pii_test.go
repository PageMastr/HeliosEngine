package pii_test

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"errors"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/keyring"
	"github.com/PageMastr/scifi-test/services/pkg/pii"
)

func ring(t *testing.T, purpose string) *keyring.Ring {
	t.Helper()
	r, err := keyring.New(purpose, nil, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func TestSealOpenBindsTheLocation(t *testing.T) {
	dek, err := pii.NewDEK()
	if err != nil {
		t.Fatal(err)
	}
	other, _ := pii.NewDEK()
	if dek == other || dek == (pii.DEK{}) {
		t.Fatal("DEKs must be random")
	}
	aad := pii.AAD("svc_identity.account", "email_ct", 42)
	msg := []byte("Ada@Example.com")
	ct, err := pii.Seal(&dek, aad, msg)
	if err != nil || len(ct) != len(msg)+pii.Overhead || bytes.Contains(ct, msg) {
		t.Fatalf("seal: %x %v", ct, err)
	}
	again, _ := pii.Seal(&dek, aad, msg)
	if bytes.Equal(ct, again) {
		t.Fatal("nonces must be random: equal plaintexts gave equal ciphertexts")
	}
	if pt, err := pii.Open(&dek, aad, ct); err != nil || !bytes.Equal(pt, msg) {
		t.Fatalf("open: %q %v", pt, err)
	}
	// Moving the value to another row, column or table, another key, or any bit flip fails.
	for name, c := range map[string]struct {
		dek *pii.DEK
		aad []byte
		ct  []byte
	}{
		"row":      {&dek, pii.AAD("svc_identity.account", "email_ct", 43), ct},
		"column":   {&dek, pii.AAD("svc_identity.account", "dob_ct", 42), ct},
		"table":    {&dek, pii.AAD("svc_identity.accoun", "temail_ct", 42), ct},
		"key":      {&other, aad, ct},
		"tampered": {&dek, aad, append(append([]byte{}, ct[:len(ct)-1]...), ct[len(ct)-1]^1)},
		"format":   {&dek, aad, append([]byte{2}, ct[1:]...)},
		"short":    {&dek, aad, ct[:pii.Overhead-1]},
		"empty":    {&dek, aad, nil},
	} {
		if _, err := pii.Open(c.dek, c.aad, c.ct); !errors.Is(err, pii.ErrDecrypt) {
			t.Errorf("%s: %v", name, err)
		}
	}
	if _, err := pii.Seal(&dek, aad, make([]byte, pii.MaxPlaintext+1)); err == nil {
		t.Fatal("oversized plaintext accepted")
	}
	dek.Clear()
	if dek != (pii.DEK{}) {
		t.Fatal("Clear")
	}
}

func TestKEKWrapUnwrap(t *testing.T) {
	kr := ring(t, "subject-kek")
	kek, err := pii.NewKEK(kr)
	if err != nil {
		t.Fatal(err)
	}
	dek, _ := pii.NewDEK()
	aad := pii.AAD("svc_identity.subject_key", "wrapped_dek", 7)
	wrapped, ver, err := kek.Wrap(&dek, aad)
	if err != nil || ver != 1 || bytes.Contains(wrapped, dek[:]) {
		t.Fatalf("wrap: %d %v", ver, err)
	}
	got, err := kek.Unwrap(ver, wrapped, aad)
	if err != nil || got != dek {
		t.Fatalf("unwrap round trip: %v", err)
	}

	// A different KEK (another environment's key file, a restored backup's) cannot unwrap it,
	// nor can the right KEK for another subject's row or an unknown version.
	wrong, _ := pii.NewKEK(ring(t, "subject-kek"))
	if _, err := wrong.Unwrap(ver, wrapped, aad); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("wrong KEK: %v", err)
	}
	if _, err := kek.Unwrap(ver, wrapped, pii.AAD("svc_identity.subject_key", "wrapped_dek", 8)); !errors.Is(err, pii.ErrDecrypt) {
		t.Fatalf("other subject: %v", err)
	}
	for _, v := range []int32{0, -1, 2} {
		if _, err := kek.Unwrap(v, wrapped, aad); !errors.Is(err, pii.ErrUnknownKEK) {
			t.Fatalf("version %d: %v", v, err)
		}
	}

	// After a rotation new DEKs use the new generation and old ones still unwrap.
	if _, err := kr.Add(nil, time.Now()); err != nil {
		t.Fatal(err)
	}
	w2, v2, _ := kek.Wrap(&dek, aad)
	if v2 != 2 {
		t.Fatalf("rotated version %d", v2)
	}
	for v, w := range map[int32][]byte{ver: wrapped, v2: w2} {
		if got, err := kek.Unwrap(v, w, aad); err != nil || got != dek {
			t.Fatalf("version %d after rotation: %v", v, err)
		}
	}
	if _, err := pii.NewKEK(&keyring.Ring{}); err == nil {
		t.Fatal("empty KEK ring accepted")
	}
}

func TestBlindIndexIsKeyed(t *testing.T) {
	pepper := ring(t, "email-bidx-pepper")
	bi, err := pii.NewBlindIndex(pepper)
	if err != nil {
		t.Fatal(err)
	}
	const email = "ada@example.com"
	sum := bi.Sum(email)
	// 05 §6.6's construction exactly: HMAC-SHA256(pepper, lower(email)).
	m := hmac.New(sha256.New, pepper.Current().Secret)
	m.Write([]byte(email))
	if !bytes.Equal(sum, m.Sum(nil)) || len(sum) != 32 {
		t.Fatalf("not HMAC-SHA256(pepper, value): %x", sum)
	}
	if !bytes.Equal(sum, bi.Sum(email)) {
		t.Fatal("not deterministic")
	}
	// Keyed: neither a plain hash nor an HMAC under another pepper matches, so a database dump
	// alone does not let anyone test guessed addresses.
	plain := sha256.Sum256([]byte(email))
	other, _ := pii.NewBlindIndex(ring(t, "email-bidx-pepper"))
	unkeyed := hmac.New(sha256.New, nil)
	unkeyed.Write([]byte(email))
	if bytes.Equal(sum, plain[:]) || bytes.Equal(sum, other.Sum(email)) || bytes.Equal(sum, unkeyed.Sum(nil)) {
		t.Fatal("blind index is not keyed by the pepper")
	}
	if bytes.Equal(sum, bi.Sum("ada@example.org")) {
		t.Fatal("different values collide")
	}
	if _, err := pii.NewBlindIndex(nil); err == nil {
		t.Fatal("nil pepper ring accepted")
	}
}

func TestAADIsUnambiguous(t *testing.T) {
	if bytes.Equal(pii.AAD("ab", "c", 1), pii.AAD("a", "bc", 1)) || bytes.Equal(pii.AAD("a", "b", 1), pii.AAD("a", "b", 2)) {
		t.Fatal("AAD encodings collide")
	}
}
