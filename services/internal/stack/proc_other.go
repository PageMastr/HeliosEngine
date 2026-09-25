//go:build !windows

package stack

import (
	"errors"
	"os"
	"syscall"
)

// isRoot reports whether the process runs as root; initdb and postgres refuse to.
func isRoot() bool { return os.Geteuid() == 0 }

// processAlive reports whether pid names a running process.
func processAlive(pid int) bool {
	err := syscall.Kill(pid, 0)
	return err == nil || errors.Is(err, syscall.EPERM)
}
