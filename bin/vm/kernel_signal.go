//go:build linux && amd64

package main

import (
	"encoding/binary"
	"math"
	"math/bits"
	"os"
	"os/signal"
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// Signals are the kernel's: a process has its actions, blocked mask and
// pending set; sending one sets it pending and wakes the process, which
// takes it on its way out of a system call, in a blocking wait, or, when
// it runs its own code, as Go interrupts that (lib/goruntime). A handler
// is a function of the guest's table, called through the exports of the
// crt. Signals of the host's terminal go to its foreground group, the
// host's others to the first process.

const (
	sigDfl      = 0
	sigIgn      = ^uint32(1)
	saNocldstop = 1
	saNocldwait = 2
	saSiginfo   = 4
	saOnstack   = 0x08000000
	saRestart   = 0x10000000
	saNodefer   = 0x40000000
	saResethand = 0x80000000
	sigBlock    = 0
	sigUnblock  = 1
	sigSetmask  = 2
	nsig        = 65
	ssOnstack   = 1
	ssDisable   = 2
	minSigstksz = 2048
)

// What a signal does by default.
const (
	actTerminate = iota
	actIgnore
	actStop
	actContinue
)

func defaultAction(sig int) int {
	switch syscall.Signal(sig) {
	case syscall.SIGCHLD, syscall.SIGURG, syscall.SIGWINCH:
		return actIgnore
	case syscall.SIGCONT:
		return actContinue
	case syscall.SIGSTOP, syscall.SIGTSTP, syscall.SIGTTIN, syscall.SIGTTOU:
		return actStop
	}

	return actTerminate
}

func sigBit(sig int) uint64 {
	return 1 << (sig - 1)
}

// stopSignals stop a process by default; SIGCONT and they discard each
// other when sent.
const stopSignals = 1<<(syscall.SIGSTOP-1) | 1<<(syscall.SIGTSTP-1) | 1<<(syscall.SIGTTIN-1) | 1<<(syscall.SIGTTOU-1)

// sent discards what a signal sent cancels: SIGCONT the pending stops, a
// stop a pending SIGCONT; the caller holds the lock.
func (s *signalState) sent(sig int) {
	switch {
	case sig == int(syscall.SIGCONT):
		s.pending &^= stopSignals
	case sigBit(sig)&stopSignals != 0:
		s.pending &^= sigBit(int(syscall.SIGCONT))
	}
}

// unblockable is what no mask holds: SIGKILL and SIGSTOP.
const unblockable = 1<<(syscall.SIGKILL-1) | 1<<(syscall.SIGSTOP-1)

type sigAction struct {
	handler uint32
	flags   uint32
	mask    uint64
}

type signalState struct {
	mu      sync.Mutex
	actions [nsig]sigAction
	blocked uint64
	pending uint64
	// dead is set when the process ended and closed its wake descriptor.
	dead bool
	// altstack is the stack of sigaltstack, none when its size is 0.
	altstack struct{ sp, size uint32 }
}

func (s *signalState) deliverable() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()

	return s.pending &^ s.blocked
}

// ignored tells whether a signal is discarded when sent.
func (a sigAction) ignored(sig int) bool {
	return a.handler == sigIgn || a.handler == sigDfl && defaultAction(sig) == actIgnore
}

// send makes a signal pending for the process and wakes it. A fatal signal
// for the first process ends vm at once: the process may be running
// code that takes no signals. SIGCONT continues a stopped process even
// when it ignores it.
func (p *Process) send(sig int) {
	if sig == int(syscall.SIGCONT) {
		p.cont()
	}

	p.sig.mu.Lock()
	p.sig.sent(sig)
	act := p.sig.actions[sig]
	blocked := p.sig.blocked&sigBit(sig) != 0

	if act.ignored(sig) && !blocked {
		p.sig.mu.Unlock()

		return
	}

	if p == p.k.first && !blocked && act.handler == sigDfl && defaultAction(sig) == actTerminate {
		os.Exit(128 + sig)
	}

	p.sig.pending |= sigBit(sig)
	token := p.token.Load()

	if p.sig.dead || blocked {
		token = 0
	} else {
		p.setInterrupt(1)
	}

	p.sig.mu.Unlock()
	p.wakeUp()

	if token != 0 {
		runtime.Interrupt(token)
		p.chase()
	}
}

// chase interrupts the process again until it takes its deliverable
// signals: Go's signal may find it outside its own code, in a stub, or in a
// long call of the kernel. It gives up when the process stops or leaves its
// image, and after 32 ms it tries every 10 ms.
func (p *Process) chase() {
	if !p.chasing.CompareAndSwap(false, true) {
		return
	}

	go func() {
		for k := 0; ; k++ {
			wait := time.Millisecond

			if k >= 32 {
				wait = 10 * time.Millisecond
			}

			time.Sleep(wait)
			token := p.token.Load()
			p.k.mu.Lock()
			halted := p.halted
			p.k.mu.Unlock()

			if token != 0 && !halted && p.sig.deliverable() != 0 {
				runtime.Interrupt(token)

				continue
			}

			p.chasing.Store(false)

			// a signal sent meanwhile found the flag set
			if token == 0 || halted || p.sig.deliverable() == 0 || !p.chasing.CompareAndSwap(false, true) {
				return
			}
		}
	}()
}

// setInstance sets the instance the process runs, under the lock of its
// signals, which write the instance's interrupt word.
func (p *Process) setInstance(i *Instance) {
	p.sig.mu.Lock()
	p.inst = i
	p.sig.mu.Unlock()
}

// setInterrupt sets the interrupt word of the process's instance: while it
// is not zero, every return from an import interrupts the code. The caller
// holds the lock of the signals.
func (p *Process) setInterrupt(v uint32) {
	if p.inst != nil && !p.sig.dead {
		atomic.StoreUint32((*uint32)(unsafe.Pointer(&p.inst.block[offInterrupt])), v)
	}
}

// raiseInterrupt sets the interrupt word when a signal is deliverable.
func (p *Process) raiseInterrupt() {
	p.sig.mu.Lock()

	if p.sig.pending&^p.sig.blocked != 0 {
		p.setInterrupt(1)
	}

	p.sig.mu.Unlock()
}

// redZone is how far below __stack_pointer a leaf function keeps its frame
// without moving it, as LLVM's wasm code does.
const redZone = 128

// interrupt takes the deliverable signals where the process's own code was
// interrupted, by Go or on its way out of an import: a handler's frames on
// the shadow stack go below the red zone of the function interrupted.
// Taking them all clears the interrupt word.
func (p *Process) interrupt() {
	at, ok := p.inst.globalAt("__stack_pointer")

	if !ok {
		p.deliver()

		return
	}

	sp := binary.LittleEndian.Uint32(p.inst.block[at:])
	binary.LittleEndian.PutUint32(p.inst.block[at:], sp-redZone)
	p.deliver()
	binary.LittleEndian.PutUint32(p.inst.block[at:], sp)
}

// wakeUp makes a blocking call of the process look again: for a signal, or
// a change of its children.
func (p *Process) wakeUp() {
	p.sig.mu.Lock()

	if !p.sig.dead {
		var one = [8]byte{1}
		syscall.Write(p.wake, one[:])
	}

	p.sig.mu.Unlock()
}

// sendSelf sends a signal the process raised itself; it takes it on its
// way out of the call.
func (p *Process) sendSelf(sig int) {
	p.sig.mu.Lock()
	p.sig.sent(sig)
	act := p.sig.actions[sig]

	if !act.ignored(sig) || p.sig.blocked&sigBit(sig) != 0 {
		p.sig.pending |= sigBit(sig)
	}

	p.sig.mu.Unlock()
}

// take removes the lowest deliverable signal and returns it with its
// action, or 0.
func (p *Process) take() (int, sigAction) {
	p.sig.mu.Lock()
	defer p.sig.mu.Unlock()

	ready := p.sig.pending &^ p.sig.blocked

	if ready == 0 {
		p.setInterrupt(0)

		return 0, sigAction{}
	}

	sig := bits.TrailingZeros64(ready) + 1
	p.sig.pending &^= sigBit(sig)
	act := p.sig.actions[sig]

	if act.handler != sigDfl && act.handler != sigIgn && act.flags&saResethand != 0 {
		p.sig.actions[sig] = sigAction{}
	}

	return sig, act
}

// deliver takes the pending signals: it runs their handlers and their
// default actions.
func (p *Process) deliver() {
	p.handleSignals()
}

// interrupted takes the signals that interrupt a blocking call and tells
// whether the call restarts: when every handler run asked for SA_RESTART.
func (p *Process) interrupted() bool {
	return p.handleSignals()
}

func (p *Process) handleSignals() bool {
	restart := true

	for {
		sig, act := p.take()

		if sig == 0 {
			return restart
		}

		switch act.handler {
		case sigIgn:
		case sigDfl:
			switch defaultAction(sig) {
			case actTerminate:
				p.exitWith(sig)
			case actStop:
				p.stop(sig)
			}
		default:
			if act.flags&saRestart == 0 {
				restart = false
			}

			p.runHandler(sig, act)
		}
	}
}

// stop stops the process until SIGCONT or SIGKILL (kernel_fork.go). The
// first process stops vm, so the host's shell gets the terminal back.
func (p *Process) stop(sig int) {
	if p == p.k.first {
		syscall.Kill(os.Getpid(), syscall.SIGSTOP)

		return
	}

	p.stopped(sig)
}

// runHandler calls a handler with the action's mask added for its time;
// errno is the interrupted code's again after it. A handler may leave by
// longjmp (kernel_jump.go), and then nothing after the call runs: here,
// nor in the kernel's frames above.
func (p *Process) runHandler(sig int, act sigAction) {
	p.sig.mu.Lock()
	saved := p.sig.blocked
	p.sig.blocked |= act.mask &^ unblockable

	if act.flags&saNodefer == 0 {
		p.sig.blocked |= sigBit(sig)
	}

	alt := p.sig.altstack
	p.sig.mu.Unlock()
	errno := p.errno()
	ctx := p.inst.ctx()

	// A handler of SA_ONSTACK runs on the alternate stack, unless it is
	// on it already; a longjmp out of it restores the stack pointer.
	sp := binary.LittleEndian.Uint32(p.inst.block[p.stackPointer():])
	moved := act.flags&saOnstack != 0 && alt.size != 0 && sp-alt.sp > alt.size

	if moved {
		binary.LittleEndian.PutUint32(p.inst.block[p.stackPointer():], (alt.sp+alt.size)&^15)
	}

	if act.flags&saSiginfo != 0 {
		if p.siginfo == 0 {
			p.siginfo = p.malloc(36)
		}

		info := p.span(p.siginfo, 36)
		clear(info)
		p.put32(p.siginfo, uint32(sig))
		p.put32(p.siginfo+12, uint32(p.pid))
		f, _ := p.inst.export("__call_signal_action")
		f.Interface().(func(uintptr, uint32, uint32, uint32, uint32))(ctx, act.handler, uint32(sig), p.siginfo, 0)
	} else {
		f, _ := p.inst.export("__call_signal_handler")
		f.Interface().(func(uintptr, uint32, uint32))(ctx, act.handler, uint32(sig))
	}

	if moved {
		binary.LittleEndian.PutUint32(p.inst.block[p.stackPointer():], sp)
	}

	p.setErrno(errno)
	p.sig.mu.Lock()
	p.sig.blocked = saved
	p.sig.mu.Unlock()
}

func (p *Process) sigprocmask(how, set, old uint32) error {
	mask := p.mask()

	if old != 0 {
		p.put64(old, mask)
	}

	if set != 0 {
		s := p.u64(set)

		switch how {
		case sigBlock:
			mask |= s
		case sigUnblock:
			mask &^= s
		case sigSetmask:
			mask = s
		default:
			return syscall.EINVAL
		}

		p.setMask(mask)
	}

	return nil
}

// sigwait waits until a signal of set is pending and takes it, for at most
// timeout when that is not negative, else EAGAIN. Another signal the process
// would deliver interrupts the wait: its handler runs and the error is
// EINTR.
func (p *Process) sigwait(set uint64, timeout time.Duration) (int, error) {
	set &^= unblockable
	end := time.Now().Add(timeout)
	wake := []pollFd{{fd: int32(p.wake), events: pollIn}}

	for {
		var b [8]byte
		syscall.Read(p.wake, b[:])
		p.sig.mu.Lock()

		if ready := p.sig.pending & set; ready != 0 {
			sig := bits.TrailingZeros64(ready) + 1
			p.sig.pending &^= sigBit(sig)
			p.sig.mu.Unlock()

			return sig, nil
		}

		other := p.sig.pending &^ p.sig.blocked
		p.sig.mu.Unlock()

		if other != 0 {
			p.handleSignals()

			return 0, syscall.EINTR
		}

		ms := -1

		if timeout >= 0 {
			left := time.Until(end)

			if left <= 0 {
				return 0, syscall.EAGAIN
			}

			ms = int(min(left.Milliseconds()+1, 1<<30))
		}

		wake[0].revents = 0
		poll(wake, ms)
	}
}

// putSiginfo writes the siginfo_t of a signal sigwaitinfo takes.
func (p *Process) putSiginfo(at uint32, sig int) {
	if at != 0 {
		clear(p.span(at, 36))
		p.put32(at, uint32(sig))
	}
}

// setMember checks a signal a sigset_t takes: not 32 and 33, which
// sigfillset leaves out as musl's.
func (p *Process) setMember(sig uint32) bool {
	if !validSignal(sig) || sig == 32 || sig == 33 {
		p.setErrno(syscall.EINVAL)

		return false
	}

	return true
}

func (p *Process) setAction(sig int, act sigAction) sigAction {
	p.sig.mu.Lock()
	defer p.sig.mu.Unlock()

	old := p.sig.actions[sig]
	p.sig.actions[sig] = act

	if act.ignored(sig) {
		p.sig.pending &^= sigBit(sig)
	}

	return old
}

func (p *Process) setMask(mask uint64) uint64 {
	p.sig.mu.Lock()
	defer p.sig.mu.Unlock()

	old := p.sig.blocked
	p.sig.blocked = mask &^ unblockable

	return old
}

func (p *Process) mask() uint64 {
	p.sig.mu.Lock()
	defer p.sig.mu.Unlock()

	return p.sig.blocked
}

func validSignal(sig uint32) bool {
	return sig >= 1 && sig < nsig
}

// signalTargets are the processes kill sends to: one pid, a group (pid 0
// is the sender's, -1 every process but the first and the sender).
func (k *Kernel) signalTargets(from *Process, pid int32) []*Process {
	return k.processes(func(q *Process) bool {
		switch {
		case pid > 0:
			return q.pid == pid
		case pid == 0:
			return q.pgid == from.pgid
		case pid == -1:
			return q.pid != 1 && q != from
		}

		return q.pgid == -pid
	})
}

// processes are those that match.
func (k *Kernel) processes(match func(*Process) bool) []*Process {
	k.mu.Lock()
	defer k.mu.Unlock()

	var out []*Process

	for _, q := range k.procs {
		if match(q) {
			out = append(out, q)
		}
	}

	return out
}

func (p *Process) kill(pid int32, sig uint32) error {
	if sig != 0 && !validSignal(sig) {
		return syscall.EINVAL
	}

	targets := p.k.signalTargets(p, pid)

	if len(targets) == 0 {
		return syscall.ESRCH
	}

	if sig != 0 {
		for _, q := range targets {
			if q == p {
				q.sendSelf(int(sig))
			} else {
				q.send(int(sig))
			}
		}
	}

	return nil
}

func (p *Process) libcSignal(sig, handler uint32) uint32 {
	if !validSignal(sig) || sig == uint32(syscall.SIGKILL) || sig == uint32(syscall.SIGSTOP) {
		p.setErrno(syscall.EINVAL)

		return ^uint32(0)
	}

	return p.setAction(int(sig), sigAction{handler: handler, flags: saRestart}).handler
}

func (p *Process) libcSigaction(sig, act, old uint32) uint32 {
	if !validSignal(sig) || act != 0 && (sig == uint32(syscall.SIGKILL) || sig == uint32(syscall.SIGSTOP)) {
		return p.result(0, syscall.EINVAL)
	}

	p.sig.mu.Lock()
	prev := p.sig.actions[sig]
	p.sig.mu.Unlock()

	if act != 0 {
		p.setAction(int(sig), sigAction{handler: p.u32(act), mask: p.u64(act + 8), flags: p.u32(act + 16)})
	}

	if old != 0 {
		clear(p.span(old, 24))
		p.put32(old, prev.handler)
		p.put64(old+8, prev.mask)
		p.put32(old+16, prev.flags)
	}

	return p.result(0, nil)
}

func (p *Process) libcSigprocmask(how, set, old uint32) uint32 {
	return p.result(0, p.sigprocmask(how, set, old))
}

// pthread_sigmask is sigprocmask of the process's one thread; it
// returns an error number.
func (p *Process) libcPthread_sigmask(how, set, old uint32) uint32 {
	err := p.sigprocmask(how, set, old)
	p.deliver()

	if err != nil {
		return uint32(errnoOf(err))
	}

	return 0
}

func (p *Process) libcSigfillset(set uint32) uint32 {
	p.put64(set, ^uint64(0)&^(3<<31))

	return 0
}

func (p *Process) libcSigemptyset(set uint32) uint32 {
	p.put64(set, 0)

	return 0
}

func (p *Process) libcSigaddset(set, sig uint32) uint32 {
	if !p.setMember(sig) {
		return math.MaxUint32
	}

	p.put64(set, p.u64(set)|sigBit(int(sig)))

	return 0
}

func (p *Process) libcSigdelset(set, sig uint32) uint32 {
	if !p.setMember(sig) {
		return math.MaxUint32
	}

	p.put64(set, p.u64(set)&^sigBit(int(sig)))

	return 0
}

func (p *Process) libcSigismember(set, sig uint32) uint32 {
	return bit(validSignal(sig) && p.u64(set)&sigBit(int(sig)) != 0)
}

func (p *Process) libcSigisemptyset(set uint32) uint32 {
	return bit(p.u64(set) == 0)
}

func (p *Process) libcSigorset(d, a, b uint32) uint32 {
	p.put64(d, p.u64(a)|p.u64(b))

	return 0
}

func (p *Process) libcSigandset(d, a, b uint32) uint32 {
	p.put64(d, p.u64(a)&p.u64(b))

	return 0
}

func (p *Process) libcSigpending(set uint32) uint32 {
	p.sig.mu.Lock()
	pending := p.sig.pending
	p.sig.mu.Unlock()
	p.put64(set, pending)

	return p.result(0, nil)
}

// sigwait returns an error number.
func (p *Process) libcSigwait(set, at uint32) uint32 {
	for {
		sig, err := p.sigwait(p.u64(set), -1)

		if err == nil {
			p.put32(at, uint32(sig))

			return 0
		}
	}
}

func (p *Process) libcSigwaitinfo(set, info uint32) uint32 {
	sig, err := p.sigwait(p.u64(set), -1)

	if err == nil {
		p.putSiginfo(info, sig)
	}

	return p.result(sig, err)
}

func (p *Process) libcSigtimedwait(set, info, timeout uint32) uint32 {
	d := time.Duration(-1)

	if timeout != 0 {
		var err error

		if d, err = p.timespec(timeout); err != nil {
			return p.result(0, err)
		}
	}

	sig, err := p.sigwait(p.u64(set), d)

	if err == nil {
		p.putSiginfo(info, sig)
	}

	return p.result(sig, err)
}

func (p *Process) libcSigaltstack(ss, old uint32) uint32 {
	sp := binary.LittleEndian.Uint32(p.inst.block[p.stackPointer():])
	p.sig.mu.Lock()
	alt := p.sig.altstack
	p.sig.mu.Unlock()
	on := alt.size != 0 && sp-alt.sp <= alt.size

	if old != 0 {
		flags := uint32(0)

		switch {
		case alt.size == 0:
			flags = ssDisable
		case on:
			flags = ssOnstack
		}

		p.put32(old, alt.sp)
		p.put32(old+4, flags)
		p.put32(old+8, alt.size)
	}

	if ss == 0 {
		return p.result(0, nil)
	}

	flags := p.u32(ss + 4)

	switch {
	case on:
		return p.result(0, syscall.EPERM)
	case flags&^(ssOnstack|ssDisable|1<<31) != 0:
		return p.result(0, syscall.EINVAL)
	case flags&ssDisable != 0:
		alt.sp, alt.size = 0, 0
	case p.u32(ss+8) < minSigstksz:
		return p.result(0, syscall.ENOMEM)
	default:
		alt.sp, alt.size = p.u32(ss), p.u32(ss+8)
		p.span(alt.sp, alt.size)
	}

	p.sig.mu.Lock()
	p.sig.altstack = alt
	p.sig.mu.Unlock()

	return p.result(0, nil)
}

// siginterrupt makes a signal's handler interrupt calls, or restart
// them.
func (p *Process) libcSiginterrupt(sig, flag uint32) uint32 {
	if !validSignal(sig) {
		return p.result(0, syscall.EINVAL)
	}

	p.sig.mu.Lock()

	if flag != 0 {
		p.sig.actions[sig].flags &^= saRestart
	} else {
		p.sig.actions[sig].flags |= saRestart
	}

	p.sig.mu.Unlock()

	return p.result(0, nil)
}

func (p *Process) libcPause() uint32 {
	for {
		if _, err := p.waitFds(nil, -1, false); err != nil {
			break
		}
	}

	return p.result(0, syscall.EINTR)
}

func (p *Process) libcSigsuspend(set uint32) uint32 {
	old := p.setMask(p.u64(set))

	for {
		if _, err := p.waitFds(nil, -1, false); err != nil {
			break
		}
	}

	p.handleSignals()
	p.setMask(old)

	return p.result(0, syscall.EINTR)
}

func (p *Process) libcRaise(sig uint32) uint32 {
	if !validSignal(sig) {
		return p.result(0, syscall.EINVAL)
	}

	p.sendSelf(int(sig))

	return p.result(0, nil)
}

func (p *Process) libcKill(pid, sig uint32) uint32 {
	return p.result(0, p.kill(int32(pid), sig))
}

// sigqueue sends a signal as kill does; its value does not reach
// the handler.
func (p *Process) libcSigqueue(pid, sig, value uint32) uint32 {
	if int32(pid) <= 0 {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(0, p.kill(int32(pid), sig))
}

func (p *Process) libcKillpg(pgrp, sig uint32) uint32 {
	if int32(pgrp) <= 1 {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(0, p.kill(-int32(pgrp), sig))
}

// abort raises SIGABRT, unblocked, and when a handler returns from it ends
// the process by it.
func (p *Process) abort() {
	abrt := int(syscall.SIGABRT)
	p.setMask(p.mask() &^ sigBit(abrt))
	p.sendSelf(abrt)
	p.deliver()
	p.setAction(abrt, sigAction{})
	p.exitWith(abrt)
}

// routeSignals takes the host's signals for the processes.
func (k *Kernel) routeSignals() {
	c := make(chan os.Signal, 16)
	signal.Notify(c, syscall.SIGINT, syscall.SIGQUIT, syscall.SIGTSTP, syscall.SIGWINCH, syscall.SIGTERM, syscall.SIGHUP,
		syscall.SIGUSR1, syscall.SIGUSR2, syscall.SIGCONT, syscall.SIGALRM)

	go func() {
		for s := range c {
			sig := int(s.(syscall.Signal))

			switch s {
			case syscall.SIGINT, syscall.SIGQUIT, syscall.SIGTSTP, syscall.SIGWINCH:
				k.mu.Lock()
				fg := k.fg
				k.mu.Unlock()

				for _, q := range k.processes(func(q *Process) bool { return q.pgid == fg }) {
					q.send(sig)
				}
			default:
				if k.first != nil {
					k.first.send(sig)
				}
			}
		}
	}()
}

func (p *Process) libcAbort() {
	p.abort()
}
