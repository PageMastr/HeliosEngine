// Package connecttoken implements the netcode.io v1.02 connect token format ("NETCODE 1.02"),
// byte-compatible with the vendored netcode v1.4.8 in third_party/netcode.
//
// A connect token is a 2048-byte blob the backend hands to a client over HTTPS. Its public part
// tells the client which gateway addresses to try and which keys to use; its 1024-byte private
// part is sealed with XChaCha20-Poly1305 under the shard's private key, so only gateways holding
// that key can open it (04 §2.3, 05 §1.3).
//
// Layout (all integers little-endian, as written by netcode_write_connect_token):
//
//	version_info      13 B   "NETCODE 1.02\0"
//	protocol_id        8 B
//	create_timestamp   8 B   unix seconds
//	expire_timestamp   8 B   unix seconds
//	nonce             24 B   XChaCha20 nonce for the private part
//	private_data    1024 B   sealed private part (1008 B plaintext + 16 B MAC)
//	timeout_seconds    4 B   int32 (negative = never time out)
//	num_addresses      4 B   1..32
//	addresses          n x   {type u8 (1 = IPv4, 2 = IPv6), 4 x u8 | 8 x u16, port u16}
//	client_to_server  32 B
//	server_to_client  32 B
//	zero padding to 2048 B
//
// Private part plaintext (sealed with AD = version_info ‖ protocol_id ‖ expire_timestamp):
//
//	client_id 8 B, timeout_seconds 4 B, num_addresses 4 B, addresses, client_to_server 32 B,
//	server_to_client 32 B, user_data 256 B, zero padding to 1008 B.
//
// The functions in this package are safe for concurrent use; values are not shared.
package connecttoken

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net/netip"
	"time"

	"golang.org/x/crypto/chacha20poly1305"
)

// Sizes and limits of the netcode 1.02 format.
const (
	TokenBytes        = 2048
	PrivateBytes      = 1024
	NonceBytes        = 24
	KeyBytes          = 32
	MACBytes          = 16
	UserDataBytes     = 256
	MaxServers        = 32
	VersionInfoBytes  = 13
	privatePlainBytes = PrivateBytes - MACBytes

	addressIPv4 = 1
	addressIPv6 = 2
)

// VersionInfo is the 13-byte version string (including the terminating NUL) of netcode 1.02.
var VersionInfo = [VersionInfoBytes]byte{'N', 'E', 'T', 'C', 'O', 'D', 'E', ' ', '1', '.', '0', '2', 0}

// Errors returned by the codec. Parse and Open errors deliberately carry no detail an attacker
// could use as an oracle beyond "malformed" vs "does not authenticate".
var (
	ErrMalformed     = errors.New("connecttoken: malformed token")
	ErrDecrypt       = errors.New("connecttoken: private data failed to authenticate")
	ErrTooManyServer = fmt.Errorf("connecttoken: more than %d server addresses", MaxServers)
	ErrNoServers     = errors.New("connecttoken: at least one server address is required")
)

// Key is a 32-byte symmetric key (shard private key or per-session packet key).
type Key [KeyBytes]byte

// Nonce is the 24-byte XChaCha20 nonce of the private part.
type Nonce [NonceBytes]byte

// PrivateData is the decrypted private part. Only the backend and gateways ever see it.
type PrivateData struct {
	ClientID          uint64
	TimeoutSeconds    int32
	ServerAddresses   []netip.AddrPort // internal addresses the gateway compares against its own
	ClientToServerKey Key
	ServerToClientKey Key
	UserData          [UserDataBytes]byte
}

// Token is a parsed connect token. PrivateData stays sealed; use Open to decrypt it.
type Token struct {
	ProtocolID        uint64
	CreateTimestamp   uint64
	ExpireTimestamp   uint64
	Nonce             Nonce
	PrivateData       [PrivateBytes]byte
	TimeoutSeconds    int32
	ServerAddresses   []netip.AddrPort // public addresses the client connects to
	ClientToServerKey Key
	ServerToClientKey Key
}

// Params describes a token to mint.
type Params struct {
	ProtocolID        uint64
	ClientID          uint64
	PublicAddresses   []netip.AddrPort
	InternalAddresses []netip.AddrPort // defaults to PublicAddresses when empty
	CreateTime        time.Time
	// ExpireSeconds is the lifetime after CreateTime. Negative means "never expires"
	// (expire_timestamp = 2^64-1), matching netcode_generate_connect_token.
	ExpireSeconds  int32
	TimeoutSeconds int32
	UserData       [UserDataBytes]byte
	PrivateKey     Key
}

// Generate mints a token. rnd supplies the nonce and the two per-session keys (crypto/rand
// when nil); tests pass a deterministic reader to produce golden vectors.
func Generate(p Params, rnd io.Reader) (*Token, error) {
	if rnd == nil {
		rnd = rand.Reader
	}
	internal := p.InternalAddresses
	if len(internal) == 0 {
		internal = p.PublicAddresses
	}
	if err := checkAddresses(p.PublicAddresses); err != nil {
		return nil, err
	}
	if err := checkAddresses(internal); err != nil {
		return nil, err
	}
	if len(internal) != len(p.PublicAddresses) {
		return nil, errors.New("connecttoken: public and internal address lists differ in length")
	}

	// Same order of random draws as netcode_generate_connect_token: nonce, then c2s, then s2c.
	var nonce Nonce
	priv := PrivateData{
		ClientID:        p.ClientID,
		TimeoutSeconds:  p.TimeoutSeconds,
		ServerAddresses: internal,
		UserData:        p.UserData,
	}
	for _, b := range [][]byte{nonce[:], priv.ClientToServerKey[:], priv.ServerToClientKey[:]} {
		if _, err := io.ReadFull(rnd, b); err != nil {
			return nil, fmt.Errorf("connecttoken: random source: %w", err)
		}
	}

	create := uint64(p.CreateTime.Unix())
	expire := ^uint64(0)
	if p.ExpireSeconds >= 0 {
		expire = create + uint64(p.ExpireSeconds)
	}
	sealed, err := SealPrivate(&priv, p.ProtocolID, expire, &nonce, &p.PrivateKey)
	if err != nil {
		return nil, err
	}
	return &Token{
		ProtocolID:        p.ProtocolID,
		CreateTimestamp:   create,
		ExpireTimestamp:   expire,
		Nonce:             nonce,
		PrivateData:       sealed,
		TimeoutSeconds:    p.TimeoutSeconds,
		ServerAddresses:   append([]netip.AddrPort(nil), p.PublicAddresses...),
		ClientToServerKey: priv.ClientToServerKey,
		ServerToClientKey: priv.ServerToClientKey,
	}, nil
}

// ExpiresAt returns the expiry as a time (zero time for "never").
func (t *Token) ExpiresAt() time.Time {
	if t.ExpireTimestamp == ^uint64(0) {
		return time.Time{}
	}
	return time.Unix(int64(t.ExpireTimestamp), 0).UTC()
}

// Marshal writes the 2048-byte wire form.
func (t *Token) Marshal() ([]byte, error) {
	if err := checkAddresses(t.ServerAddresses); err != nil {
		return nil, err
	}
	w := writer{buf: make([]byte, 0, TokenBytes)}
	w.bytes(VersionInfo[:])
	w.u64(t.ProtocolID)
	w.u64(t.CreateTimestamp)
	w.u64(t.ExpireTimestamp)
	w.bytes(t.Nonce[:])
	w.bytes(t.PrivateData[:])
	w.u32(uint32(t.TimeoutSeconds))
	w.addresses(t.ServerAddresses)
	w.bytes(t.ClientToServerKey[:])
	w.bytes(t.ServerToClientKey[:])
	if len(w.buf) > TokenBytes {
		return nil, ErrMalformed
	}
	return w.buf[:TokenBytes], nil // zero padding: cap is TokenBytes and make() zeroed it
}

// Parse reads a 2048-byte token with the same checks as netcode_read_connect_token.
func Parse(b []byte) (*Token, error) {
	if len(b) != TokenBytes {
		return nil, ErrMalformed
	}
	r := reader{buf: b}
	var t Token
	var version [VersionInfoBytes]byte
	r.bytes(version[:])
	if version != VersionInfo {
		return nil, ErrMalformed
	}
	t.ProtocolID = r.u64()
	t.CreateTimestamp = r.u64()
	t.ExpireTimestamp = r.u64()
	if t.CreateTimestamp > t.ExpireTimestamp {
		return nil, ErrMalformed
	}
	r.bytes(t.Nonce[:])
	r.bytes(t.PrivateData[:])
	t.TimeoutSeconds = int32(r.u32())
	addrs, ok := r.addresses()
	if !ok {
		return nil, ErrMalformed
	}
	t.ServerAddresses = addrs
	r.bytes(t.ClientToServerKey[:])
	r.bytes(t.ServerToClientKey[:])
	if r.err {
		return nil, ErrMalformed
	}
	return &t, nil
}

// Open decrypts and parses the private part with the shard key — what a gateway does when a
// connection request arrives.
func (t *Token) Open(key *Key) (*PrivateData, error) {
	return OpenPrivate(&t.PrivateData, t.ProtocolID, t.ExpireTimestamp, &t.Nonce, key)
}

// SealPrivate serializes and encrypts a private part (netcode_write_connect_token_private +
// netcode_encrypt_connect_token_private).
func SealPrivate(p *PrivateData, protocolID, expireTimestamp uint64, nonce *Nonce, key *Key) ([PrivateBytes]byte, error) {
	var out [PrivateBytes]byte
	if err := checkAddresses(p.ServerAddresses); err != nil {
		return out, err
	}
	w := writer{buf: make([]byte, 0, PrivateBytes)}
	w.u64(p.ClientID)
	w.u32(uint32(p.TimeoutSeconds))
	w.addresses(p.ServerAddresses)
	w.bytes(p.ClientToServerKey[:])
	w.bytes(p.ServerToClientKey[:])
	w.bytes(p.UserData[:])
	if len(w.buf) > privatePlainBytes {
		return out, ErrMalformed
	}
	plain := w.buf[:privatePlainBytes]

	aead, err := chacha20poly1305.NewX(key[:])
	if err != nil {
		return out, err
	}
	ad := additionalData(protocolID, expireTimestamp)
	sealed := aead.Seal(out[:0], nonce[:], plain, ad[:])
	if len(sealed) != PrivateBytes {
		return out, ErrMalformed
	}
	return out, nil
}

// OpenPrivate decrypts and parses a sealed private part (netcode_decrypt_connect_token_private +
// netcode_read_connect_token_private).
func OpenPrivate(sealed *[PrivateBytes]byte, protocolID, expireTimestamp uint64, nonce *Nonce, key *Key) (*PrivateData, error) {
	aead, err := chacha20poly1305.NewX(key[:])
	if err != nil {
		return nil, err
	}
	ad := additionalData(protocolID, expireTimestamp)
	plain, err := aead.Open(make([]byte, 0, privatePlainBytes), nonce[:], sealed[:], ad[:])
	if err != nil {
		return nil, ErrDecrypt
	}
	r := reader{buf: plain}
	var p PrivateData
	p.ClientID = r.u64()
	p.TimeoutSeconds = int32(r.u32())
	addrs, ok := r.addresses()
	if !ok {
		return nil, ErrMalformed
	}
	p.ServerAddresses = addrs
	r.bytes(p.ClientToServerKey[:])
	r.bytes(p.ServerToClientKey[:])
	r.bytes(p.UserData[:])
	if r.err {
		return nil, ErrMalformed
	}
	return &p, nil
}

func additionalData(protocolID, expireTimestamp uint64) [VersionInfoBytes + 16]byte {
	var ad [VersionInfoBytes + 16]byte
	copy(ad[:], VersionInfo[:])
	binary.LittleEndian.PutUint64(ad[VersionInfoBytes:], protocolID)
	binary.LittleEndian.PutUint64(ad[VersionInfoBytes+8:], expireTimestamp)
	return ad
}

func checkAddresses(addrs []netip.AddrPort) error {
	if len(addrs) == 0 {
		return ErrNoServers
	}
	if len(addrs) > MaxServers {
		return ErrTooManyServer
	}
	for _, a := range addrs {
		if !a.IsValid() {
			return fmt.Errorf("connecttoken: invalid server address %v", a)
		}
	}
	return nil
}

// --- little-endian writer/reader ---------------------------------------------------------

type writer struct{ buf []byte }

func (w *writer) bytes(b []byte) { w.buf = append(w.buf, b...) }
func (w *writer) u8(v uint8)     { w.buf = append(w.buf, v) }
func (w *writer) u16(v uint16)   { w.buf = binary.LittleEndian.AppendUint16(w.buf, v) }
func (w *writer) u32(v uint32)   { w.buf = binary.LittleEndian.AppendUint32(w.buf, v) }
func (w *writer) u64(v uint64)   { w.buf = binary.LittleEndian.AppendUint64(w.buf, v) }

// addresses writes the count and each address the way netcode does: IPv4 as four bytes in
// dotted order, IPv6 as eight host-order 16-bit groups each written little-endian.
func (w *writer) addresses(addrs []netip.AddrPort) {
	w.u32(uint32(len(addrs)))
	for _, a := range addrs {
		ip := a.Addr()
		if ip.Is4() {
			w.u8(addressIPv4)
			v4 := ip.As4()
			w.bytes(v4[:])
		} else {
			w.u8(addressIPv6)
			v6 := ip.As16()
			for g := 0; g < 8; g++ {
				w.u16(binary.BigEndian.Uint16(v6[2*g:]))
			}
		}
		w.u16(a.Port())
	}
}

type reader struct {
	buf []byte
	err bool
}

func (r *reader) take(n int) []byte {
	if r.err || len(r.buf) < n {
		r.err = true
		return make([]byte, n)
	}
	b := r.buf[:n]
	r.buf = r.buf[n:]
	return b
}

func (r *reader) bytes(dst []byte) { copy(dst, r.take(len(dst))) }
func (r *reader) u8() uint8        { return r.take(1)[0] }
func (r *reader) u16() uint16      { return binary.LittleEndian.Uint16(r.take(2)) }
func (r *reader) u32() uint32      { return binary.LittleEndian.Uint32(r.take(4)) }
func (r *reader) u64() uint64      { return binary.LittleEndian.Uint64(r.take(8)) }

func (r *reader) addresses() ([]netip.AddrPort, bool) {
	n := int32(r.u32())
	if n <= 0 || n > MaxServers {
		return nil, false
	}
	out := make([]netip.AddrPort, 0, n)
	for i := int32(0); i < n; i++ {
		switch r.u8() {
		case addressIPv4:
			var v4 [4]byte
			r.bytes(v4[:])
			out = append(out, netip.AddrPortFrom(netip.AddrFrom4(v4), r.u16()))
		case addressIPv6:
			var v6 [16]byte
			for g := 0; g < 8; g++ {
				binary.BigEndian.PutUint16(v6[2*g:], r.u16())
			}
			out = append(out, netip.AddrPortFrom(netip.AddrFrom16(v6), r.u16()))
		default:
			return nil, false
		}
	}
	return out, !r.err
}
