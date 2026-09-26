package orchestrator_test

import (
	"bufio"
	"encoding/json"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
)

// cppVectors is the file engine/server's codec tests compare the C++ encoder against
// (test_contracts.cpp); gen_go_vectors.go.txt next to it wrote it from these Go types.
var cppVectors = filepath.Join("..", "..", "..", "engine", "server", "tests", "data", "go_contract_vectors.tsv")

// TestEncodingMatchesTheCppContractVectors keeps the fields WP-0.15r added (fd, serverBuild,
// held) optional on the wire: each orchestrator vector decodes into today's Go type and encodes
// back byte for byte, so messages that do not set the new fields are exactly what the C++ cell
// and gateway are tested against, and the C++ side keeps working until it sends them.
func TestEncodingMatchesTheCppContractVectors(t *testing.T) {
	f, err := os.Open(cppVectors)
	if errors.Is(err, fs.ErrNotExist) {
		t.Skipf("%s not found: services/ is not inside a Helios checkout", cppVectors)
	}
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	vectors := map[string]string{}
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64<<10), 1<<20)
	for sc.Scan() {
		if name, value, ok := strings.Cut(sc.Text(), "\t"); ok && !strings.HasPrefix(name, "#") {
			vectors[name] = value
		}
	}
	if err := sc.Err(); err != nil {
		t.Fatal(err)
	}
	for name, v := range map[string]any{
		"ProcessInfoCell":          &orchestrator.ProcessInfo{},
		"ProcessInfoGateway":       &orchestrator.ProcessInfo{},
		"RegisterResult":           &orchestrator.RegisterResult{},
		"RegisterResultGateway":    &orchestrator.RegisterResult{},
		"HeartbeatRequest":         &orchestrator.HeartbeatRequest{},
		"HeartbeatResult":          &orchestrator.HeartbeatResult{},
		"AllocateIdBlocksRequest":  &orchestrator.AllocateIdBlocksRequest{},
		"AllocateIdBlocksResponse": &orchestrator.AllocateIdBlocksResponse{},
		"DeregisterRequest":        &orchestrator.DeregisterRequest{},
		"ResolveZoneRequestId":     &orchestrator.ResolveZoneRequest{},
		"ResolveZoneRequestName":   &orchestrator.ResolveZoneRequest{},
		"Route":                    &orchestrator.Route{},
	} {
		want, ok := vectors[name]
		if !ok {
			t.Errorf("%s: missing from %s", name, cppVectors)
			continue
		}
		if err := json.Unmarshal([]byte(want), v); err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		if got, _ := json.Marshal(v); string(got) != want {
			t.Errorf("%s:\n got %s\nwant %s", name, got, want)
		}
	}
	// And the new fields, when set, use the names 05 §1.4 gives them.
	b, _ := json.Marshal(orchestrator.HeartbeatRequest{ProcessID: 1, Epoch: 1,
		Held: []orchestrator.HeldRegion{{Region: 1002, LeaseGen: 7}}})
	if !strings.Contains(string(b), `"held":[{"region":"1002","leaseGen":"7"}]`) {
		t.Fatalf("held: %s", b)
	}
	b, _ = json.Marshal(orchestrator.ProcessInfo{Name: "c", Kind: "cell", FD: orchestrator.FailureDomain{AZ: "a", Rack: "r", Host: "h"},
		ServerBuild: 9})
	if !strings.Contains(string(b), `"fd":{"az":"a","rack":"r","host":"h"},"serverBuild":"9"`) {
		t.Fatalf("fd and serverBuild: %s", b)
	}
}
