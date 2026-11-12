//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"maps"
	"os"
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
	wExited    = 4
	wContinued = 8
	wNowait    = 0x1000000
	pAll       = 0
	pPid       = 1
	pPgid      = 2
	cldExited  = 1
	cldKilled  = 2
	cldStopped = 5
	cldCont    = 6
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
	p.inst.attach(p)
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
	c, err := p.k.newProcess(p)

	if err != nil {
		return p.result(0, err)
	}

	c.setInstance(p.inst)
	c.image, c.vfork = p.image, v
	c.heap, c.static, c.env, c.errnoAt, c.environAt, c.siginfo = p.heap, p.static, p.env, p.errnoAt, p.environAt, p.siginfo
	c.regexes, c.nextRegex, c.constants, c.jumps, c.stackAt = p.regexes, p.nextRegex, p.constants, p.jumps, p.stackAt
	c.locale, c.envStrings, c.mappings, c.tzAt, c.herrnoAt = p.locale, p.envStrings, p.mappings, p.tzAt, p.herrnoAt
	c.token.Store(p.token.Load())
	c.inherit(p)
	p.inst.attach(c)

	return c.result(0, nil)
}

// fork makes a child that is a copy of the process: its memory in use,
// from the stack pointer to the top of the heap, its instance and its
// frames up to _start, on a goroutine of its own; setup changes it before
// it runs. The call fork is in returns 0 in the child. A signal handler
// cannot fork: Go's frames are in the way.
func (p *Process) fork(setup func(c *Process)) (int32, error) {
	if chaosFail("fork") {
		return 0, syscall.EAGAIN
	}

	j := new(Jump)

	if !p.inst.setjmp(j) {
		p.fatal("fork: called through a pointer")
	}

	frames, ok := p.inst.frames(j, "_start")

	if !ok {
		return 0, syscall.ENOSYS
	}

	from := uint64(binary.LittleEndian.Uint32(p.inst.block[p.stackPointer():])) &^ 4095
	to := p.inst.memory.pages * pageSize

	if p.heap != nil {
		to = uint64(p.heap.top)
	}

	c, err := p.k.newProcess(p)

	if err != nil {
		return 0, err
	}

	inst := p.inst.clone(p.k, runStack, from, to)
	inst.attach(c)
	c.image = p.image
	c.heap, c.static, c.env, c.errnoAt, c.environAt, c.siginfo = p.heap.clone(), p.static, p.env, p.errnoAt, p.environAt, p.siginfo
	c.nextRegex, c.stackAt, c.locale, c.tzAt, c.herrnoAt = p.nextRegex, p.stackAt, p.locale, p.tzAt, p.herrnoAt
	c.regexes, c.constants, c.jumps = map[uint32]*posixRegex{}, map[string]uint32{}, map[uint32]*jump{}

	for id, r := range p.regexes {
		copied := *r
		c.regexes[id] = &copied
	}

	for s, at := range p.constants {
		c.constants[s] = at
	}

	c.envStrings, c.mappings = maps.Clone(p.envStrings), map[uint32]mapping{}

	for at, m := range p.mappings {
		if m.file != nil {
			m.file.ref()
			m.read = bytes.Clone(m.read)
		}

		c.mappings[at] = m
	}

	// The jumps of frames deeper than the call have no frames in the child.
	for buf, o := range p.jumps {
		if o.sp() <= j.sp() {
			copied := *o
			c.jumps[buf] = &copied
		}
	}

	c.inherit(p)

	if setup != nil {
		setup(c)
	}

	go c.runFrom(p.image, inst, func() { inst.resume(frames) })

	return c.pid, nil
}

// inherit gives a child its parent's descriptors, signal actions, mask and
// alternate stack.
func (c *Process) inherit(p *Process) {
	c.files = make([]*Descriptor, len(p.files))

	for fd, d := range p.files {
		if d != nil {
			d.obj.ref()
			c.files[fd] = &Descriptor{obj: d.obj, cloexec: d.cloexec}
		}
	}

	p.sig.mu.Lock()
	c.sig.actions, c.sig.blocked, c.sig.altstack = p.sig.actions, p.sig.blocked, p.sig.altstack
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

	i.attach(p)
}

// pidSelector is what waitpid's pid selects of the children: one, the
// caller's group (0), all (-1) or a group.
func (p *Process) pidSelector(pid int32) func(q *Process) bool {
	switch {
	case pid > 0:
		return func(q *Process) bool { return q.pid == pid }
	case pid == 0:
		pgid := p.pgid

		return func(q *Process) bool { return q.pgid == pgid }
	case pid == -1:
		return func(*Process) bool { return true }
	}

	return func(q *Process) bool { return q.pgid == -pid }
}

// reap takes a child selected that ended, stopped or continued, as options
// ask: it returns its pid and status, or 0 and whether any child is
// selected. With wNowait the child stays as it was.
func (p *Process) reap(selects func(*Process) bool, options uint32) (int32, int, bool) {
	k := p.k
	k.mu.Lock()
	defer k.mu.Unlock()

	found := false

	for _, q := range k.procs {
		if q.ppid != p.pid || !selects(q) {
			continue
		}

		found = true

		if q.zombie && options&wExited != 0 {
			if options&wNowait == 0 {
				delete(k.procs, q.pid)
			}

			return q.pid, q.status, true
		}

		if r := q.report; r&0xff == stoppedBy && options&wUntraced != 0 || r == continued && options&wContinued != 0 {
			if options&wNowait == 0 {
				q.report = 0
			}

			return q.pid, r, true
		}
	}

	return 0, 0, found
}

// wait waits until a selected child changes as options ask and returns its
// pid and wait status, or 0 with wNohang while none has.
func (p *Process) wait(selects func(*Process) bool, options uint32) (int32, int, error) {
	for {
		seen := p.childEvents.Load()
		q, status, found := p.reap(selects, options)

		if q != 0 {
			return q, status, nil
		}

		if !found {
			return 0, 0, syscall.ECHILD
		}

		if options&wNohang != 0 {
			return 0, 0, nil
		}

		if err := p.waitEvent(func() bool { return p.childEvents.Load() != seen }); err != nil {
			return 0, 0, err
		}
	}
}

// waitpid is wait4 without the usage of the child.
func (p *Process) waitpid(pid int32, at, options uint32) (int, error) {
	const (
		clone     = uint32(1 << 31)
		all       = uint32(1 << 30)
		notThread = uint32(1 << 29)
		basic     = wNohang | wUntraced | wContinued
	)

	if options&^(basic|clone|all|notThread) != 0 {
		return 0, syscall.EINVAL
	}

	// Every child is a fork child with SIGCHLD, and each process has one thread.
	if options&clone != 0 && options&all == 0 {
		return 0, syscall.ECHILD
	}

	q, status, err := p.wait(p.pidSelector(pid), options&basic|wExited)

	if err == nil && q != 0 && at != 0 {
		p.put32(at, uint32(status))
	}

	return int(q), err
}

// wait4 is waitpid with the child's struct rusage, which is the kernel's:
// all zero.
func (p *Process) wait4(pid int32, at, options, usage uint32) uint32 {
	q, err := p.waitpid(pid, at, options)

	if err == nil && q != 0 && usage != 0 {
		p.putRusage(usage, nil)
	}

	return p.result(q, err)
}

func (p *Process) waitid(idtype, id, info, options uint32) uint32 {
	var selects func(*Process) bool

	switch {
	case idtype == pAll:
		selects = func(*Process) bool { return true }
	case idtype == pPid && int32(id) > 0:
		selects = p.pidSelector(int32(id))
	case idtype == pPgid && int32(id) >= 0:
		pgid := int32(id)

		if pgid == 0 {
			pgid = p.pgid
		}

		selects = func(q *Process) bool { return q.pgid == pgid }
	default:
		return p.result(0, syscall.EINVAL)
	}

	if options&(wExited|wUntraced|wContinued) == 0 || options&^(wNohang|wExited|wUntraced|wContinued|wNowait) != 0 {
		return p.result(0, syscall.EINVAL)
	}

	q, status, err := p.wait(selects, options)

	if err != nil {
		return p.result(0, err)
	}

	if info != 0 {
		clear(p.span(info, 36))

		if q != 0 {
			code, value := cldExited, status>>8&0xff

			switch {
			case status&0xff == stoppedBy:
				code = cldStopped
			case status == continued:
				code, value = cldCont, int(syscall.SIGCONT)
			case status&0x7f != 0:
				code, value = cldKilled, status&0x7f
			}

			p.put32(info, uint32(syscall.SIGCHLD))
			p.put32(info+8, uint32(code))
			p.put32(info+12, uint32(q))
			p.put32(info+16, uint32(os.Getuid()))
			p.put32(info+20, uint32(value))
		}
	}

	return p.result(0, nil)
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

func (p *Process) libcFork() uint32 {
	pid, err := p.fork(nil)

	return p.result(int(pid), err)
}

func (p *Process) libcWaitpid(pid, status, options uint32) uint32 {
	return p.result(p.waitpid(int32(pid), status, options))
}

func (p *Process) libcWait(status uint32) uint32 {
	return p.result(p.waitpid(-1, status, 0))
}

func (p *Process) libcWait4(pid, status, options, usage uint32) uint32 {
	return p.wait4(int32(pid), status, options, usage)
}

func (p *Process) libcVfork() uint32 {
	return p.startVfork()
}

func (p *Process) libcWait3(status, options, usage uint32) uint32 {
	return p.wait4(-1, status, options, usage)
}

func (p *Process) libcWaitid(idtype, id, info, options uint32) uint32 {
	return p.waitid(idtype, id, info, options)
}
