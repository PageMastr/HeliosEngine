//go:build integration

package integration

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"slices"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/PageMastr/scifi-test/services/internal/session"
)

// perf: BE-A2 (05 §10) asks for connect-token issue p99 < 100 ms at 100/s. This drives
// CreateSession over HTTP against the whole stack (JWT check, the ban check in PostgreSQL, the
// session record in Valkey, the token mint and the reconnect ticket) at a fixed 100 requests/s
// for 10 s, open-loop, and fails at p99 ≥ 100 ms. It measures this host, not the Windows 11
// reference machine BE-A1 names.
func TestPerfTokenIssueAt100PerSecond(t *testing.T) {
	need(t)
	const rate, total = 100, 1000
	var bearers []string
	for _, n := range []int{7, 8, 9, 10} {
		bearers = append(bearers, login(t, fmt.Sprintf("dev%d@helios.test", n), "dev").AccessToken)
	}
	client := &http.Client{Timeout: 5 * time.Second, Transport: &http.Transport{MaxIdleConnsPerHost: 128}}
	body, _ := json.Marshal(session.CreateSessionRequest{ZoneID: 1001})
	issue := func(bearer string) (time.Duration, error) {
		req, _ := http.NewRequest(http.MethodPost, apiURL+session.ServicePath+"CreateSession", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Authorization", "Bearer "+bearer)
		start := time.Now()
		res, err := client.Do(req)
		if err != nil {
			return 0, err
		}
		_, _ = io.Copy(io.Discard, res.Body)
		res.Body.Close()
		if res.StatusCode != http.StatusOK {
			return 0, fmt.Errorf("status %d", res.StatusCode)
		}
		return time.Since(start), nil
	}
	for _, b := range bearers { // warm the connections and caches
		if _, err := issue(b); err != nil {
			t.Fatal(err)
		}
	}
	lat := make([]time.Duration, total)
	var failed atomic.Int32
	var wg sync.WaitGroup
	start := time.Now()
	for i := 0; i < total; i++ {
		time.Sleep(time.Until(start.Add(time.Duration(i) * time.Second / rate)))
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			d, err := issue(bearers[i%len(bearers)])
			if err != nil {
				failed.Add(1)
				d = time.Hour
			}
			lat[i] = d
		}(i)
	}
	wg.Wait()
	elapsed := time.Since(start)
	slices.Sort(lat)
	p50, p99, worst := lat[total/2], lat[total*99/100], lat[total-1]
	t.Logf("perf: token issue at %d/s over %s: p50 %s, p99 %s, max %s, %d failed (budget p99 < 100ms, 05 BE-A2)",
		rate, elapsed.Round(time.Millisecond), p50.Round(10*time.Microsecond), p99.Round(10*time.Microsecond),
		worst.Round(10*time.Microsecond), failed.Load())
	if failed.Load() > 0 || p99 >= 100*time.Millisecond {
		t.Fatalf("BE-A2: p99 %s with %d failures", p99, failed.Load())
	}
}
