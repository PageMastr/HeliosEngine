//go:build linux

package orchestrator

import (
	"os"
	"syscall"
)

// platformState is empty on Linux: the parent-death signal binds children to the backend.
type platformState struct{}

func (*platformState) init() error       { return nil }
func (*platformState) adopt(*os.Process) {}
func (*platformState) close()            {}

// sysProcAttr kills the child if the backend dies (even by SIGKILL) and puts it in its own
// process group so a terminal Ctrl+C reaches only the backend, which then stops children in
// order.
func sysProcAttr() *syscall.SysProcAttr {
	return &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL, Setpgid: true}
}

// interrupt asks the child to shut down gracefully.
func interrupt(p *os.Process) error { return p.Signal(syscall.SIGTERM) }
