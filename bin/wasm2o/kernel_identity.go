//go:build linux && amd64

package main

import (
	"os"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

// Who a process is: its credentials, session and group, priority and
// scheduling. Every process has wasm2o's credentials, which none can change:
// setting the ids it has succeeds and does nothing, setting others fails
// as for an unprivileged process. A priority is a nice value, a policy
// SCHED_OTHER, SCHED_BATCH or SCHED_IDLE: the kernel keeps them, Go's
// scheduler does not know them, and the real time policies need privilege.

const (
	prioProcess = 0
	prioPgrp    = 1
	prioUser    = 2
	niceMin     = -20
	niceMax     = 19
	schedOther  = 0
	schedFifo   = 1
	schedRr     = 2
	schedBatch  = 3
	schedIdle   = 5
)

// hostIds are wasm2o's real, effective and saved user or group ids.
func hostIds(trap uintptr) [3]uint32 {
	var ids [3]uint32
	syscall.RawSyscall(trap, uintptr(unsafe.Pointer(&ids[0])), uintptr(unsafe.Pointer(&ids[1])), uintptr(unsafe.Pointer(&ids[2])))

	return ids
}

// setIds checks a change of ids: each one -1, to keep it, or one of the
// ids the process has.
func setIds(trap uintptr, ids ...uint32) error {
	have := hostIds(trap)

	for _, id := range ids {
		if id != ^uint32(0) && id != have[0] && id != have[1] && id != have[2] {
			return syscall.EPERM
		}
	}

	return nil
}

// process is the process of a pid, the caller for 0.
func (p *Process) process(pid int32) (*Process, error) {
	if pid == 0 || pid == p.pid {
		return p, nil
	}

	p.k.mu.Lock()
	q, ok := p.k.procs[pid]
	p.k.mu.Unlock()

	if !ok || q.zombie {
		return nil, syscall.ESRCH
	}

	return q, nil
}

// prioTargets are the processes getpriority and setpriority name.
func (p *Process) prioTargets(which, who uint32) ([]*Process, error) {
	switch which {
	case prioProcess:
		q, err := p.process(int32(who))

		if err != nil {
			return nil, err
		}

		return []*Process{q}, nil
	case prioPgrp:
		pgid := int32(who)

		if pgid == 0 {
			pgid = p.pgid
		}

		return p.k.processes(func(q *Process) bool { return q.pgid == pgid && !q.zombie }), nil
	case prioUser:
		if who != 0 && who != uint32(os.Getuid()) {
			return nil, nil
		}

		return p.k.processes(func(q *Process) bool { return !q.zombie }), nil
	}

	return nil, syscall.EINVAL
}

func (p *Process) libcSetuid(uid uint32) uint32 {
	return p.result(0, setIds(syscall.SYS_GETRESUID, uid))
}

func (p *Process) libcSeteuid(uid uint32) uint32 {
	return p.result(0, setIds(syscall.SYS_GETRESUID, uid))
}

func (p *Process) libcSetreuid(r, e uint32) uint32 {
	return p.result(0, setIds(syscall.SYS_GETRESUID, r, e))
}

func (p *Process) libcSetresuid(r, e, s uint32) uint32 {
	return p.result(0, setIds(syscall.SYS_GETRESUID, r, e, s))
}

func (p *Process) libcSetgid(gid uint32) uint32 {
	return p.result(0, setIds(syscall.SYS_GETRESGID, gid))
}

func (p *Process) libcSetegid(gid uint32) uint32 {
	return p.result(0, setIds(syscall.SYS_GETRESGID, gid))
}

func (p *Process) libcSetregid(r, e uint32) uint32 {
	return p.result(0, setIds(syscall.SYS_GETRESGID, r, e))
}

func (p *Process) libcSetresgid(r, e, s uint32) uint32 {
	return p.result(0, setIds(syscall.SYS_GETRESGID, r, e, s))
}

func (p *Process) libcGetresuid(r, e, s uint32) uint32 {
	ids := hostIds(syscall.SYS_GETRESUID)

	for i, at := range []uint32{r, e, s} {
		p.put32(at, ids[i])
	}

	return p.result(0, nil)
}

func (p *Process) libcGetresgid(r, e, s uint32) uint32 {
	ids := hostIds(syscall.SYS_GETRESGID)

	for i, at := range []uint32{r, e, s} {
		p.put32(at, ids[i])
	}

	return p.result(0, nil)
}

func (p *Process) libcSetsid() uint32 {
	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	for _, q := range p.k.procs {
		if q.pgid == p.pid {
			return p.result(0, syscall.EPERM)
		}
	}

	p.sid, p.pgid = p.pid, p.pid

	return p.result(int(p.pid), nil)
}

func (p *Process) libcGetsid(pid uint32) uint32 {
	q, err := p.process(int32(pid))

	if err != nil {
		return p.result(0, err)
	}

	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	return p.result(int(q.sid), nil)
}

func (p *Process) libcGetpgid(pid uint32) uint32 {
	q, err := p.process(int32(pid))

	if err != nil {
		return p.result(0, err)
	}

	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	return p.result(int(q.pgid), nil)
}

func (p *Process) libcSetpgrp() uint32 {
	return p.libcSetpgid(0, 0)
}

// nice returns the new nice value; lowering it needs privilege.
func (p *Process) libcNice(inc uint32) uint32 {
	p.k.mu.Lock()
	n := min(max(p.nice+int32(inc), niceMin), niceMax)

	if n < p.nice && os.Geteuid() != 0 {
		p.k.mu.Unlock()

		return p.result(0, syscall.EPERM)
	}

	p.nice = n
	p.k.mu.Unlock()

	return p.result(int(n), nil)
}

// getpriority returns the lowest nice value of the processes named.
func (p *Process) libcGetpriority(which, who uint32) uint32 {
	targets, err := p.prioTargets(which, who)

	if err == nil && len(targets) == 0 {
		err = syscall.ESRCH
	}

	if err != nil {
		return p.result(0, err)
	}

	p.k.mu.Lock()
	n := int32(niceMax)

	for _, q := range targets {
		n = min(n, q.nice)
	}

	p.k.mu.Unlock()

	return p.result(int(n), nil)
}

func (p *Process) libcSetpriority(which, who, prio uint32) uint32 {
	targets, err := p.prioTargets(which, who)

	if err == nil && len(targets) == 0 {
		err = syscall.ESRCH
	}

	if err != nil {
		return p.result(0, err)
	}

	n := min(max(int32(prio), niceMin), niceMax)
	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	for _, q := range targets {
		if n < q.nice && os.Geteuid() != 0 {
			return p.result(0, syscall.EACCES)
		}
	}

	for _, q := range targets {
		q.nice = n
	}

	return p.result(0, nil)
}

func (p *Process) libcSched_yield() uint32 {
	runtime.Gosched()

	return 0
}

func (p *Process) libcSched_get_priority_max(policy uint32) uint32 {
	switch policy {
	case schedFifo, schedRr:
		return p.result(99, nil)
	case schedOther, schedBatch, schedIdle:
		return p.result(0, nil)
	}

	return p.result(0, syscall.EINVAL)
}

func (p *Process) libcSched_get_priority_min(policy uint32) uint32 {
	switch policy {
	case schedFifo, schedRr:
		return p.result(1, nil)
	case schedOther, schedBatch, schedIdle:
		return p.result(0, nil)
	}

	return p.result(0, syscall.EINVAL)
}

func (p *Process) libcSched_getscheduler(pid uint32) uint32 {
	q, err := p.process(int32(pid))

	if err != nil {
		return p.result(0, err)
	}

	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	return p.result(int(q.policy), nil)
}

func (p *Process) libcSched_getparam(pid, param uint32) uint32 {
	if _, err := p.process(int32(pid)); err != nil {
		return p.result(0, err)
	}

	p.put32(param, 0)

	return p.result(0, nil)
}

// The priority of the policies the kernel keeps is 0.
func (p *Process) libcSched_setparam(pid, param uint32) uint32 {
	if _, err := p.process(int32(pid)); err != nil {
		return p.result(0, err)
	}

	if p.u32(param) != 0 {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(0, nil)
}

func (p *Process) libcSched_setscheduler(pid, policy, param uint32) uint32 {
	q, err := p.process(int32(pid))

	if err != nil {
		return p.result(0, err)
	}

	switch policy {
	case schedOther, schedBatch, schedIdle:
		if p.u32(param) != 0 {
			return p.result(0, syscall.EINVAL)
		}

		p.k.mu.Lock()
		q.policy = int32(policy)
		p.k.mu.Unlock()

		return p.result(0, nil)
	case schedFifo, schedRr:
		return p.result(0, syscall.EPERM)
	}

	return p.result(0, syscall.EINVAL)
}

// sched_rr_get_interval is the quantum of Linux's SCHED_RR.
func (p *Process) libcSched_rr_get_interval(pid, ts uint32) uint32 {
	if _, err := p.process(int32(pid)); err != nil {
		return p.result(0, err)
	}

	p.putTimespec(ts, 100*time.Millisecond)

	return p.result(0, nil)
}

// setgroups and initgroups need privilege.
func (p *Process) libcSetgroups(n, list uint32) uint32 {
	return p.result(0, syscall.EPERM)
}

func (p *Process) libcInitgroups(user, gid uint32) uint32 {
	return p.result(0, syscall.EPERM)
}
