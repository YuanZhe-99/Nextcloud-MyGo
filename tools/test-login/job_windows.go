package main

import (
	"unsafe"

	"golang.org/x/sys/windows"
)

// ownChildren puts this probe in a kill-on-close Job Object so every daemon it starts dies with it.
// Side effects: the job handle is never closed explicitly; process exit, a panic or an external kill closes it.
// Notes: Job membership is inherited, so daemons started by exec or by syncd.NewRemote cannot leak.
func ownChildren() {
	job, err := windows.CreateJobObject(nil, nil)
	if err != nil {
		panic("fixture job object unavailable")
	}
	var limits windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION
	limits.BasicLimitInformation.LimitFlags = windows.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
	if _, err := windows.SetInformationJobObject(job, windows.JobObjectExtendedLimitInformation, uintptr(unsafe.Pointer(&limits)), uint32(unsafe.Sizeof(limits))); err != nil {
		panic("fixture job limit unavailable")
	}
	if err := windows.AssignProcessToJobObject(job, windows.CurrentProcess()); err != nil {
		panic("fixture job assignment failed")
	}
}
