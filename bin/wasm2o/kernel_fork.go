//go:build linux && amd64

package main

import (
	"encoding/binary"
	"syscall"
)

// Children. A child of vfork runs on its parent's goroutine, in its frames
// and memory, as the instance's process, until it execs, when it goes on
// on a goroutine of its own, or ends; then the parent returns from vfork a
// second time, with the child's pid (jump.go). The parent is suspended
// meanwhile because its goroutine runs the child.
//
// An ended process is a zombie until its parent waits for it; the children
// of an ended process go to the first one.

const (
	wNohang    = 1
	wUntraced  = 2
	wContinued = 8
	// the wait statuses of a stop by sig, sig<<8|stoppedBy, and a continue
	stoppedBy = 0x7f
	continued = 0xffff
)

// vforkFrame is the parent's side of a vfork: the frame to return to and
// its stack pointer.
type vforkFrame struct {
	parent *Process
	jump   Jump
	stack  uint32
}

// resume returns the parent from vfork with pid.
func (v *vforkFrame) resume(pid int32) {
	p := v.parent
	p.inst.host = p
	p.raiseInterrupt()
	binary.LittleEndian.PutUint32(p.inst.block[p.stackPointer():], v.stack)
	p.inst.longjmp(&v.jump, uint32(pid))
	p.fatal("vfork: the function that called vfork has returned")
}

func (p *Process) startVfork() uint32 {
	v := &vforkFrame{parent: p}

	if !p.inst.setjmp(&v.jump) {
		p.fatal("vfork: called through a pointer")
	}

	v.stack = binary.LittleEndian.Uint32(p.inst.block[p.stackPointer():])
	c := p.k.newProcess(p)
	c.setInstance(p.inst)
	c.image, c.vfork = p.image, v
	c.heap, c.static, c.env, c.errnoAt, c.environAt, c.siginfo = p.heap, p.static, p.env, p.errnoAt, p.environAt, p.siginfo
	c.regexes, c.nextRegex, c.zoneNames, c.jumps, c.stackAt = p.regexes, p.nextRegex, p.zoneNames, p.jumps, p.stackAt
	c.token.Store(p.token.Load())
	c.inherit(p)
	p.inst.host = c

	return c.result(0, nil)
}

// startFork makes a child that is a copy of the process: its memory in use,
// from the stack pointer to the top of the heap, its instance and its
// frames up to _start, on a goroutine of its own. A signal handler cannot
// fork: Go's frames are in the way.
func (p *Process) startFork() uint32 {
	j := new(Jump)

	if !p.inst.setjmp(j) {
		p.fatal("fork: called through a pointer")
	}

	frames, ok := p.inst.frames(j, "_start")

	if !ok {
		return p.result(0, syscall.ENOSYS)
	}

	from := uint64(binary.LittleEndian.Uint32(p.inst.block[p.stackPointer():])) &^ 4095
	to := p.inst.memory.pages * pageSize

	if p.heap != 0 {
		to = uint64(p.u32(p.heap + heapTop))
	}

	c := p.k.newProcess(p)
	inst := p.inst.clone(p.k, runStack, from, to)
	inst.host = c
	c.image = p.image
	c.heap, c.static, c.env, c.errnoAt, c.environAt, c.siginfo = p.heap, p.static, p.env, p.errnoAt, p.environAt, p.siginfo
	c.nextRegex, c.stackAt = p.nextRegex, p.stackAt
	c.regexes, c.zoneNames, c.jumps = map[uint32]*posixRegex{}, map[string]uint32{}, map[uint32]*jump{}

	for id, r := range p.regexes {
		copied := *r
		c.regexes[id] = &copied
	}

	for name, at := range p.zoneNames {
		c.zoneNames[name] = at
	}

	// The jumps of frames deeper than the call have no frames in the child.
	for buf, o := range p.jumps {
		if o.sp() <= j.sp() {
			copied := *o
			c.jumps[buf] = &copied
		}
	}

	c.inherit(p)
	go c.runFrom(p.image, inst, func() { inst.resume(frames) })

	return p.result(int(c.pid), nil)
}

// inherit gives a child its parent's descriptors, signal actions and mask.
func (c *Process) inherit(p *Process) {
	c.files = make([]*Descriptor, len(p.files))

	for fd, d := range p.files {
		if d != nil {
			c.files[fd] = &Descriptor{file: d.file.ref(), cloexec: d.cloexec}
		}
	}

	p.sig.mu.Lock()
	c.sig.actions, c.sig.blocked = p.sig.actions, p.sig.blocked
	p.sig.mu.Unlock()
}

// execImage starts a program exec found: it unwinds the old one, or a
// child of vfork goes on with it on a goroutine of its own and gives its
// parent's frames back.
func (p *Process) execImage(img *execImage) {
	v := p.vfork

	if v == nil {
		panic(img)
	}

	p.vfork = nil
	p.execReset()
	p.setInstance(nil)
	go p.run(img)
	v.resume(p.pid)
}

// endBorrowers ends the children of vfork still on the frames an image
// unwound, as a trap in one of them does: they go with it.
func (p *Process) endBorrowers(i *Instance, e any) {
	for q := i.host.(*Process); q != p; {
		v := q.vfork
		q.vfork = nil
		status := int(syscall.SIGKILL)

		if t, ok := e.(*Trap); ok {
			status = trapSignal(t.code)
		}

		q.terminate(status)
		q = v.parent
	}

	i.host = p
}

// waitSelects tells whether waitpid's pid selects child q.
func (p *Process) waitSelects(pid int32, q *Process) bool {
	switch {
	case pid > 0:
		return q.pid == pid
	case pid == 0:
		return q.pgid == p.pgid
	case pid == -1:
		return true
	}

	return q.pgid == -pid
}

// reap takes a child pid selects that ended, or stopped or continued when
// options ask: it returns its pid and status, or 0 and whether any child
// is selected.
func (p *Process) reap(pid int32, options uint32) (int32, int, bool) {
	k := p.k
	k.mu.Lock()
	defer k.mu.Unlock()

	found := false

	for _, q := range k.procs {
		if q.ppid != p.pid || !p.waitSelects(pid, q) {
			continue
		}

		if q.zombie {
			delete(k.procs, q.pid)

			return q.pid, q.status, true
		}

		if r := q.report; r&0xff == stoppedBy && options&wUntraced != 0 || r == continued && options&wContinued != 0 {
			q.report = 0

			return q.pid, r, true
		}

		found = true
	}

	return 0, 0, found
}

func (p *Process) waitpid(pid int32, at, options uint32) (int, error) {
	for {
		seen := p.childEvents.Load()
		q, status, found := p.reap(pid, options)

		if q != 0 {
			if at != 0 {
				p.put32(at, uint32(status))
			}

			return int(q), nil
		}

		if !found {
			return 0, syscall.ECHILD
		}

		if options&wNohang != 0 {
			return 0, nil
		}

		if err := p.waitEvent(func() bool { return p.childEvents.Load() != seen }); err != nil {
			return 0, err
		}
	}
}

// stopped stops the process by sig: its parent learns it, and it waits
// until it is continued or a SIGKILL is pending.
func (p *Process) stopped(sig int) {
	p.k.mu.Lock()
	p.halted, p.report = true, sig<<8|stoppedBy
	p.k.mu.Unlock()
	p.tellParent(saNocldstop)
	wake := []pollFd{{fd: int32(p.wake), events: pollIn}}

	for {
		var b [8]byte
		syscall.Read(p.wake, b[:])
		p.k.mu.Lock()
		halted := p.halted
		p.k.mu.Unlock()
		p.sig.mu.Lock()
		killed := p.sig.pending&sigBit(int(syscall.SIGKILL)) != 0
		p.sig.mu.Unlock()

		if !halted || killed {
			return
		}

		wake[0].revents = 0
		poll(wake, -1)
	}
}

// cont continues the process if it is stopped.
func (p *Process) cont() {
	p.k.mu.Lock()
	was := p.halted

	if was {
		p.halted, p.report = false, continued
	}

	p.k.mu.Unlock()

	if was {
		p.wakeUp()
		p.tellParent(saNocldstop)
	}
}

// tellParent tells the parent of a change of the process: SIGCHLD unless
// the parent's action has flag.
func (p *Process) tellParent(flag uint32) {
	p.k.mu.Lock()
	parent := p.k.procs[p.ppid]
	p.k.mu.Unlock()

	if parent == nil {
		return
	}

	parent.sig.mu.Lock()
	quiet := parent.sig.actions[syscall.SIGCHLD].flags&flag != 0
	parent.sig.mu.Unlock()

	if quiet {
		parent.childChanged(0)
	} else {
		parent.childChanged(int(syscall.SIGCHLD))
	}
}

// waitEvent waits until ready holds, or a signal interrupts: then the
// error is EINTR, after the handlers ran when they all restart calls.
// Ready comes first, as a call that has its result returns it.
func (p *Process) waitEvent(ready func() bool) error {
	wake := []pollFd{{fd: int32(p.wake), events: pollIn}}

	for {
		signal := p.wakeSignal()

		if ready() {
			return nil
		}

		if signal {
			if p.interrupted() {
				continue
			}

			return syscall.EINTR
		}

		wake[0].revents = 0

		if _, err := poll(wake, -1); err != nil && err != syscall.EINTR {
			return err
		}
	}
}

func (k *Kernel) forkFunctions() map[string]any {
	return map[string]any{
		"vfork": func(ctx uintptr) uint32 {
			return proc(ctx).startVfork()
		},
		"fork": func(ctx uintptr) uint32 {
			return proc(ctx).startFork()
		},
		"waitpid": func(ctx uintptr, pid, status, options uint32) uint32 {
			p := proc(ctx)

			return p.result(p.waitpid(int32(pid), status, options))
		},
		"wait": func(ctx uintptr, status uint32) uint32 {
			p := proc(ctx)

			return p.result(p.waitpid(-1, status, 0))
		},
	}
}
