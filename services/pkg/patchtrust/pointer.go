package patchtrust

import (
	"crypto/ed25519"
	"fmt"
	"strings"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
	"github.com/PageMastr/scifi-test/services/pkg/manifest"
)

// ManifestRef names a build's manifest: what a pointer (or its pre-download "next") says to install.
type ManifestRef struct {
	BuildID      string
	ManifestHash cdc.Hash // the manifest's header hash, its identity (engine/patch/README.md)
	CompatEpoch  uint32
}

// Next is a pointer's pre-download target (05 §7, 08 §2.6).
type Next struct {
	ManifestRef
	AvailableAt uint64 // unix seconds
}

// Pointer is a channel's signed pointer, /channels/<product>/<channel>/<platform>.json on the CDN (05 §7).
//
//	{"build_id":S,"cdn_hosts":[S,...],"channel":S,"compat_epoch":N,"expires":N,"key_id":H16,
//	 "manifest_hash":H32,"min_client":S,"min_launcher":S,"next":{"available_at":N,"build_id":S,
//	 "compat_epoch":N,"manifest_hash":H32},"platform":S,"product_id":S,"rollback":B,"rollout_pct":N,
//	 "sequence":N,"sig":H64,"signed_at":N}
//
// "next" is optional. 05 §7's fields plus product_id, channel and platform (so a pointer verifies only
// where it was published), key_id (the signing subkey) and signed_at (checked against the subkey's
// validity). Sig is the subkey's Ed25519 signature over pointerContext followed by the document without
// its "sig" member.
type Pointer struct {
	ProductID   string
	Channel     string // ^[a-z][a-z0-9-]{1,31}$: live, ptr, beta, dev, qa (05 §7)
	Platform    string
	ManifestRef        // build_id, manifest_hash, compat_epoch
	Sequence    uint64 // 1..MaxInt; monotonic per product, channel and platform
	MinLauncher string // a dotted version, ^[0-9]{1,9}(\.[0-9]{1,9}){0,3}$
	MinClient   string
	CDNHosts    []string // 0..MaxCDNHosts base URLs: https://, or http:// on a loopback host
	Next        *Next
	RolloutPct  uint8  // 0..100
	Rollback    bool   // lets Sequence be lower than an install's stored one
	SignedAt    uint64 // unix seconds
	Expires     uint64 // unix seconds; SignedAt < Expires <= SignedAt + MaxPointerLifetime
	KeyID       KeyID
	Sig         Signature
}

func validChannel(s string) bool {
	return len(s) >= 2 && validRole(s)
}

func validVersionText(s string) bool {
	if len(s) == 0 || len(s) > MaxVersionText {
		return false
	}
	parts := strings.Split(s, ".")
	if len(parts) > 4 {
		return false
	}
	for _, p := range parts {
		if len(p) == 0 || len(p) > 9 {
			return false
		}
		for i := 0; i < len(p); i++ {
			if p[i] < '0' || p[i] > '9' {
				return false
			}
		}
	}
	return true
}

// authority returns the part of rest (a URL after its "scheme://") before the first '/', '?' or '#'.
func authority(rest string) string {
	if i := strings.IndexAny(rest, "/?#"); i >= 0 {
		return rest[:i]
	}
	return rest
}

// IsLoopbackURL reports whether s is an http:// URL whose authority is exactly localhost, 127.0.0.1 or
// [::1], optionally with a port of 1–5 digits, followed by nothing or a path. A URL with userinfo
// ("http://localhost:1@cdn.example/") is refused: its host is the part after '@'. Pure; any goroutine.
func IsLoopbackURL(s string) bool {
	rest, ok := strings.CutPrefix(s, "http://")
	if !ok || strings.ContainsRune(s, '@') {
		return false
	}
	auth := authority(rest)
	if len(auth) < len(rest) && rest[len(auth)] != '/' {
		return false // a query or fragment right after the authority
	}
	for _, host := range []string{"localhost", "127.0.0.1", "[::1]"} {
		port, ok := strings.CutPrefix(auth, host)
		if !ok {
			continue
		}
		if port == "" {
			return true
		}
		digits, ok := strings.CutPrefix(port, ":")
		if !ok || len(digits) == 0 || len(digits) > 5 {
			return false
		}
		for i := 0; i < len(digits); i++ {
			if digits[i] < '0' || digits[i] > '9' {
				return false
			}
		}
		return true
	}
	return false
}

// validHost accepts https:// URLs with a non-empty authority and http:// URLs on a loopback host (the dev
// CDN, IsLoopbackURL), without spaces or userinfo ('@').
func validHost(s string) bool {
	if len(s) == 0 || len(s) > MaxHostLength || !printable(s) || strings.ContainsAny(s, " @") {
		return false
	}
	if rest, ok := strings.CutPrefix(s, "https://"); ok {
		return authority(rest) != ""
	}
	return IsLoopbackURL(s)
}

func (m *ManifestRef) validate(what string) error {
	if !manifest.ValidBuildID(m.BuildID) {
		return fmt.Errorf("%sbuild_id %q is not a build ID", what, m.BuildID)
	}
	return nil
}

// Validate checks the pointer's own rules (not its signature or lifetime): identifiers, ranges, hosts.
func (p *Pointer) Validate() error {
	switch {
	case !manifest.ValidProductID(p.ProductID):
		return fmt.Errorf("product_id %q is not a product ID", p.ProductID)
	case !validChannel(p.Channel):
		return fmt.Errorf("channel %q is not a channel name", p.Channel)
	case !manifest.ValidPlatform(p.Platform):
		return fmt.Errorf("platform %q is not a platform name", p.Platform)
	case p.Sequence == 0 || p.Sequence > MaxInt:
		return fmt.Errorf("sequence %d is outside 1..%d", p.Sequence, uint64(MaxInt))
	case !validVersionText(p.MinLauncher):
		return fmt.Errorf("min_launcher %q is not a dotted version", p.MinLauncher)
	case !validVersionText(p.MinClient):
		return fmt.Errorf("min_client %q is not a dotted version", p.MinClient)
	case len(p.CDNHosts) > MaxCDNHosts:
		return fmt.Errorf("%d cdn_hosts, the limit is %d", len(p.CDNHosts), MaxCDNHosts)
	case p.RolloutPct > 100:
		return fmt.Errorf("rollout_pct %d is above 100", p.RolloutPct)
	case p.SignedAt > MaxInt || p.Expires > MaxInt:
		return fmt.Errorf("signed_at or expires is above %d", uint64(MaxInt))
	}
	if err := p.ManifestRef.validate(""); err != nil {
		return err
	}
	for _, h := range p.CDNHosts {
		if !validHost(h) {
			return fmt.Errorf("cdn_hosts entry %q is not an https:// or loopback http:// URL", h)
		}
	}
	if p.Next != nil {
		if err := p.Next.validate("next."); err != nil {
			return err
		}
		if p.Next.AvailableAt > MaxInt {
			return fmt.Errorf("next.available_at is above %d", uint64(MaxInt))
		}
	}
	return nil
}

func (p *Pointer) encode(withSig bool) []byte {
	w := writer{b: make([]byte, 0, 640)}
	w.raw("{")
	w.key("build_id", true)
	w.str(p.BuildID)
	w.key("cdn_hosts", false)
	w.raw("[")
	for i, h := range p.CDNHosts {
		if i > 0 {
			w.raw(",")
		}
		w.str(h)
	}
	w.raw("]")
	w.key("channel", false)
	w.str(p.Channel)
	w.key("compat_epoch", false)
	w.uint(uint64(p.CompatEpoch))
	w.key("expires", false)
	w.uint(p.Expires)
	w.key("key_id", false)
	w.hexBytes(p.KeyID[:])
	w.key("manifest_hash", false)
	w.hexBytes(p.ManifestHash[:])
	w.key("min_client", false)
	w.str(p.MinClient)
	w.key("min_launcher", false)
	w.str(p.MinLauncher)
	if p.Next != nil {
		w.key("next", false)
		w.raw("{")
		w.key("available_at", true)
		w.uint(p.Next.AvailableAt)
		w.key("build_id", false)
		w.str(p.Next.BuildID)
		w.key("compat_epoch", false)
		w.uint(uint64(p.Next.CompatEpoch))
		w.key("manifest_hash", false)
		w.hexBytes(p.Next.ManifestHash[:])
		w.raw("}")
	}
	w.key("platform", false)
	w.str(p.Platform)
	w.key("product_id", false)
	w.str(p.ProductID)
	w.key("rollback", false)
	w.boolean(p.Rollback)
	w.key("rollout_pct", false)
	w.uint(uint64(p.RolloutPct))
	w.key("sequence", false)
	w.uint(p.Sequence)
	if withSig {
		w.key("sig", false)
		w.hexBytes(p.Sig[:])
	}
	w.key("signed_at", false)
	w.uint(p.SignedAt)
	w.raw("}")
	return w.b
}

// Marshal returns the canonical document. It fails if the pointer is invalid.
func (p *Pointer) Marshal() ([]byte, error) {
	if err := p.Validate(); err != nil {
		return nil, fmt.Errorf("patchtrust: pointer: %w", err)
	}
	return p.encode(true), nil
}

// SignedMessage returns the bytes the subkey signs: pointerContext, then the document without "sig".
func (p *Pointer) SignedMessage() []byte {
	return append([]byte(pointerContext), p.encode(false)...)
}

// Sign validates the pointer, sets KeyID to the key's fingerprint and signs it.
func (p *Pointer) Sign(key ed25519.PrivateKey) error {
	p.KeyID = Fingerprint(PublicKeyOf(key))
	if err := p.Validate(); err != nil {
		return fmt.Errorf("patchtrust: pointer: %w", err)
	}
	copy(p.Sig[:], ed25519.Sign(key, p.SignedMessage()))
	return nil
}

// u32 reads an integer that must fit 32 bits.
func (r *reader) u32(name string) uint32 {
	v := r.uint()
	if r.err == nil && v > uint64(^uint32(0)) {
		r.fail("%s %d does not fit 32 bits", name, v)
	}
	return uint32(v)
}

// ParsePointer reads a canonical pointer document and validates it; it does not check the signature
// (Verifier.VerifyPointer does). Any other encoding of the same content is refused.
func ParsePointer(b []byte) (*Pointer, error) {
	if len(b) > MaxPointerSize {
		return nil, fmt.Errorf("%d bytes, the limit is %d", len(b), MaxPointerSize)
	}
	r := reader{b: b}
	p := &Pointer{}
	r.lit("{")
	r.key("build_id", true)
	p.BuildID = r.str(maxIDText)
	r.key("cdn_hosts", false)
	r.lit("[")
	for r.err == nil && !r.peek("]") {
		if len(p.CDNHosts) == MaxCDNHosts {
			r.fail("more than %d cdn_hosts", MaxCDNHosts)
			break
		}
		if len(p.CDNHosts) > 0 {
			r.lit(",")
		}
		p.CDNHosts = append(p.CDNHosts, r.str(MaxHostLength))
	}
	r.lit("]")
	r.key("channel", false)
	p.Channel = r.str(maxIDText)
	r.key("compat_epoch", false)
	p.CompatEpoch = r.u32("compat_epoch")
	r.key("expires", false)
	p.Expires = r.uint()
	r.key("key_id", false)
	r.hexBytes(p.KeyID[:])
	r.key("manifest_hash", false)
	r.hexBytes(p.ManifestHash[:])
	r.key("min_client", false)
	p.MinClient = r.str(MaxVersionText)
	r.key("min_launcher", false)
	p.MinLauncher = r.str(MaxVersionText)
	if r.peek(`,"next":`) {
		n := &Next{}
		r.key("next", false)
		r.lit("{")
		r.key("available_at", true)
		n.AvailableAt = r.uint()
		r.key("build_id", false)
		n.BuildID = r.str(maxIDText)
		r.key("compat_epoch", false)
		n.CompatEpoch = r.u32("next.compat_epoch")
		r.key("manifest_hash", false)
		r.hexBytes(n.ManifestHash[:])
		r.lit("}")
		p.Next = n
	}
	r.key("platform", false)
	p.Platform = r.str(maxIDText)
	r.key("product_id", false)
	p.ProductID = r.str(maxIDText)
	r.key("rollback", false)
	p.Rollback = r.boolean()
	r.key("rollout_pct", false)
	if pct := r.uint(); r.err == nil && pct > 100 {
		r.fail("rollout_pct %d is above 100", pct)
	} else {
		p.RolloutPct = uint8(pct)
	}
	r.key("sequence", false)
	p.Sequence = r.uint()
	r.key("sig", false)
	r.hexBytes(p.Sig[:])
	r.key("signed_at", false)
	p.SignedAt = r.uint()
	r.lit("}")
	r.end()
	if r.err != nil {
		return nil, r.err
	}
	if err := p.Validate(); err != nil {
		return nil, err
	}
	return p, nil
}
