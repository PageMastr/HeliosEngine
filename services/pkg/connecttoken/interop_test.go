package connecttoken

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/connecttoken/cinterop"
)

// TestNetcodeCInterop compiles testdata/interop/netcode_interop.c against the vendored netcode
// and checks both directions live: tokens minted here are parsed, decrypted and used for a full
// netcode client/server handshake by the C library, and the C generators still reproduce the
// committed golden vectors. It needs a C compiler, so it only runs with HELIOS_NETCODE_INTEROP=1
// (CI keeps the golden-vector tests above, which need no compiler).
func TestNetcodeCInterop(t *testing.T) {
	if !cinterop.Enabled() {
		t.Skip("set HELIOS_NETCODE_INTEROP=1 to build and run the C interop harness")
	}
	harness := cinterop.Build(t)
	dir := t.TempDir()

	t.Run("golden vectors reproduce", func(t *testing.T) {
		out := filepath.Join(dir, "fixed.json")
		cinterop.Run(t, harness, "gen-fixed", out)
		got, _ := os.ReadFile(out)
		want, err := os.ReadFile(filepath.Join(vectorsDir, "netcode_token_fixed.json"))
		if err != nil || !bytes.Equal(got, want) {
			t.Fatalf("netcode C no longer reproduces netcode_token_fixed.json (err=%v)", err)
		}
		// gen-api draws a fresh nonce, so compare only its decryptable content.
		apiOut := filepath.Join(dir, "api.json")
		cinterop.Run(t, harness, "gen-api", apiOut)
		var v apiVector
		b, _ := os.ReadFile(apiOut)
		if err := json.Unmarshal(b, &v); err != nil {
			t.Fatal(err)
		}
		tok, err := Parse(mustHex(t, v.Token, TokenBytes))
		if err != nil {
			t.Fatal(err)
		}
		var key Key
		copy(key[:], mustHex(t, v.PrivateKey, KeyBytes))
		if _, err := tok.Open(&key); err != nil {
			t.Fatalf("fresh C token does not open in Go: %v", err)
		}
	})

	// A fresh Go token: C must parse and decrypt it and see exactly our fields.
	p := testParams(t)
	p.CreateTime = time.Now()
	p.ExpireSeconds = 45
	p.PublicAddresses = mustAddrs(t, []string{"[::1]:40000", "127.0.0.1:40001"})
	p.InternalAddresses = mustAddrs(t, []string{"[::1]:40000", "10.9.8.7:40001"})
	tok, err := Generate(p, nil)
	if err != nil {
		t.Fatal(err)
	}
	raw, _ := tok.Marshal()
	tokenPath := filepath.Join(dir, "go-token.bin")
	keyPath := filepath.Join(dir, "key.hex")
	if err := os.WriteFile(tokenPath, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyPath, []byte(hex.EncodeToString(p.PrivateKey[:])), 0o600); err != nil {
		t.Fatal(err)
	}

	t.Run("C decrypts Go token", func(t *testing.T) {
		out := cinterop.Run(t, harness, "verify", tokenPath, keyPath)
		var got struct {
			ProtocolID        string   `json:"protocol_id"`
			CreateTimestamp   string   `json:"create_timestamp"`
			ExpireTimestamp   string   `json:"expire_timestamp"`
			TimeoutSeconds    int32    `json:"timeout_seconds"`
			ClientID          string   `json:"client_id"`
			PublicAddresses   []string `json:"public_addresses"`
			InternalAddresses []string `json:"internal_addresses"`
			ClientToServerKey string   `json:"client_to_server_key"`
			UserData          string   `json:"user_data"`
		}
		if err := json.Unmarshal(out, &got); err != nil {
			t.Fatalf("harness output %q: %v", out, err)
		}
		if mustU64(t, got.ProtocolID, 16) != p.ProtocolID || mustU64(t, got.ClientID, 16) != p.ClientID ||
			got.TimeoutSeconds != p.TimeoutSeconds ||
			mustU64(t, got.ExpireTimestamp, 10)-mustU64(t, got.CreateTimestamp, 10) != 45 {
			t.Fatalf("header mismatch: %+v", got)
		}
		if !equalAddrs(mustAddrs(t, got.PublicAddresses), p.PublicAddresses) ||
			!equalAddrs(mustAddrs(t, got.InternalAddresses), p.InternalAddresses) {
			t.Fatalf("addresses mismatch: %v / %v", got.PublicAddresses, got.InternalAddresses)
		}
		if got.ClientToServerKey != hex.EncodeToString(tok.ClientToServerKey[:]) ||
			got.UserData != hex.EncodeToString(p.UserData[:]) {
			t.Fatal("key or user data mismatch")
		}
	})

	t.Run("netcode handshake with Go token", func(t *testing.T) {
		out := cinterop.Run(t, harness, "connect", tokenPath, keyPath, "[::1]:40000")
		var got struct {
			Connected bool   `json:"connected"`
			ClientID  string `json:"client_id"`
			UserData  string `json:"user_data"`
		}
		if err := json.Unmarshal(out, &got); err != nil || !got.Connected {
			t.Fatalf("handshake failed: %s (%v)", out, err)
		}
		if mustU64(t, got.ClientID, 16) != p.ClientID || got.UserData != hex.EncodeToString(p.UserData[:]) {
			t.Fatalf("server saw wrong client: %s", out)
		}
	})

	t.Run("gateway ignores token for another address", func(t *testing.T) {
		// 10.9.8.7:40001 is only the *internal* address of the second entry; a server bound to a
		// public-only address must not accept it.
		if out, ok := cinterop.Try(harness, "connect", tokenPath, keyPath, "127.0.0.1:40001"); ok {
			t.Fatalf("handshake unexpectedly succeeded: %s", out)
		}
	})
}
