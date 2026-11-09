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

func (k *Kernel) identityFunctions() map[string]any {
	return map[string]any{
		"setuid": func(ctx uintptr, uid uint32) uint32 {
			return proc(ctx).result(0, setIds(syscall.SYS_GETRESUID, uid))
		},
		"seteuid": func(ctx uintptr, uid uint32) uint32 {
			return proc(ctx).result(0, setIds(syscall.SYS_GETRESUID, uid))
		},
		"setreuid": func(ctx uintptr, r, e uint32) uint32 {
			return proc(ctx).result(0, setIds(syscall.SYS_GETRESUID, r, e))
		},
		"setresuid": func(ctx uintptr, r, e, s uint32) uint32 {
			return proc(ctx).result(0, setIds(syscall.SYS_GETRESUID, r, e, s))
		},
		"setgid": func(ctx uintptr, gid uint32) uint32 {
			return proc(ctx).result(0, setIds(syscall.SYS_GETRESGID, gid))
		},
		"setegid": func(ctx uintptr, gid uint32) uint32 {
			return proc(ctx).result(0, setIds(syscall.SYS_GETRESGID, gid))
		},
		"setregid": func(ctx uintptr, r, e uint32) uint32 {
			return proc(ctx).result(0, setIds(syscall.SYS_GETRESGID, r, e))
		},
		"setresgid": func(ctx uintptr, r, e, s uint32) uint32 {
			return proc(ctx).result(0, setIds(syscall.SYS_GETRESGID, r, e, s))
		},
		"getresuid": func(ctx uintptr, r, e, s uint32) uint32 {
			p := proc(ctx)
			ids := hostIds(syscall.SYS_GETRESUID)

			for i, at := range []uint32{r, e, s} {
				p.put32(at, ids[i])
			}

			return p.result(0, nil)
		},
		"getresgid": func(ctx uintptr, r, e, s uint32) uint32 {
			p := proc(ctx)
			ids := hostIds(syscall.SYS_GETRESGID)

			for i, at := range []uint32{r, e, s} {
				p.put32(at, ids[i])
			}

			return p.result(0, nil)
		},
		// setgroups and initgroups need privilege.
		"setgroups": func(ctx uintptr, n, list uint32) uint32 {
			return proc(ctx).result(0, syscall.EPERM)
		},
		"initgroups": func(ctx uintptr, user, gid uint32) uint32 {
			return proc(ctx).result(0, syscall.EPERM)
		},
		"setsid": func(ctx uintptr) uint32 {
			p := proc(ctx)
			k.mu.Lock()
			defer k.mu.Unlock()

			for _, q := range k.procs {
				if q.pgid == p.pid {
					return p.result(0, syscall.EPERM)
				}
			}

			p.sid, p.pgid = p.pid, p.pid

			return p.result(int(p.pid), nil)
		},
		"getsid": func(ctx uintptr, pid uint32) uint32 {
			p := proc(ctx)
			q, err := p.process(int32(pid))

			if err != nil {
				return p.result(0, err)
			}

			k.mu.Lock()
			defer k.mu.Unlock()

			return p.result(int(q.sid), nil)
		},
		"getpgid": func(ctx uintptr, pid uint32) uint32 {
			p := proc(ctx)
			q, err := p.process(int32(pid))

			if err != nil {
				return p.result(0, err)
			}

			k.mu.Lock()
			defer k.mu.Unlock()

			return p.result(int(q.pgid), nil)
		},
		"setpgrp": func(ctx uintptr) uint32 {
			return k.setpgid(ctx, 0, 0)
		},
		// nice returns the new nice value; lowering it needs privilege.
		"nice": func(ctx uintptr, inc uint32) uint32 {
			p := proc(ctx)
			k.mu.Lock()
			n := min(max(p.nice+int32(inc), niceMin), niceMax)

			if n < p.nice && os.Geteuid() != 0 {
				k.mu.Unlock()

				return p.result(0, syscall.EPERM)
			}

			p.nice = n
			k.mu.Unlock()

			return p.result(int(n), nil)
		},
		// getpriority returns the lowest nice value of the processes named.
		"getpriority": func(ctx uintptr, which, who uint32) uint32 {
			p := proc(ctx)
			targets, err := p.prioTargets(which, who)

			if err == nil && len(targets) == 0 {
				err = syscall.ESRCH
			}

			if err != nil {
				return p.result(0, err)
			}

			k.mu.Lock()
			n := int32(niceMax)

			for _, q := range targets {
				n = min(n, q.nice)
			}

			k.mu.Unlock()

			return p.result(int(n), nil)
		},
		"setpriority": func(ctx uintptr, which, who, prio uint32) uint32 {
			p := proc(ctx)
			targets, err := p.prioTargets(which, who)

			if err == nil && len(targets) == 0 {
				err = syscall.ESRCH
			}

			if err != nil {
				return p.result(0, err)
			}

			n := min(max(int32(prio), niceMin), niceMax)
			k.mu.Lock()
			defer k.mu.Unlock()

			for _, q := range targets {
				if n < q.nice && os.Geteuid() != 0 {
					return p.result(0, syscall.EACCES)
				}
			}

			for _, q := range targets {
				q.nice = n
			}

			return p.result(0, nil)
		},
		"sched_yield": func(ctx uintptr) uint32 {
			runtime.Gosched()

			return 0
		},
		"sched_get_priority_max": func(ctx uintptr, policy uint32) uint32 {
			p := proc(ctx)

			switch policy {
			case schedFifo, schedRr:
				return p.result(99, nil)
			case schedOther, schedBatch, schedIdle:
				return p.result(0, nil)
			}

			return p.result(0, syscall.EINVAL)
		},
		"sched_get_priority_min": func(ctx uintptr, policy uint32) uint32 {
			p := proc(ctx)

			switch policy {
			case schedFifo, schedRr:
				return p.result(1, nil)
			case schedOther, schedBatch, schedIdle:
				return p.result(0, nil)
			}

			return p.result(0, syscall.EINVAL)
		},
		"sched_getscheduler": func(ctx uintptr, pid uint32) uint32 {
			p := proc(ctx)
			q, err := p.process(int32(pid))

			if err != nil {
				return p.result(0, err)
			}

			k.mu.Lock()
			defer k.mu.Unlock()

			return p.result(int(q.policy), nil)
		},
		"sched_getparam": func(ctx uintptr, pid, param uint32) uint32 {
			p := proc(ctx)

			if _, err := p.process(int32(pid)); err != nil {
				return p.result(0, err)
			}

			p.put32(param, 0)

			return p.result(0, nil)
		},
		// The priority of the policies the kernel keeps is 0.
		"sched_setparam": func(ctx uintptr, pid, param uint32) uint32 {
			p := proc(ctx)

			if _, err := p.process(int32(pid)); err != nil {
				return p.result(0, err)
			}

			if p.u32(param) != 0 {
				return p.result(0, syscall.EINVAL)
			}

			return p.result(0, nil)
		},
		"sched_setscheduler": func(ctx uintptr, pid, policy, param uint32) uint32 {
			p := proc(ctx)
			q, err := p.process(int32(pid))

			if err != nil {
				return p.result(0, err)
			}

			switch policy {
			case schedOther, schedBatch, schedIdle:
				if p.u32(param) != 0 {
					return p.result(0, syscall.EINVAL)
				}

				k.mu.Lock()
				q.policy = int32(policy)
				k.mu.Unlock()

				return p.result(0, nil)
			case schedFifo, schedRr:
				return p.result(0, syscall.EPERM)
			}

			return p.result(0, syscall.EINVAL)
		},
		// sched_rr_get_interval is the quantum of Linux's SCHED_RR.
		"sched_rr_get_interval": func(ctx uintptr, pid, ts uint32) uint32 {
			p := proc(ctx)

			if _, err := p.process(int32(pid)); err != nil {
				return p.result(0, err)
			}

			p.putTimespec(ts, 100*time.Millisecond)

			return p.result(0, nil)
		},
	}
}
