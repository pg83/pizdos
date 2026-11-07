//go:build linux && amd64

package main

import (
	"math/bits"
	"os"
	"os/signal"
	"sync"
	"syscall"
)

// Signals are the kernel's: a process has its actions, blocked mask and
// pending set; sending one sets it pending and wakes the process, which
// takes it on its way out of a system call or in a blocking wait. A
// handler is a function of the guest's table, called through the exports
// of the crt. Signals of the host's terminal go to its foreground group,
// the host's others to the first process.

const (
	sigDfl      = 0
	sigIgn      = 1
	saSiginfo   = 4
	saRestart   = 0x10000000
	saNodefer   = 0x40000000
	saResethand = 0x80000000
	sigBlock    = 0
	sigUnblock  = 1
	sigSetmask  = 2
	nsig        = 65
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
// for the first process ends wasm2o at once: the process may be running
// code that takes no signals.
func (p *Process) send(sig int) {
	p.sig.mu.Lock()
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
	p.sig.mu.Unlock()
	var one = [8]byte{1}
	syscall.Write(p.wake, one[:])
}

// sendSelf sends a signal the process raised itself; it takes it on its
// way out of the call.
func (p *Process) sendSelf(sig int) {
	p.sig.mu.Lock()
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
		return 0, sigAction{}
	}

	sig := bits.TrailingZeros64(ready) + 1
	p.sig.pending &^= sigBit(sig)
	act := p.sig.actions[sig]

	if act.handler > sigIgn && act.flags&saResethand != 0 {
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
				p.stop()
			}
		default:
			if act.flags&saRestart == 0 {
				restart = false
			}

			p.runHandler(sig, act)
		}
	}
}

// stop stops the process. The first process stops wasm2o, so the host's
// shell gets the terminal back.
func (p *Process) stop() {
	if p == p.k.first {
		syscall.Kill(os.Getpid(), syscall.SIGSTOP)
	}
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

	p.sig.mu.Unlock()
	errno := p.errno()
	ctx := p.inst.ctx()

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

	p.setErrno(errno)
	p.sig.mu.Lock()
	p.sig.blocked = saved
	p.sig.mu.Unlock()
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
// is the sender's, -1 every process).
func (k *Kernel) signalTargets(from *Process, pid int32) []*Process {
	return k.processes(func(q *Process) bool {
		switch {
		case pid > 0:
			return q.pid == pid
		case pid == 0:
			return q.pgid == from.pgid
		case pid == -1:
			return q.pid != 1
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

func (k *Kernel) signalFunctions() map[string]any {
	return map[string]any{
		"signal": func(ctx uintptr, sig, handler uint32) uint32 {
			p := proc(ctx)

			if !validSignal(sig) || sig == uint32(syscall.SIGKILL) || sig == uint32(syscall.SIGSTOP) {
				p.setErrno(syscall.EINVAL)

				return ^uint32(0)
			}

			return p.setAction(int(sig), sigAction{handler: handler, flags: saRestart}).handler
		},
		"sigaction": func(ctx uintptr, sig, act, old uint32) uint32 {
			p := proc(ctx)

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
		},
		"sigprocmask": func(ctx uintptr, how, set, old uint32) uint32 {
			p := proc(ctx)
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
					return p.result(0, syscall.EINVAL)
				}

				p.setMask(mask)
			}

			return p.result(0, nil)
		},
		"sigfillset": func(ctx uintptr, set uint32) uint32 {
			proc(ctx).put64(set, ^uint64(0)&^(3<<31))

			return 0
		},
		"sigsuspend": func(ctx uintptr, set uint32) uint32 {
			p := proc(ctx)
			old := p.setMask(p.u64(set))

			for {
				if _, err := p.waitFds(nil, -1, false); err != nil {
					break
				}
			}

			p.handleSignals()
			p.setMask(old)

			return p.result(0, syscall.EINTR)
		},
		"raise": func(ctx uintptr, sig uint32) uint32 {
			p := proc(ctx)

			if !validSignal(sig) {
				return p.result(0, syscall.EINVAL)
			}

			p.sendSelf(int(sig))

			return p.result(0, nil)
		},
		"kill": func(ctx uintptr, pid, sig uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.kill(int32(pid), sig))
		},
		"killpg": func(ctx uintptr, pgrp, sig uint32) uint32 {
			p := proc(ctx)

			if int32(pgrp) <= 1 {
				return p.result(0, syscall.EINVAL)
			}

			return p.result(0, p.kill(-int32(pgrp), sig))
		},
		"abort": func(ctx uintptr) {
			p := proc(ctx)
			abrt := int(syscall.SIGABRT)
			p.setMask(p.mask() &^ sigBit(abrt))
			p.sendSelf(abrt)
			p.deliver()
			p.setAction(abrt, sigAction{})
			p.exitWith(abrt)
		},
	}
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
