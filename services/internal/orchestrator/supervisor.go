package orchestrator

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"time"

	"github.com/prometheus/client_golang/prometheus"
)

// Restart policies.
const (
	RestartAlways    = "always"
	RestartOnFailure = "on-failure"
	RestartNever     = "never"
)

// ProcessSpec describes one supervised child (the LocalProcessPlacer of 05 §1.4 in its
// Phase 0 form: a fixed list from config).
type ProcessSpec struct {
	Name        string
	Exe         string
	Args        []string
	Env         []string // extra KEY=VALUE entries
	Dir         string
	Restart     string        // always (default) | on-failure | never
	BackoffMin  time.Duration // first restart delay (default 250 ms)
	BackoffMax  time.Duration // cap for the doubling delay (default 30 s)
	StableAfter time.Duration // a run this long resets the backoff (default 10 s)
	StopTimeout time.Duration // grace period between interrupt and kill (default 5 s)
}

// SupervisorEvent reports a child state change (logging, metrics, tests).
type SupervisorEvent struct {
	Name     string
	Kind     string // started | exited | restarting | failed | stopped
	PID      int
	ExitCode int
	Restarts int
	Delay    time.Duration
	Err      error
}

// ChildStatus is a snapshot for the admin API.
type ChildStatus struct {
	Name     string    `json:"name"`
	Running  bool      `json:"running"`
	PID      int       `json:"pid"`
	Restarts int       `json:"restarts"`
	Since    time.Time `json:"since"`
	LastExit int       `json:"lastExit"`
}

// Supervisor starts, watches and restarts child processes with exponential backoff. Children
// are tied to the supervisor's lifetime by the OS (Linux: parent-death signal; Windows: a
// kill-on-close job object), so a crashed backend never leaves orphaned cells holding ports.
type Supervisor struct {
	specs  []ProcessSpec
	env    []string
	logDir string
	log    *slog.Logger
	// OnEvent, if set before Start, observes every event (called from child goroutines).
	OnEvent func(SupervisorEvent)

	restarts *prometheus.CounterVec
	running  *prometheus.GaugeVec

	mu       sync.Mutex
	status   map[string]*ChildStatus
	stop     chan struct{}
	wg       sync.WaitGroup
	started  bool
	platform platformState
}

// NewSupervisor prepares a supervisor. env is appended to every child's environment (NATS
// URL, shard, ...); child output goes to <logDir>/<name>.log.
func NewSupervisor(specs []ProcessSpec, env []string, logDir string, log *slog.Logger, reg prometheus.Registerer) *Supervisor {
	if log == nil {
		log = slog.Default()
	}
	s := &Supervisor{
		specs: specs, env: env, logDir: logDir, log: log, status: map[string]*ChildStatus{},
		restarts: prometheus.NewCounterVec(prometheus.CounterOpts{Name: "helios_supervisor_restarts_total",
			Help: "Restarts of supervised processes."}, []string{"name"}),
		running: prometheus.NewGaugeVec(prometheus.GaugeOpts{Name: "helios_supervisor_running",
			Help: "1 while the supervised process runs."}, []string{"name"}),
	}
	if reg != nil {
		reg.MustRegister(s.restarts, s.running)
	}
	return s
}

func (spec *ProcessSpec) withDefaults() ProcessSpec {
	c := *spec
	if c.Restart == "" {
		c.Restart = RestartAlways
	}
	if c.BackoffMin <= 0 {
		c.BackoffMin = 250 * time.Millisecond
	}
	if c.BackoffMax < c.BackoffMin {
		c.BackoffMax = 30 * time.Second
		if c.BackoffMax < c.BackoffMin {
			c.BackoffMax = c.BackoffMin
		}
	}
	if c.StableAfter <= 0 {
		c.StableAfter = 10 * time.Second
	}
	if c.StopTimeout <= 0 {
		c.StopTimeout = 5 * time.Second
	}
	return c
}

// Start launches every child. It returns immediately; failures to start are retried with
// backoff like crashes.
func (s *Supervisor) Start(context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.started {
		return errors.New("supervisor: already started")
	}
	if len(s.specs) > 0 {
		if err := os.MkdirAll(s.logDir, 0o755); err != nil {
			return err
		}
		if err := s.platform.init(); err != nil {
			s.log.Warn("supervisor: children will not be bound to the backend lifetime", "err", err)
		}
	}
	s.stop = make(chan struct{})
	s.started = true
	for _, spec := range s.specs {
		spec := spec.withDefaults()
		s.status[spec.Name] = &ChildStatus{Name: spec.Name}
		s.wg.Add(1)
		go s.supervise(spec)
	}
	return nil
}

// Stop interrupts every child, waits for them (killing any that exceed their stop timeout)
// and returns when all supervision goroutines have finished or ctx expires.
func (s *Supervisor) Stop(ctx context.Context) error {
	s.mu.Lock()
	if !s.started {
		s.mu.Unlock()
		return nil
	}
	s.started = false
	close(s.stop)
	s.mu.Unlock()
	done := make(chan struct{})
	go func() { s.wg.Wait(); close(done) }()
	select {
	case <-done:
		s.platform.close()
		return nil
	case <-ctx.Done():
		return fmt.Errorf("supervisor: children still stopping: %w", ctx.Err())
	}
}

// Status returns a snapshot of every child.
func (s *Supervisor) Status() []ChildStatus {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]ChildStatus, 0, len(s.specs))
	for _, spec := range s.specs {
		if st, ok := s.status[spec.Name]; ok {
			out = append(out, *st)
		}
	}
	return out
}

func (s *Supervisor) emit(e SupervisorEvent) {
	switch e.Kind {
	case "started":
		s.log.Info("child started", "name", e.Name, "pid", e.PID)
	case "exited":
		s.log.Warn("child exited", "name", e.Name, "pid", e.PID, "code", e.ExitCode)
	case "restarting":
		s.log.Info("child restarting", "name", e.Name, "in", e.Delay, "restarts", e.Restarts)
	case "failed":
		s.log.Error("child failed to start", "name", e.Name, "err", e.Err)
	case "stopped":
		s.log.Info("child stopped", "name", e.Name)
	}
	if s.OnEvent != nil {
		s.OnEvent(e)
	}
}

func (s *Supervisor) setStatus(name string, f func(*ChildStatus)) {
	s.mu.Lock()
	f(s.status[name])
	s.mu.Unlock()
}

func (s *Supervisor) supervise(spec ProcessSpec) {
	defer s.wg.Done()
	backoff := spec.BackoffMin
	restarts := 0
	for {
		code, ran, err := s.runOnce(spec)
		if errors.Is(err, errStopped) {
			s.emit(SupervisorEvent{Name: spec.Name, Kind: "stopped", ExitCode: code, Restarts: restarts})
			return
		}
		if err != nil {
			s.emit(SupervisorEvent{Name: spec.Name, Kind: "failed", Err: err, Restarts: restarts})
		}
		if spec.Restart == RestartNever || (spec.Restart == RestartOnFailure && err == nil && code == 0) {
			return
		}
		if ran >= spec.StableAfter {
			backoff = spec.BackoffMin
		}
		restarts++
		s.restarts.WithLabelValues(spec.Name).Inc()
		s.setStatus(spec.Name, func(st *ChildStatus) { st.Restarts = restarts })
		s.emit(SupervisorEvent{Name: spec.Name, Kind: "restarting", Restarts: restarts, Delay: backoff})
		select {
		case <-s.stop:
			s.emit(SupervisorEvent{Name: spec.Name, Kind: "stopped", Restarts: restarts})
			return
		case <-time.After(backoff):
		}
		backoff *= 2
		if backoff > spec.BackoffMax {
			backoff = spec.BackoffMax
		}
	}
}

var errStopped = errors.New("stopped")

// runOnce starts the child and waits for it to exit or for Stop. The goroutine stays locked
// to its OS thread for the child's lifetime because Linux delivers the parent-death signal
// when the *thread* that forked the child exits.
func (s *Supervisor) runOnce(spec ProcessSpec) (exitCode int, ran time.Duration, err error) {
	select {
	case <-s.stop:
		return 0, 0, errStopped
	default:
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	logFile, err := os.OpenFile(filepath.Join(s.logDir, spec.Name+".log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return -1, 0, err
	}
	defer logFile.Close()
	fmt.Fprintf(logFile, "--- %s starting %s %v\n", time.Now().UTC().Format(time.RFC3339), spec.Exe, spec.Args)

	cmd := exec.Command(spec.Exe, spec.Args...)
	cmd.Dir = spec.Dir
	cmd.Env = append(append(os.Environ(), s.env...), spec.Env...)
	cmd.Env = append(cmd.Env, "HELIOS_PROCESS_NAME="+spec.Name)
	cmd.Stdout = io.Writer(logFile)
	cmd.Stderr = io.Writer(logFile)
	cmd.SysProcAttr = sysProcAttr()
	start := time.Now()
	if err := cmd.Start(); err != nil {
		return -1, 0, err
	}
	s.platform.adopt(cmd.Process)
	pid := cmd.Process.Pid
	s.running.WithLabelValues(spec.Name).Set(1)
	s.setStatus(spec.Name, func(st *ChildStatus) { st.Running, st.PID, st.Since = true, pid, start })
	s.emit(SupervisorEvent{Name: spec.Name, Kind: "started", PID: pid})

	exited := make(chan error, 1)
	go func() { exited <- cmd.Wait() }()
	stopped := false
	select {
	case <-exited: // the exit status is read from ProcessState below
	case <-s.stop:
		stopped = true
		if err := interrupt(cmd.Process); err != nil {
			_ = cmd.Process.Kill()
		}
		select {
		case <-exited:
		case <-time.After(spec.StopTimeout):
			s.log.Warn("child ignored interrupt; killing", "name", spec.Name, "pid", pid)
			_ = cmd.Process.Kill()
			<-exited
		}
	}
	code := -1
	if cmd.ProcessState != nil {
		code = cmd.ProcessState.ExitCode()
	}
	s.running.WithLabelValues(spec.Name).Set(0)
	s.setStatus(spec.Name, func(st *ChildStatus) { st.Running, st.LastExit = false, code })
	if stopped {
		return code, time.Since(start), errStopped
	}
	s.emit(SupervisorEvent{Name: spec.Name, Kind: "exited", PID: pid, ExitCode: code})
	return code, time.Since(start), nil
}
