package orchestrator

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/testutil"
)

// TestMain doubles as the supervised child: with HELIOS_SUPERVISOR_HELPER set, the test
// binary behaves like a tiny server process instead of running tests.
func TestMain(m *testing.M) {
	switch mode := os.Getenv("HELIOS_SUPERVISOR_HELPER"); {
	case mode == "":
		os.Exit(m.Run())
	case strings.HasPrefix(mode, "exit="):
		var code int
		fmt.Sscanf(mode, "exit=%d", &code)
		os.Exit(code)
	case mode == "sleep":
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, os.Interrupt, syscall.SIGTERM)
		select {
		case <-ch:
			fmt.Println("graceful shutdown")
			os.Exit(0)
		case <-time.After(60 * time.Second):
			os.Exit(3)
		}
	case mode == "ignore":
		signal.Ignore(os.Interrupt, syscall.SIGTERM)
		time.Sleep(60 * time.Second)
		os.Exit(4)
	case mode == "env":
		fmt.Printf("name=%s shard=%s extra=%s\n", os.Getenv("HELIOS_PROCESS_NAME"), os.Getenv("HELIOS_SHARD"), os.Getenv("EXTRA"))
		os.Exit(0)
	}
	os.Exit(99)
}

func helperSpec(t *testing.T, name, mode string) ProcessSpec {
	t.Helper()
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	return ProcessSpec{Name: name, Exe: exe, Env: []string{"HELIOS_SUPERVISOR_HELPER=" + mode},
		BackoffMin: 10 * time.Millisecond, BackoffMax: 40 * time.Millisecond, StopTimeout: 2 * time.Second}
}

// eventLog records every event; next consumes the first unconsumed match, so events for
// different children can arrive in any order without being lost.
type eventLog struct {
	mu     sync.Mutex
	events []SupervisorEvent
	used   []bool
	notify chan struct{}
}

func newEventLog() *eventLog { return &eventLog{notify: make(chan struct{}, 1)} }

func (l *eventLog) hook(e SupervisorEvent) {
	l.mu.Lock()
	l.events = append(l.events, e)
	l.used = append(l.used, false)
	l.mu.Unlock()
	select {
	case l.notify <- struct{}{}:
	default:
	}
}

// next waits for the next event of kind for child name.
func (l *eventLog) next(t *testing.T, name, kind string) SupervisorEvent {
	t.Helper()
	deadline := time.After(10 * time.Second)
	for {
		l.mu.Lock()
		for i, e := range l.events {
			if !l.used[i] && e.Name == name && e.Kind == kind {
				l.used[i] = true
				l.mu.Unlock()
				return e
			}
		}
		l.mu.Unlock()
		select {
		case <-l.notify:
		case <-time.After(20 * time.Millisecond):
		case <-deadline:
			t.Fatalf("timed out waiting for %s/%s", name, kind)
		}
	}
}

func startSupervisor(t *testing.T, specs ...ProcessSpec) (*Supervisor, *eventLog, *prometheus.Registry) {
	t.Helper()
	reg := prometheus.NewRegistry()
	s := NewSupervisor(specs, []string{"HELIOS_SHARD=test"}, t.TempDir(), quiet, reg)
	log := newEventLog()
	s.OnEvent = log.hook
	if err := s.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = s.Stop(ctx)
	})
	return s, log, reg
}

func TestRestartAlwaysWithBackoff(t *testing.T) {
	s, log, _ := startSupervisor(t, helperSpec(t, "crashy", "exit=1"))
	var delays []time.Duration
	for i := 0; i < 4; i++ {
		ex := log.next(t, "crashy", "exited")
		if ex.ExitCode != 1 {
			t.Fatalf("exit code %d", ex.ExitCode)
		}
		delays = append(delays, log.next(t, "crashy", "restarting").Delay)
	}
	want := []time.Duration{10 * time.Millisecond, 20 * time.Millisecond, 40 * time.Millisecond, 40 * time.Millisecond}
	for i := range want {
		if delays[i] != want[i] {
			t.Fatalf("backoff %v, want %v", delays, want)
		}
	}
	if got := testutil.ToFloat64(s.restarts.WithLabelValues("crashy")); got < 4 {
		t.Fatalf("restart metric %v", got)
	}
	if st := s.Status(); len(st) != 1 || st[0].Restarts < 4 || st[0].LastExit != 1 {
		t.Fatalf("status %+v", st)
	}
}

func TestRestartPolicies(t *testing.T) {
	onFailClean := helperSpec(t, "clean", "exit=0")
	onFailClean.Restart = RestartOnFailure
	never := helperSpec(t, "never", "exit=5")
	never.Restart = RestartNever
	onFailDirty := helperSpec(t, "dirty", "exit=2")
	onFailDirty.Restart = RestartOnFailure
	s, log, _ := startSupervisor(t, onFailClean, never, onFailDirty)

	log.next(t, "dirty", "restarting") // on-failure restarts a non-zero exit
	log.next(t, "clean", "exited")
	log.next(t, "never", "exited")
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := s.Stop(ctx); err != nil {
		t.Fatal(err)
	}
	for _, st := range s.Status() {
		if (st.Name == "clean" || st.Name == "never") && st.Restarts != 0 {
			t.Fatalf("%s restarted: %+v", st.Name, st)
		}
	}
}

func TestStopIsGracefulThenForceful(t *testing.T) {
	graceful := helperSpec(t, "graceful", "sleep")
	stubborn := helperSpec(t, "stubborn", "ignore")
	stubborn.StopTimeout = 200 * time.Millisecond
	s, log, _ := startSupervisor(t, graceful, stubborn)
	log.next(t, "graceful", "started")
	log.next(t, "stubborn", "started")
	time.Sleep(200 * time.Millisecond) // let the helpers install their signal handlers
	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := s.Stop(ctx); err != nil {
		t.Fatal(err)
	}
	if took := time.Since(start); took > 5*time.Second {
		t.Fatalf("stop took %v", took)
	}
	for _, st := range s.Status() {
		if st.Running {
			t.Fatalf("%s still running", st.Name)
		}
	}
	if err := s.Stop(ctx); err != nil {
		t.Fatal("second Stop must be a no-op")
	}
}

func TestChildEnvironmentAndLog(t *testing.T) {
	spec := helperSpec(t, "envy", "env")
	spec.Env = append(spec.Env, "EXTRA=42")
	spec.Restart = RestartNever
	reg := prometheus.NewRegistry()
	dir := t.TempDir()
	s := NewSupervisor([]ProcessSpec{spec}, []string{"HELIOS_SHARD=eu9"}, dir, quiet, reg)
	log := newEventLog()
	s.OnEvent = log.hook
	if err := s.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	log.next(t, "envy", "exited")
	_ = s.Stop(context.Background())
	b, err := os.ReadFile(filepath.Join(dir, "envy.log"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(b), "name=envy shard=eu9 extra=42") {
		t.Fatalf("child log: %q", b)
	}
}

func TestMissingExecutableRetries(t *testing.T) {
	spec := ProcessSpec{Name: "ghost", Exe: filepath.Join(t.TempDir(), "does-not-exist"),
		BackoffMin: 5 * time.Millisecond, BackoffMax: 10 * time.Millisecond}
	_, log, _ := startSupervisor(t, spec)
	e := log.next(t, "ghost", "failed")
	if e.Err == nil {
		t.Fatal("expected a start error")
	}
	log.next(t, "ghost", "restarting")
	log.next(t, "ghost", "failed")
}

func TestSpecDefaults(t *testing.T) {
	d := (&ProcessSpec{}).withDefaults()
	if d.Restart != RestartAlways || d.BackoffMin != 250*time.Millisecond || d.BackoffMax != 30*time.Second ||
		d.StableAfter != 10*time.Second || d.StopTimeout != 5*time.Second {
		t.Fatalf("defaults %+v", d)
	}
	big := (&ProcessSpec{BackoffMin: time.Minute}).withDefaults()
	if big.BackoffMax != time.Minute {
		t.Fatalf("max below min: %+v", big)
	}
	s := NewSupervisor(nil, nil, t.TempDir(), nil, nil)
	if err := s.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := s.Start(context.Background()); err == nil {
		t.Fatal("double start")
	}
	_ = s.Stop(context.Background())
}
