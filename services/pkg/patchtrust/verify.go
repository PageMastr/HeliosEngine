package patchtrust

import (
	"crypto/ed25519"
	"errors"
	"fmt"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
)

// Check names one check of the trust chain. Every rejection is an *Error carrying the check that failed;
// the C++ verifier (helios::patch::TrustCheck) uses the same names, and the shared vectors in
// services/testdata/vectors/trust/ pin which check each hostile case fails.
type Check string

// The checks, in the order the chain runs them (engine/patch/README.md, "Verification order").
const (
	CheckKeysetMalformed     Check = "keyset-malformed"      // not a canonical, valid keyset document
	CheckKeysetRoot          Check = "keyset-root"           // rootEpoch names neither root of the pair
	CheckKeysetRootRatchet   Check = "keyset-root-ratchet"   // signed by a root the install has moved past
	CheckKeysetSignature     Check = "keyset-signature"      // the root's signature does not verify
	CheckKeysetProduct       Check = "keyset-product"        // another product's keyset
	CheckKeysetVersion       Check = "keyset-version"        // version below the stored one
	CheckPointerMalformed    Check = "pointer-malformed"     // not a canonical, valid pointer document
	CheckPointerKeyUnknown   Check = "pointer-key-unknown"   // key_id is not in the keyset (or was revoked)
	CheckPointerKeyRole      Check = "pointer-key-role"      // the subkey's role is not "manifest"
	CheckPointerSignature    Check = "pointer-signature"     // the subkey's signature does not verify
	CheckPointerKeyWindow    Check = "pointer-key-window"    // signed_at outside the subkey's validity
	CheckPointerFuture       Check = "pointer-future"        // signed_at later than now + MaxClockSkew
	CheckPointerLifetime     Check = "pointer-lifetime"      // expires not within 7 days after signed_at
	CheckPointerProduct      Check = "pointer-product"       // another product's pointer
	CheckPointerChannel      Check = "pointer-channel"       // another channel's pointer
	CheckPointerPlatform     Check = "pointer-platform"      // another platform's pointer
	CheckPointerExpired      Check = "pointer-expired"       // now >= expires
	CheckPointerSequence     Check = "pointer-sequence"      // sequence below the stored one, no rollback
	CheckManifestMalformed   Check = "manifest-malformed"    // the .hman header does not read
	CheckManifestHash        Check = "manifest-hash"         // header hash differs from manifest_hash
	CheckManifestKeyUnknown  Check = "manifest-key-unknown"  // keyId is not in the keyset
	CheckManifestKeyRole     Check = "manifest-key-role"     // the subkey's role is not "manifest"
	CheckManifestSignature   Check = "manifest-signature"    // the signature over bytes [0, 256) fails
	CheckManifestKeyWindow   Check = "manifest-key-window"   // createdAt outside the subkey's validity
	CheckManifestFuture      Check = "manifest-future"       // createdAt later than now + MaxClockSkew
	CheckManifestProduct     Check = "manifest-product"      // another product's manifest
	CheckManifestPlatform    Check = "manifest-platform"     // another platform's manifest
	CheckManifestBuild       Check = "manifest-build"        // buildId differs from the pointer's
	CheckManifestCompatEpoch Check = "manifest-compat-epoch" // compatEpoch differs from the pointer's
	CheckManifestExpired     Check = "manifest-expired"      // expiresAt set and now >= expiresAt
	CheckManifestBody        Check = "manifest-body"         // the payload does not decode to a valid body
	CheckChunkMissing        Check = "chunk-missing"         // the chunk object is not on the CDN
	CheckChunkCorrupt        Check = "chunk-corrupt"         // it does not decode to rawSize bytes
	CheckChunkHash           Check = "chunk-hash"            // its bytes do not hash to the chunk ID
)

// ErrRejected is wrapped by every *Error: errors.Is(err, ErrRejected) tells a trust failure from an I/O one.
var ErrRejected = errors.New("patchtrust: rejected")

// Error is a rejection: the check that failed and why.
type Error struct {
	Check  Check
	Detail string
}

func (e *Error) Error() string { return string(e.Check) + ": " + e.Detail }

// Unwrap makes errors.Is(err, ErrRejected) hold.
func (e *Error) Unwrap() error { return ErrRejected }

func reject(c Check, format string, args ...any) error {
	return &Error{Check: c, Detail: fmt.Sprintf(format, args...)}
}

// CheckOf returns the check an error failed, or "" if it is not a rejection.
func CheckOf(err error) Check {
	var e *Error
	if errors.As(err, &e) {
		return e.Check
	}
	return ""
}

// RootPair is a product's root public keys (08 §2.10.3): Current signs keysets of root epoch Epoch, and
// the pre-committed Next those of Epoch+1. In a product build it comes from the stamped product block
// (08 §2.10.4, a later WP); dev tools read it from roots.json.
type RootPair struct {
	Epoch   uint32 // 1..2^32-2
	Current PublicKey
	Next    PublicKey
}

// Validate checks the pair: an epoch in 1..2^32-2, two different keys, and neither of small order
// (IsWeakPublicKey). A zero Next, for a pair whose next root is not yet chosen, is one: with it anyone could
// sign a keyset of epoch Epoch+1 that verifies about one time in four, so a pair must always name a real next
// root.
func (rp RootPair) Validate() error {
	switch {
	case rp.Epoch == 0 || rp.Epoch == ^uint32(0):
		return fmt.Errorf("patchtrust: root epoch %d is outside 1..%d", rp.Epoch, ^uint32(0)-1)
	case rp.Current == rp.Next:
		return errors.New("patchtrust: the current and next roots are the same key")
	case IsWeakPublicKey(rp.Current) || IsWeakPublicKey(rp.Next):
		return errors.New("patchtrust: a root key has small order (zero, the identity or another torsion point)")
	}
	return nil
}

// Target is what an install verifies for: its product, channel and platform.
type Target struct {
	ProductID string
	Channel   string // ^[a-z][a-z0-9-]{1,31}$
	Platform  string
}

// Validate checks the three identifiers (they become CDN path segments).
func (t Target) Validate() error {
	if !manifest.ValidProductID(t.ProductID) || !validChannel(t.Channel) || !manifest.ValidPlatform(t.Platform) {
		return fmt.Errorf("patchtrust: invalid target %q/%q/%q", t.ProductID, t.Channel, t.Platform)
	}
	return nil
}

// State holds the ratchets an install persists (08 §2.5: install.db keeps them; WP-0.17). The zero
// State is a fresh install's.
type State struct {
	RootEpoch       uint32 `json:"rootEpoch"`       // the highest root epoch accepted
	KeysetVersion   uint64 `json:"keysetVersion"`   // the highest keyset version accepted
	PointerSequence uint64 `json:"pointerSequence"` // the last accepted pointer's (a rollback pointer lowers it)
}

// Advance returns the state after accepting this keyset and pointer.
func Advance(st State, ks *Keyset, p *Pointer) State {
	st.RootEpoch = max(st.RootEpoch, ks.RootEpoch)
	st.KeysetVersion = max(st.KeysetVersion, ks.Version)
	if p.Rollback {
		st.PointerSequence = p.Sequence
	} else {
		st.PointerSequence = max(st.PointerSequence, p.Sequence)
	}
	return st
}

// Options adjust a Verifier.
type Options struct {
	// AllowTestKeys accepts the test-only roots of the shared vectors (IsTestOnlyKey). Only tests set it;
	// without it NewVerifier refuses a root pair that contains one.
	AllowTestKeys bool
}

// Verifier checks keysets, pointers, manifests and chunks for one install (05 §7 and 08 §2.5's order).
// It holds no mutable state and is safe for concurrent use.
type Verifier struct {
	target Target
	roots  RootPair
}

// NewVerifier checks the target's identifiers and the root pair (RootPair.Validate, and no test-only root
// unless opts allow it).
func NewVerifier(t Target, roots RootPair, opts Options) (*Verifier, error) {
	if err := t.Validate(); err != nil {
		return nil, err
	}
	if err := roots.Validate(); err != nil {
		return nil, err
	}
	if !opts.AllowTestKeys && (IsTestOnlyKey(roots.Current) || IsTestOnlyKey(roots.Next)) {
		return nil, errors.New("patchtrust: the root pair contains a test-only key")
	}
	return &Verifier{target: t, roots: roots}, nil
}

// Target returns the verifier's target.
func (v *Verifier) Target() Target { return v.target }

func verifySig(pub PublicKey, msg []byte, sig Signature) bool {
	return ed25519.Verify(pub[:], msg, sig[:])
}

// VerifyKeyset checks a keyset document: canonical and valid, signed by the root its rootEpoch names
// (Epoch: current, Epoch+1: next), not below the stored root epoch, for this product, with a version not
// below the stored one.
func (v *Verifier) VerifyKeyset(b []byte, st State) (*Keyset, error) {
	ks, err := ParseKeyset(b)
	if err != nil {
		return nil, reject(CheckKeysetMalformed, "%v", err)
	}
	var root PublicKey
	switch ks.RootEpoch {
	case v.roots.Epoch:
		root = v.roots.Current
	case v.roots.Epoch + 1:
		root = v.roots.Next
	default:
		return nil, reject(CheckKeysetRoot, "rootEpoch %d is neither the current root's (%d) nor the next's",
			ks.RootEpoch, v.roots.Epoch)
	}
	if ks.RootEpoch < st.RootEpoch {
		return nil, reject(CheckKeysetRootRatchet, "signed by root epoch %d; this install accepts %d and later",
			ks.RootEpoch, st.RootEpoch)
	}
	if !verifySig(root, ks.SignedMessage(), ks.Sig) {
		return nil, reject(CheckKeysetSignature, "the root of epoch %d did not sign this keyset", ks.RootEpoch)
	}
	if ks.ProductID != v.target.ProductID {
		return nil, reject(CheckKeysetProduct, "keyset of %q, expected %q", ks.ProductID, v.target.ProductID)
	}
	if ks.Version < st.KeysetVersion {
		return nil, reject(CheckKeysetVersion, "version %d is below the stored %d", ks.Version, st.KeysetVersion)
	}
	return ks, nil
}

// signingKey finds a subkey that may sign pointers and manifests at time t.
func signingKey(ks *Keyset, id KeyID, unknown, role Check) (*KeysetKey, error) {
	key := ks.Key(id)
	if key == nil {
		return nil, reject(unknown, "key %x is not in keyset version %d", id, ks.Version)
	}
	if key.Role != RoleManifest {
		return nil, reject(role, "key %x has role %q, not %q", id, key.Role, RoleManifest)
	}
	return key, nil
}

// inFuture reports whether a signer-claimed time t lies more than MaxClockSkew after now.
func inFuture(t, now uint64) bool { return t > now && t-now > MaxClockSkew }

// VerifyPointer checks a pointer document against a verified keyset: canonical and valid, signed by a
// manifest subkey of the keyset within its validity, signed_at not after now + MaxClockSkew, a lifetime of
// at most 7 days, this install's product, channel and platform, not expired at now (unix seconds), and a
// sequence not below the stored one unless the pointer is signed rollback: true.
func (v *Verifier) VerifyPointer(b []byte, ks *Keyset, now uint64, st State) (*Pointer, error) {
	p, err := ParsePointer(b)
	if err != nil {
		return nil, reject(CheckPointerMalformed, "%v", err)
	}
	key, err := signingKey(ks, p.KeyID, CheckPointerKeyUnknown, CheckPointerKeyRole)
	if err != nil {
		return nil, err
	}
	if !verifySig(key.Pub, p.SignedMessage(), p.Sig) {
		return nil, reject(CheckPointerSignature, "key %x did not sign this pointer", p.KeyID)
	}
	if p.SignedAt < key.NotBefore || p.SignedAt >= key.NotAfter {
		return nil, reject(CheckPointerKeyWindow, "signed at %d, key %x signs in [%d, %d)", p.SignedAt, p.KeyID,
			key.NotBefore, key.NotAfter)
	}
	if inFuture(p.SignedAt, now) {
		return nil, reject(CheckPointerFuture, "signed at %d, more than %d s after now (%d)", p.SignedAt,
			MaxClockSkew, now)
	}
	if p.Expires <= p.SignedAt || p.Expires-p.SignedAt > MaxPointerLifetime {
		return nil, reject(CheckPointerLifetime, "expires %d is not within %d s after signed_at %d", p.Expires,
			MaxPointerLifetime, p.SignedAt)
	}
	switch {
	case p.ProductID != v.target.ProductID:
		return nil, reject(CheckPointerProduct, "pointer of %q, expected %q", p.ProductID, v.target.ProductID)
	case p.Channel != v.target.Channel:
		return nil, reject(CheckPointerChannel, "pointer of channel %q, expected %q", p.Channel, v.target.Channel)
	case p.Platform != v.target.Platform:
		return nil, reject(CheckPointerPlatform, "pointer of platform %q, expected %q", p.Platform,
			v.target.Platform)
	}
	if now >= p.Expires {
		return nil, reject(CheckPointerExpired, "expired at %d, now is %d", p.Expires, now)
	}
	if p.Sequence < st.PointerSequence && !p.Rollback {
		return nil, reject(CheckPointerSequence, "sequence %d is below the stored %d and not a rollback",
			p.Sequence, st.PointerSequence)
	}
	return p, nil
}

// VerifyManifestHeader checks a .hman file's header against a verified keyset and the manifest a verified
// pointer names (Pointer.ManifestRef, or its Next): the header reads, its hash is ref.ManifestHash, a
// manifest subkey signed bytes [0, 256) within its validity (at createdAt, which is not after now +
// MaxClockSkew), and product, platform, build, compat epoch and expiry match. It does not decode the
// payload.
func (v *Verifier) VerifyManifestHeader(file []byte, ks *Keyset, ref ManifestRef, now uint64) (manifest.HeaderInfo,
	error) {
	return verifyManifestHeader(v.target, file, ks, ref, now)
}

func verifyManifestHeader(t Target, file []byte, ks *Keyset, ref ManifestRef, now uint64) (manifest.HeaderInfo,
	error) {
	info, err := manifest.ParseHeader(file, 0)
	if err != nil {
		return info, reject(CheckManifestMalformed, "%v", err)
	}
	h := &info.Header
	if info.HeaderHash != ref.ManifestHash {
		return info, reject(CheckManifestHash, "header hash %x, the pointer names %x", info.HeaderHash[:],
			ref.ManifestHash[:])
	}
	key, err := signingKey(ks, KeyID(h.KeyID), CheckManifestKeyUnknown, CheckManifestKeyRole)
	if err != nil {
		return info, err
	}
	if !verifySig(key.Pub, file[:manifest.SignedBytes], Signature(h.Signature)) {
		return info, reject(CheckManifestSignature, "key %x did not sign this manifest", h.KeyID[:])
	}
	if h.CreatedAt < key.NotBefore || h.CreatedAt >= key.NotAfter {
		return info, reject(CheckManifestKeyWindow, "created at %d, key %x signs in [%d, %d)", h.CreatedAt,
			h.KeyID[:], key.NotBefore, key.NotAfter)
	}
	if inFuture(h.CreatedAt, now) {
		return info, reject(CheckManifestFuture, "created at %d, more than %d s after now (%d)", h.CreatedAt,
			MaxClockSkew, now)
	}
	switch {
	case h.ProductID != t.ProductID:
		return info, reject(CheckManifestProduct, "manifest of %q, expected %q", h.ProductID, t.ProductID)
	case h.Platform != t.Platform:
		return info, reject(CheckManifestPlatform, "manifest of platform %q, expected %q", h.Platform,
			t.Platform)
	case h.BuildID != ref.BuildID:
		return info, reject(CheckManifestBuild, "manifest of build %q, the pointer names %q", h.BuildID, ref.BuildID)
	case h.CompatEpoch != ref.CompatEpoch:
		return info, reject(CheckManifestCompatEpoch, "compat epoch %d, the pointer names %d", h.CompatEpoch,
			ref.CompatEpoch)
	case h.ExpiresAt != 0 && now >= h.ExpiresAt:
		return info, reject(CheckManifestExpired, "expired at %d, now is %d", h.ExpiresAt, now)
	}
	return info, nil
}

// VerifyManifest runs VerifyManifestHeader, then decodes and validates the whole manifest.
func (v *Verifier) VerifyManifest(file []byte, ks *Keyset, ref ManifestRef, now uint64) (*manifest.Manifest, error) {
	return verifyManifest(v.target, file, ks, ref, now)
}

// VerifyManifestWithKeyset runs VerifyManifest's checks for target t against a keyset the caller already
// trusts, without a root pair: a publisher re-pointing a manifest it wrote, under its own signing directory's
// keyset. It never checks the keyset itself, so an install must use a Verifier, which checks the keyset against
// the root pair first.
func VerifyManifestWithKeyset(t Target, file []byte, ks *Keyset, ref ManifestRef, now uint64) (*manifest.Manifest,
	error) {
	if err := t.Validate(); err != nil {
		return nil, err
	}
	return verifyManifest(t, file, ks, ref, now)
}

func verifyManifest(t Target, file []byte, ks *Keyset, ref ManifestRef, now uint64) (*manifest.Manifest, error) {
	if _, err := verifyManifestHeader(t, file, ks, ref, now); err != nil {
		return nil, err
	}
	m, err := manifest.Parse(file, 0)
	if err != nil {
		return nil, reject(CheckManifestBody, "%v", err)
	}
	return m, nil
}

// VerifyChunk checks a chunk's decoded bytes against its manifest entry: rawSize bytes hashing to its ID.
func VerifyChunk(c manifest.Chunk, raw []byte) error {
	if uint64(len(raw)) != uint64(c.RawSize) {
		return reject(CheckChunkCorrupt, "chunk %x is %d bytes, the manifest says %d", c.Hash[:], len(raw), c.RawSize)
	}
	if got := cdc.Sum(raw); got != c.Hash {
		return reject(CheckChunkHash, "chunk %x hashes to %x", c.Hash[:], got[:])
	}
	return nil
}
