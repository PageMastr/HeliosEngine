package stack

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// ErrDataDirLocked is returned when another helios-backend owns the data directory.
var ErrDataDirLocked = errors.New("data directory is in use by another helios-backend")

// DataDirLock is an exclusive lock on a data directory, held for the life of the process. The
// OS drops it when the process dies, so a crash never leaves it stuck; while it is held no second
// backend can start on the same embedded PostgreSQL, NATS store or keys.
type DataDirLock struct {
	f   *os.File
	pid string
}

// LockDataDir creates dir if needed and takes its lock.
func LockDataDir(dir string) (*DataDirLock, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(filepath.Join(dir, "backend.lock"), os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, err
	}
	pidFile := filepath.Join(dir, "backend.pid")
	if err := lockFile(f); err != nil {
		f.Close()
		holder, _ := os.ReadFile(pidFile)
		if h := strings.TrimSpace(string(holder)); h != "" {
			return nil, fmt.Errorf("%w (pid %s): %s", ErrDataDirLocked, h, dir)
		}
		return nil, fmt.Errorf("%w: %s", ErrDataDirLocked, dir)
	}
	_ = os.WriteFile(pidFile, []byte(strconv.Itoa(os.Getpid())+"\n"), 0o600)
	return &DataDirLock{f: f, pid: pidFile}, nil
}

// Unlock releases the lock.
func (l *DataDirLock) Unlock() error {
	if l == nil || l.f == nil {
		return nil
	}
	_ = os.Remove(l.pid)
	err := unlockFile(l.f)
	if cerr := l.f.Close(); err == nil {
		err = cerr
	}
	l.f = nil
	return err
}
