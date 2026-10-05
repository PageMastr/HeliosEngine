package patchtrust

import (
	"bytes"
	"crypto/ed25519"
	"fmt"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
)

// Sizes and limits shared with engine/patch (trust.h).
const (
	PublicKeySize  = ed25519.PublicKeySize // 32
	SignatureSize  = ed25519.SignatureSize // 64
	KeyIDSize      = manifest.KeyIDSize    // 16: a subkey ID, as in the .hman header's keyId
	MaxKeysetSize  = 64 << 10              // bytes of keyset.json
	MaxPointerSize = 16 << 10              // bytes of a pointer
	MaxKeys        = 64                    // subkeys in one keyset
	MaxCDNHosts    = 16
	MaxHostLength  = 256
	MaxRoleLength  = 32
	MaxVersionText = 39 // four dotted parts of at most 9 digits
	maxIDText      = 64 // product IDs, channels, platforms and build IDs are shorter

	// MaxPointerLifetime bounds expires - signed_at (08 §2.10.3: pointers expire after 7 days, against
	// freeze attacks). With MaxClockSkew it bounds a pointer's life from the verifier's clock: signed_at is
	// at most now + MaxClockSkew, so a pointer that verifies expires at most 7 days and 1 hour after now,
	// and a subkey past its notAfter cannot sign a pointer that verifies more than that after notAfter.
	MaxPointerLifetime = 7 * 24 * 3600

	// MaxClockSkew is how far a pointer's signed_at or a manifest's createdAt may lie after the verifier's
	// now (the signer's and the install's clocks differ). Without it a signer-claimed future time passes the
	// subkey window check: a pre-staged next-quarter subkey could sign before its notBefore, and a pointer
	// could stay valid for as long past now as its subkey's window allows.
	MaxClockSkew = 3600
)

// The subkey roles of 08 §2.10.3. A keyset may name other roles (a later phase's); they are kept and
// never match a role a check requires.
const (
	RoleManifest = "manifest" // game and launcher pointers and manifests
	RoleNews     = "news"     // news and status banners (08 §2.4)
	RoleAddons   = "addons"   // the addon portal (08 §1.12)
)

// Domain-separation prefixes of the signed messages: a signature over one kind of document never
// verifies as another (manifests sign their binary header, which starts with "HMAN").
const (
	keysetContext  = "HELIOS-KEYSET-V0\n"
	pointerContext = "HELIOS-POINTER-V0\n"
)

type (
	KeyID     [KeyIDSize]byte     // the first 16 bytes of BLAKE2b-256 of the public key
	PublicKey [PublicKeySize]byte // an Ed25519 public key
	Signature [SignatureSize]byte // an Ed25519 signature
)

// Fingerprint is a public key's ID: the first 16 bytes of its BLAKE2b-256.
func Fingerprint(pub PublicKey) KeyID {
	var id KeyID
	h := cdc.Sum(pub[:])
	copy(id[:], h[:KeyIDSize])
	return id
}

// smallOrderKeys are the encodings, with the sign bit cleared, of the eight points of small order on
// edwards25519: y = 0 (order 4), 1 (order 1), the two order-8 y values, p-1 (order 2), and the
// non-canonical p (= 0) and p+1 (= 1) that Go and Monocypher also decode (libsodium's has_small_order list).
var smallOrderKeys = [...]PublicKey{
	{},
	{0x01},
	{0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
		0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05},
	{0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
		0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a},
	{0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
	{0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
	{0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
}

// IsWeakPublicKey reports whether pub encodes a point of small order (1, 2, 4 or 8), in either sign.
// crypto/ed25519 and Monocypher accept such a key, and with it the signature R = identity, S = 0, which
// nobody made, for every message whose challenge is a multiple of the point's order (one in four for the
// all-zero key, a zero-valued PublicKey). The verifiers refuse such keys as roots and as subkeys. Keys are
// public, so the comparison need not be constant-time.
func IsWeakPublicKey(pub PublicKey) bool {
	pub[PublicKeySize-1] &= 0x7f
	for i := range smallOrderKeys {
		if pub == smallOrderKeys[i] {
			return true
		}
	}
	return false
}

// PublicKeyOf returns the public half of an Ed25519 private key.
func PublicKeyOf(priv ed25519.PrivateKey) PublicKey {
	var pub PublicKey
	copy(pub[:], priv.Public().(ed25519.PublicKey))
	return pub
}

// KeysetKey is one subkey of a keyset (08 §2.10.3).
type KeysetKey struct {
	ID        KeyID     // Fingerprint(Pub)
	Role      string    // ^[a-z][a-z0-9-]{0,31}$: RoleManifest, RoleNews, RoleAddons or a later role
	Pub       PublicKey //
	NotBefore uint64    // unix seconds: the first second the key may sign
	NotAfter  uint64    // unix seconds: the first second it may no longer sign (> NotBefore)
}

// Keyset is a product's root-signed list of subkeys, /keys/<productId>/keyset.json on the CDN.
//
//	{"keys":[{"id":H16,"notAfter":N,"notBefore":N,"pub":H32,"role":S},...],"productId":S,"rootEpoch":N,
//	 "sig":H64,"version":N}
//
// RootEpoch says which root signed it (the product block's pair: Epoch is the current root, Epoch+1 the
// next); Version only grows. Sig is the root's Ed25519 signature over keysetContext followed by the
// document without its "sig" member.
type Keyset struct {
	ProductID string
	Version   uint64 // 1..MaxInt
	RootEpoch uint32 // 1..2^32-2
	Keys      []KeysetKey
	Sig       Signature
}

func validRole(s string) bool {
	if len(s) == 0 || len(s) > MaxRoleLength || s[0] < 'a' || s[0] > 'z' {
		return false
	}
	for i := 1; i < len(s); i++ {
		if c := s[i]; !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-') {
			return false
		}
	}
	return true
}

// Validate checks the keyset's own rules (not its signature): identifiers, ranges, 1..MaxKeys keys sorted by
// ID without duplicates, each ID the fingerprint of its key, no key of small order (IsWeakPublicKey: anyone
// could sign under one), NotBefore < NotAfter.
func (k *Keyset) Validate() error {
	switch {
	case !manifest.ValidProductID(k.ProductID):
		return fmt.Errorf("productId %q is not a product ID", k.ProductID)
	case k.Version == 0 || k.Version > MaxInt:
		return fmt.Errorf("version %d is outside 1..%d", k.Version, uint64(MaxInt))
	case k.RootEpoch == 0 || k.RootEpoch == ^uint32(0):
		return fmt.Errorf("rootEpoch %d is outside 1..%d", k.RootEpoch, ^uint32(0)-1)
	case len(k.Keys) == 0 || len(k.Keys) > MaxKeys:
		return fmt.Errorf("%d keys, expected 1..%d", len(k.Keys), MaxKeys)
	}
	for i := range k.Keys {
		key := &k.Keys[i]
		switch {
		case key.ID != Fingerprint(key.Pub):
			return fmt.Errorf("key %x: the ID is not the fingerprint of its public key", key.ID)
		case i > 0 && bytes.Compare(k.Keys[i-1].ID[:], key.ID[:]) >= 0:
			return fmt.Errorf("key %x: keys are not sorted by ID or repeat", key.ID)
		case IsWeakPublicKey(key.Pub):
			return fmt.Errorf("key %x: the public key has small order", key.ID)
		case !validRole(key.Role):
			return fmt.Errorf("key %x: role %q is not a role name", key.ID, key.Role)
		case key.NotBefore >= key.NotAfter || key.NotAfter > MaxInt:
			return fmt.Errorf("key %x: validity [%d, %d) is empty or above %d", key.ID, key.NotBefore,
				key.NotAfter, uint64(MaxInt))
		}
	}
	return nil
}

func (k *Keyset) encode(withSig bool) []byte {
	w := writer{b: make([]byte, 0, 128+160*len(k.Keys))}
	w.raw("{")
	w.key("keys", true)
	w.raw("[")
	for i := range k.Keys {
		key := &k.Keys[i]
		if i > 0 {
			w.raw(",")
		}
		w.raw("{")
		w.key("id", true)
		w.hexBytes(key.ID[:])
		w.key("notAfter", false)
		w.uint(key.NotAfter)
		w.key("notBefore", false)
		w.uint(key.NotBefore)
		w.key("pub", false)
		w.hexBytes(key.Pub[:])
		w.key("role", false)
		w.str(key.Role)
		w.raw("}")
	}
	w.raw("]")
	w.key("productId", false)
	w.str(k.ProductID)
	w.key("rootEpoch", false)
	w.uint(uint64(k.RootEpoch))
	if withSig {
		w.key("sig", false)
		w.hexBytes(k.Sig[:])
	}
	w.key("version", false)
	w.uint(k.Version)
	w.raw("}")
	return w.b
}

// Marshal returns the canonical document. It fails if the keyset is invalid.
func (k *Keyset) Marshal() ([]byte, error) {
	if err := k.Validate(); err != nil {
		return nil, fmt.Errorf("patchtrust: keyset: %w", err)
	}
	return k.encode(true), nil
}

// SignedMessage returns the bytes the root signs: keysetContext, then the document without "sig".
func (k *Keyset) SignedMessage() []byte {
	return append([]byte(keysetContext), k.encode(false)...)
}

// Sign validates the keyset and signs it with a root key.
func (k *Keyset) Sign(root ed25519.PrivateKey) error {
	if err := k.Validate(); err != nil {
		return fmt.Errorf("patchtrust: keyset: %w", err)
	}
	copy(k.Sig[:], ed25519.Sign(root, k.SignedMessage()))
	return nil
}

// Key returns the subkey with this ID, or nil.
func (k *Keyset) Key(id KeyID) *KeysetKey {
	lo, hi := 0, len(k.Keys)
	for lo < hi {
		mid := (lo + hi) / 2
		switch c := bytes.Compare(k.Keys[mid].ID[:], id[:]); {
		case c == 0:
			return &k.Keys[mid]
		case c < 0:
			lo = mid + 1
		default:
			hi = mid
		}
	}
	return nil
}

// ParseKeyset reads a canonical keyset document and validates it; it does not check the signature
// (Verifier.VerifyKeyset does). Any other encoding of the same content is refused.
func ParseKeyset(b []byte) (*Keyset, error) {
	if len(b) > MaxKeysetSize {
		return nil, fmt.Errorf("%d bytes, the limit is %d", len(b), MaxKeysetSize)
	}
	r := reader{b: b}
	k := &Keyset{}
	r.lit("{")
	r.key("keys", true)
	r.lit("[")
	for r.err == nil && !r.peek("]") {
		if len(k.Keys) == MaxKeys {
			r.fail("more than %d keys", MaxKeys)
			break
		}
		if len(k.Keys) > 0 {
			r.lit(",")
		}
		var key KeysetKey
		r.lit("{")
		r.key("id", true)
		r.hexBytes(key.ID[:])
		r.key("notAfter", false)
		key.NotAfter = r.uint()
		r.key("notBefore", false)
		key.NotBefore = r.uint()
		r.key("pub", false)
		r.hexBytes(key.Pub[:])
		r.key("role", false)
		key.Role = r.str(MaxRoleLength)
		r.lit("}")
		k.Keys = append(k.Keys, key)
	}
	r.lit("]")
	r.key("productId", false)
	k.ProductID = r.str(maxIDText)
	r.key("rootEpoch", false)
	epoch := r.uint()
	r.key("sig", false)
	r.hexBytes(k.Sig[:])
	r.key("version", false)
	k.Version = r.uint()
	r.lit("}")
	r.end()
	if r.err != nil {
		return nil, r.err
	}
	if epoch > uint64(^uint32(0)) {
		return nil, fmt.Errorf("rootEpoch %d is outside 1..%d", epoch, ^uint32(0)-1)
	}
	k.RootEpoch = uint32(epoch)
	if err := k.Validate(); err != nil {
		return nil, err
	}
	return k, nil
}
