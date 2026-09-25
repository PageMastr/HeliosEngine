//go:build windows

package orchestrator

import (
	"os"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

// platformState holds a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: every child is
// assigned to it, and when the backend exits for any reason Windows closes the handle and
// terminates the children, so no orphaned cell keeps UDP 7777 or a zone.
type platformState struct {
	job windows.Handle
}

func (s *platformState) init() error {
	job, err := windows.CreateJobObject(nil, nil)
	if err != nil {
		return err
	}
	info := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{
		BasicLimitInformation: windows.JOBOBJECT_BASIC_LIMIT_INFORMATION{LimitFlags: windows.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE},
	}
	if _, err := windows.SetInformationJobObject(job, windows.JobObjectExtendedLimitInformation,
		uintptr(unsafe.Pointer(&info)), uint32(unsafe.Sizeof(info))); err != nil {
		_ = windows.CloseHandle(job)
		return err
	}
	s.job = job
	return nil
}

func (s *platformState) adopt(p *os.Process) {
	if s.job == 0 {
		return
	}
	h, err := windows.OpenProcess(windows.PROCESS_SET_QUOTA|windows.PROCESS_TERMINATE, false, uint32(p.Pid))
	if err != nil {
		return
	}
	defer windows.CloseHandle(h)
	_ = windows.AssignProcessToJobObject(s.job, h)
}

func (s *platformState) close() {
	if s.job != 0 {
		_ = windows.CloseHandle(s.job)
		s.job = 0
	}
}

// sysProcAttr starts each child in its own process group so it can receive CTRL_BREAK.
func sysProcAttr() *syscall.SysProcAttr {
	return &syscall.SysProcAttr{CreationFlags: windows.CREATE_NEW_PROCESS_GROUP}
}

// interrupt sends CTRL_BREAK_EVENT (the Windows analogue of SIGTERM for console programs);
// helios C++ servers treat it as a graceful shutdown request. It fails for children without a
// console, and the caller then kills the process.
func interrupt(p *os.Process) error {
	return windows.GenerateConsoleCtrlEvent(windows.CTRL_BREAK_EVENT, uint32(p.Pid))
}
