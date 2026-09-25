package connecttoken

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

// vectorsDir holds the golden vectors shared with the C++ side (05 §8 "Contract" tests).
const vectorsDir = "../../testdata/vectors"

type fixedVector struct {
	Name              string   `json:"name"`
	ProtocolID        string   `json:"protocol_id"`
	CreateTimestamp   string   `json:"create_timestamp"`
	ExpireTimestamp   string   `json:"expire_timestamp"`
	TimeoutSeconds    int32    `json:"timeout_seconds"`
	ClientID          string   `json:"client_id"`
	PublicAddresses   []string `json:"public_addresses"`
	InternalAddresses []string `json:"internal_addresses"`
	PrivateKey        string   `json:"private_key"`
	Nonce             string   `json:"nonce"`
	ClientToServerKey string   `json:"client_to_server_key"`
	ServerToClientKey string   `json:"server_to_client_key"`
	UserData          string   `json:"user_data"`
	Token             string   `json:"token"`
}

type apiVector struct {
	ProtocolID        string   `json:"protocol_id"`
	ClientID          string   `json:"client_id"`
	TimeoutSeconds    int32    `json:"timeout_seconds"`
	ExpireSeconds     uint64   `json:"expire_seconds"`
	PublicAddresses   []string `json:"public_addresses"`
	InternalAddresses []string `json:"internal_addresses"`
	PrivateKey        string   `json:"private_key"`
	UserData          string   `json:"user_data"`
	Token             string   `json:"token"`
}

func loadJSON(t *testing.T, name string, v any) {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(vectorsDir, name))
	if err != nil {
		t.Fatalf("read vector: %v", err)
	}
	if err := json.Unmarshal(b, v); err != nil {
		t.Fatalf("parse vector %s: %v", name, err)
	}
}

func mustHex(t *testing.T, s string, n int) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil || (n > 0 && len(b) != n) {
		t.Fatalf("bad hex (want %d bytes): %v", n, err)
	}
	return b
}

func mustU64(t *testing.T, s string, base int) uint64 {
	t.Helper()
	v, err := strconv.ParseUint(s, base, 64)
	if err != nil {
		t.Fatalf("bad integer %q: %v", s, err)
	}
	return v
}

func mustAddrs(t *testing.T, ss []string) []netip.AddrPort {
	t.Helper()
	out := make([]netip.AddrPort, len(ss))
	for i, s := range ss {
		a, err := netip.ParseAddrPort(s)
		if err != nil {
			t.Fatalf("bad address %q: %v", s, err)
		}
		out[i] = a
	}
	return out
}

func equalAddrs(a, b []netip.AddrPort) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// TestFixedVectorsByteExact regenerates the tokens that netcode's own C writer produced from
// fixed inputs and requires byte-for-byte equality, then decrypts them.
func TestFixedVectorsByteExact(t *testing.T) {
	var file struct {
		Vectors []fixedVector `json:"vectors"`
	}
	loadJSON(t, "netcode_token_fixed.json", &file)
	if len(file.Vectors) < 2 {
		t.Fatalf("expected at least 2 fixed vectors, got %d", len(file.Vectors))
	}
	for _, v := range file.Vectors {
		t.Run(v.Name, func(t *testing.T) {
			var key Key
			copy(key[:], mustHex(t, v.PrivateKey, KeyBytes))
			var ud [UserDataBytes]byte
			copy(ud[:], mustHex(t, v.UserData, UserDataBytes))
			create := mustU64(t, v.CreateTimestamp, 10)
			expire := mustU64(t, v.ExpireTimestamp, 10)
			expireSeconds := int32(-1)
			if expire != ^uint64(0) {
				expireSeconds = int32(expire - create)
			}
			rnd := bytes.NewReader(append(append(mustHex(t, v.Nonce, NonceBytes),
				mustHex(t, v.ClientToServerKey, KeyBytes)...), mustHex(t, v.ServerToClientKey, KeyBytes)...))
			tok, err := Generate(Params{
				ProtocolID:        mustU64(t, v.ProtocolID, 16),
				ClientID:          mustU64(t, v.ClientID, 16),
				PublicAddresses:   mustAddrs(t, v.PublicAddresses),
				InternalAddresses: mustAddrs(t, v.InternalAddresses),
				CreateTime:        time.Unix(int64(create), 0),
				ExpireSeconds:     expireSeconds,
				TimeoutSeconds:    v.TimeoutSeconds,
				UserData:          ud,
				PrivateKey:        key,
			}, rnd)
			if err != nil {
				t.Fatalf("Generate: %v", err)
			}
			got, err := tok.Marshal()
			if err != nil {
				t.Fatalf("Marshal: %v", err)
			}
			want := mustHex(t, v.Token, TokenBytes)
			if !bytes.Equal(got, want) {
				for i := range got {
					if got[i] != want[i] {
						t.Fatalf("token differs from netcode C output at byte %d: got %02x want %02x", i, got[i], want[i])
					}
				}
			}

			parsed, err := Parse(want)
			if err != nil {
				t.Fatalf("Parse: %v", err)
			}
			if parsed.ExpireTimestamp != expire || parsed.CreateTimestamp != create {
				t.Fatalf("timestamps: got %d/%d", parsed.CreateTimestamp, parsed.ExpireTimestamp)
			}
			if !equalAddrs(parsed.ServerAddresses, mustAddrs(t, v.PublicAddresses)) {
				t.Fatalf("public addresses: got %v", parsed.ServerAddresses)
			}
			priv, err := parsed.Open(&key)
			if err != nil {
				t.Fatalf("Open: %v", err)
			}
			if priv.ClientID != mustU64(t, v.ClientID, 16) || priv.TimeoutSeconds != v.TimeoutSeconds {
				t.Fatalf("private header: %+v", priv)
			}
			if !equalAddrs(priv.ServerAddresses, mustAddrs(t, v.InternalAddresses)) {
				t.Fatalf("internal addresses: got %v", priv.ServerAddresses)
			}
			if priv.UserData != ud || priv.ClientToServerKey != parsed.ClientToServerKey ||
				priv.ServerToClientKey != parsed.ServerToClientKey {
				t.Fatal("private payload mismatch")
			}
		})
	}
}

// TestNetcodeAPIVectorDecrypts opens a token minted by netcode_generate_connect_token (random
// nonce and keys) — the exact path the C++ gateway will exercise in reverse.
func TestNetcodeAPIVectorDecrypts(t *testing.T) {
	var v apiVector
	loadJSON(t, "netcode_token_api.json", &v)
	tok, err := Parse(mustHex(t, v.Token, TokenBytes))
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if tok.ProtocolID != mustU64(t, v.ProtocolID, 16) {
		t.Fatalf("protocol id %x", tok.ProtocolID)
	}
	if tok.ExpireTimestamp-tok.CreateTimestamp != v.ExpireSeconds || tok.TimeoutSeconds != v.TimeoutSeconds {
		t.Fatalf("lifetime/timeout mismatch: %d %d", tok.ExpireTimestamp-tok.CreateTimestamp, tok.TimeoutSeconds)
	}
	if !equalAddrs(tok.ServerAddresses, mustAddrs(t, v.PublicAddresses)) {
		t.Fatalf("public addresses %v", tok.ServerAddresses)
	}
	var key Key
	copy(key[:], mustHex(t, v.PrivateKey, KeyBytes))
	priv, err := tok.Open(&key)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if priv.ClientID != mustU64(t, v.ClientID, 16) {
		t.Fatalf("client id %x", priv.ClientID)
	}
	if !equalAddrs(priv.ServerAddresses, mustAddrs(t, v.InternalAddresses)) {
		t.Fatalf("internal addresses %v", priv.ServerAddresses)
	}
	if !bytes.Equal(priv.UserData[:], mustHex(t, v.UserData, UserDataBytes)) {
		t.Fatal("user data mismatch")
	}
	if priv.ClientToServerKey != tok.ClientToServerKey || priv.ServerToClientKey != tok.ServerToClientKey {
		t.Fatal("public and private session keys differ")
	}
}

func testParams(t *testing.T) Params {
	t.Helper()
	var key Key
	if _, err := rand.Read(key[:]); err != nil {
		t.Fatal(err)
	}
	ud := (&UserData{AccountID: 42, CharacterID: 7, SessionEpoch: 3}).Marshal()
	return Params{
		ProtocolID:      0x1234,
		ClientID:        99,
		PublicAddresses: mustAddrs(t, []string{"127.0.0.1:7777", "[::1]:7778"}),
		CreateTime:      time.Unix(1_800_000_000, 0),
		ExpireSeconds:   30,
		TimeoutSeconds:  10,
		UserData:        ud,
		PrivateKey:      key,
	}
}

func TestRoundTrip(t *testing.T) {
	p := testParams(t)
	tok, err := Generate(p, nil)
	if err != nil {
		t.Fatal(err)
	}
	b, err := tok.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != TokenBytes {
		t.Fatalf("len %d", len(b))
	}
	back, err := Parse(b)
	if err != nil {
		t.Fatal(err)
	}
	if back.ExpiresAt() != time.Unix(1_800_000_030, 0).UTC() {
		t.Fatalf("ExpiresAt %v", back.ExpiresAt())
	}
	priv, err := back.Open(&p.PrivateKey)
	if err != nil {
		t.Fatal(err)
	}
	// Internal addresses default to the public list.
	if !equalAddrs(priv.ServerAddresses, p.PublicAddresses) || priv.ClientID != 99 {
		t.Fatalf("private %+v", priv)
	}
	ud, err := UnmarshalUserData(&priv.UserData)
	if err != nil || ud.AccountID != 42 || ud.CharacterID != 7 || ud.SessionEpoch != 3 {
		t.Fatalf("user data %+v %v", ud, err)
	}
	// Two tokens for the same params never share a nonce or session keys.
	tok2, _ := Generate(p, nil)
	if tok2.Nonce == tok.Nonce || tok2.ClientToServerKey == tok.ClientToServerKey {
		t.Fatal("random material repeated")
	}
}

func TestNeverExpires(t *testing.T) {
	p := testParams(t)
	p.ExpireSeconds = -1
	tok, err := Generate(p, nil)
	if err != nil {
		t.Fatal(err)
	}
	if tok.ExpireTimestamp != ^uint64(0) || !tok.ExpiresAt().IsZero() {
		t.Fatalf("expire %x", tok.ExpireTimestamp)
	}
}

func TestOpenRejectsTampering(t *testing.T) {
	p := testParams(t)
	tok, err := Generate(p, nil)
	if err != nil {
		t.Fatal(err)
	}
	var wrong Key
	wrong[0] = 1
	if _, err := tok.Open(&wrong); !errors.Is(err, ErrDecrypt) {
		t.Fatalf("wrong key: %v", err)
	}
	cases := map[string]func(*Token){
		"private byte":  func(x *Token) { x.PrivateData[100] ^= 1 },
		"mac byte":      func(x *Token) { x.PrivateData[PrivateBytes-1] ^= 1 },
		"expire in AD":  func(x *Token) { x.ExpireTimestamp++ },
		"protocol (AD)": func(x *Token) { x.ProtocolID ^= 1 },
		"nonce":         func(x *Token) { x.Nonce[0] ^= 1 },
	}
	for name, mutate := range cases {
		c := *tok
		mutate(&c)
		if _, err := c.Open(&p.PrivateKey); !errors.Is(err, ErrDecrypt) {
			t.Errorf("%s: expected ErrDecrypt, got %v", name, err)
		}
	}
}

func TestParseRejectsMalformed(t *testing.T) {
	p := testParams(t)
	tok, err := Generate(p, nil)
	if err != nil {
		t.Fatal(err)
	}
	good, _ := tok.Marshal()
	mutate := func(f func(b []byte)) []byte {
		b := append([]byte(nil), good...)
		f(b)
		return b
	}
	const addrCountOff = VersionInfoBytes + 8 + 8 + 8 + NonceBytes + PrivateBytes + 4
	cases := map[string][]byte{
		"short":            good[:TokenBytes-1],
		"long":             append(append([]byte(nil), good...), 0),
		"version":          mutate(func(b []byte) { b[11] = '3' }),
		"version nul":      mutate(func(b []byte) { b[12] = 'x' }),
		"create > expire":  mutate(func(b []byte) { b[VersionInfoBytes+8+7] = 0xFF }),
		"zero addresses":   mutate(func(b []byte) { copy(b[addrCountOff:], []byte{0, 0, 0, 0}) }),
		"33 addresses":     mutate(func(b []byte) { copy(b[addrCountOff:], []byte{33, 0, 0, 0}) }),
		"negative count":   mutate(func(b []byte) { copy(b[addrCountOff:], []byte{0xFF, 0xFF, 0xFF, 0xFF}) }),
		"bad address type": mutate(func(b []byte) { b[addrCountOff+4] = 3 }),
	}
	for name, b := range cases {
		if _, err := Parse(b); !errors.Is(err, ErrMalformed) {
			t.Errorf("%s: expected ErrMalformed, got %v", name, err)
		}
	}
}

func TestGenerateValidation(t *testing.T) {
	p := testParams(t)
	p.PublicAddresses = nil
	if _, err := Generate(p, nil); !errors.Is(err, ErrNoServers) {
		t.Fatalf("no servers: %v", err)
	}
	p = testParams(t)
	p.PublicAddresses = make([]netip.AddrPort, MaxServers+1)
	for i := range p.PublicAddresses {
		p.PublicAddresses[i] = netip.AddrPortFrom(netip.MustParseAddr("10.0.0.1"), uint16(1000+i))
	}
	if _, err := Generate(p, nil); !errors.Is(err, ErrTooManyServer) {
		t.Fatalf("too many: %v", err)
	}
	// 32 IPv6 addresses is the worst case and must still fit both parts.
	p.PublicAddresses = p.PublicAddresses[:MaxServers]
	for i := range p.PublicAddresses {
		p.PublicAddresses[i] = netip.AddrPortFrom(netip.MustParseAddr("2001:db8::1"), uint16(1000+i))
	}
	tok, err := Generate(p, nil)
	if err != nil {
		t.Fatalf("32 IPv6: %v", err)
	}
	if b, err := tok.Marshal(); err != nil || len(b) != TokenBytes {
		t.Fatalf("marshal 32 IPv6: %v", err)
	}
	p = testParams(t)
	p.InternalAddresses = p.PublicAddresses[:1]
	if _, err := Generate(p, nil); err == nil {
		t.Fatal("mismatched internal list accepted")
	}
	p = testParams(t)
	p.PublicAddresses = []netip.AddrPort{{}}
	if _, err := Generate(p, nil); err == nil {
		t.Fatal("invalid address accepted")
	}
}

func TestUserDataLayout(t *testing.T) {
	u := UserData{
		Flags: FlagReconnect | FlagBot, AccountID: 0x0102030405060708, CharacterID: 0x1112131415161718,
		SessionEpoch: 5, ContentBuild: 0xAABB, ZoneID: 1001, PlacementTicket: 77, Entitlements: 0xF0,
	}
	for i := range u.AttestationHash {
		u.AttestationHash[i] = byte(i)
	}
	b := u.Marshal()
	// Pin the offsets that the C++ gateway reads.
	if b[0] != 1 || b[1] != 3 || b[8] != 0x08 || b[15] != 0x01 || b[16] != 0x18 || b[24] != 5 ||
		b[32] != 0xBB || b[40] != 0xE9 || b[41] != 0x03 || b[48] != 77 || b[56] != 0xF0 || b[64] != 0 || b[95] != 31 {
		t.Fatalf("layout drifted: % x", b[:96])
	}
	for _, x := range b[96:] {
		if x != 0 {
			t.Fatal("reserved tail must be zero")
		}
	}
	back, err := UnmarshalUserData(&b)
	if err != nil || back != u {
		t.Fatalf("round trip %+v %v", back, err)
	}
	b[0] = 2
	if _, err := UnmarshalUserData(&b); !errors.Is(err, ErrUserDataVersion) {
		t.Fatalf("version check: %v", err)
	}
}

func FuzzParse(f *testing.F) {
	var file struct {
		Vectors []fixedVector `json:"vectors"`
	}
	if b, err := os.ReadFile(filepath.Join(vectorsDir, "netcode_token_fixed.json")); err == nil && json.Unmarshal(b, &file) == nil {
		for _, v := range file.Vectors {
			if tb, err := hex.DecodeString(v.Token); err == nil {
				f.Add(tb)
			}
		}
	}
	f.Add(make([]byte, TokenBytes))
	f.Fuzz(func(t *testing.T, b []byte) {
		tok, err := Parse(b)
		if err != nil {
			return
		}
		out, err := tok.Marshal()
		if err != nil {
			t.Fatalf("parsed token does not re-marshal: %v", err)
		}
		again, err := Parse(out)
		if err != nil {
			t.Fatalf("re-marshaled token does not parse: %v", err)
		}
		if again.ProtocolID != tok.ProtocolID || !equalAddrs(again.ServerAddresses, tok.ServerAddresses) ||
			again.PrivateData != tok.PrivateData {
			t.Fatal("round trip changed the token")
		}
	})
}
