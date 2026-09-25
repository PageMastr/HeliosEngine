package keyring

import (
	"bytes"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

func TestLoadOrCreateRotatePrune(t *testing.T) {
	path := filepath.Join(t.TempDir(), "keys", "shard.json")
	now := time.Date(2026, 9, 1, 10, 0, 0, 0, time.UTC)
	r, created, err := LoadOrCreate(path, "netcode-shard", bytes.NewReader(bytes.Repeat([]byte{7}, 64)), now)
	if err != nil || !created {
		t.Fatalf("create: %v %v", created, err)
	}
	if r.Current().ID != 1 || !bytes.Equal(r.Current().Secret, bytes.Repeat([]byte{7}, 32)) {
		t.Fatalf("first key %+v", r.Current())
	}
	if runtime.GOOS != "windows" {
		st, _ := os.Stat(path)
		if st.Mode().Perm() != 0o600 {
			t.Fatalf("key file mode %v", st.Mode().Perm())
		}
	}
	again, created, err := LoadOrCreate(path, "netcode-shard", nil, now)
	if err != nil || created || again.Current().ID != 1 || !bytes.Equal(again.Current().Secret, r.Current().Secret) {
		t.Fatalf("reload: %v %v", created, err)
	}
	e2, err := again.Add(nil, now.Add(24*time.Hour))
	if err != nil || e2.ID != 2 {
		t.Fatalf("add: %+v %v", e2, err)
	}
	if err := again.Save(path); err != nil {
		t.Fatal(err)
	}
	loaded, err := Load(path)
	if err != nil || loaded.Current().ID != 2 || len(loaded.Keys) != 2 {
		t.Fatalf("load after rotate: %v", err)
	}
	if k, ok := loaded.Get(1); !ok || !bytes.Equal(k.Secret, r.Current().Secret) {
		t.Fatal("old generation lost")
	}
	if _, ok := loaded.Get(9); ok {
		t.Fatal("unknown id found")
	}
	loaded.Prune(1)
	if len(loaded.Keys) != 1 || loaded.Current().ID != 2 {
		t.Fatalf("prune: %+v", loaded.Keys)
	}
}

func TestLoadRejectsBadFiles(t *testing.T) {
	dir := t.TempDir()
	cases := map[string]string{
		"garbage":   `{`,
		"version":   `{"version":2,"keys":[]}`,
		"empty":     `{"version":1,"keys":[]}`,
		"short":     `{"version":1,"keys":[{"id":1,"secret":"AAAA"}]}`,
		"zero id":   `{"version":1,"keys":[{"id":0,"secret":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}]}`,
		"duplicate": `{"version":1,"keys":[{"id":1,"secret":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="},{"id":1,"secret":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}]}`,
	}
	for name, body := range cases {
		p := filepath.Join(dir, name+".json")
		if err := os.WriteFile(p, []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
		if _, err := Load(p); err == nil {
			t.Errorf("%s: accepted", name)
		}
		if _, _, err := LoadOrCreate(p, "x", nil, time.Now()); err == nil {
			t.Errorf("%s: LoadOrCreate must not overwrite a corrupt file", name)
		}
	}
	// Out-of-order IDs are sorted so Current is the newest.
	p := filepath.Join(dir, "order.json")
	body := `{"version":1,"keys":[{"id":3,"secret":"AwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwM="},{"id":1,"secret":"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="}]}`
	if err := os.WriteFile(p, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	r, err := Load(p)
	if err != nil || r.Current().ID != 3 {
		t.Fatalf("order: %v", err)
	}
}
