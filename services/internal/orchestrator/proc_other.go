//go:build !linux && !windows

package orchestrator

import (
	"os"
	"syscall"
)

// platformState is empty here; there is no portable parent-death binding on BSD/macOS, so a
// crashed backend can leave children running (they self-fence once their lease lapses).
type platformState struct{}

func (*platformState) init() error       { return nil }
func (*platformState) adopt(*os.Process) {}
func (*platformState) close()            {}

func sysProcAttr() *syscall.SysProcAttr { return &syscall.SysProcAttr{Setpgid: true} }

func interrupt(p *os.Process) error { return p.Signal(syscall.SIGTERM) }
