// Interruption of native code that runs on a goroutine, for vm's
// kernel (bin/vm): the goroutine names the code and a stub, and when
// another goroutine asks, the next preemption signal that finds it at an
// asynchronous safe point in that code injects a call of the stub there,
// as one of asyncPreempt would be. The stub saves what the code needs and
// may run Go: the code keeps neither stack addresses nor Go pointers in
// its registers, so its frames move with the stack.

package runtime

import (
	"internal/runtime/atomic"
	"unsafe"
)

type gInterrupt struct {
	lo, hi, stub uintptr
	wanted       atomic.Bool
}

// InstanceOffset is the offset in g of the instance of vm's code the
// goroutine runs, which the code reads after a call of Go; the entry
// thunks set it.
func InstanceOffset() uintptr {
	return unsafe.Offsetof(g{}.instance)
}

// SetInterrupt makes the calling goroutine interruptible in the code
// [lo, hi) by a call of stub, or not with stub 0, and returns its token
// for Interrupt.
func SetInterrupt(lo, hi, stub uintptr) uintptr {
	gp := getg()
	gp.interrupt.wanted.Store(false)
	gp.interrupt.lo, gp.interrupt.hi, gp.interrupt.stub = lo, hi, stub

	return uintptr(unsafe.Pointer(gp))
}

// Interrupt asks the goroutine of a token, which has not given its
// interruption up, for one: its thread gets the preemption signal when it
// runs.
func Interrupt(token uintptr) {
	gp := (*g)(unsafe.Pointer(token))
	gp.interrupt.wanted.Store(true)

	if mp := gp.m; mp != nil && preemptMSupported && debug.asyncpreemptoff == 0 {
		preemptM(mp)
	}
}

// interruptG injects the interruption a preemption signal finds wanted
// when the goroutine is at a safe point in its code; it reports whether it
// did, and asyncPreempt waits for the next signal.
//
//go:nosplit
//go:nowritebarrierrec
func interruptG(gp *g, ctxt *sigctxt) bool {
	in := &gp.interrupt
	pc := ctxt.sigpc()

	if in.stub == 0 || !in.wanted.Load() || pc < in.lo || pc >= in.hi {
		return false
	}

	ok, resume := isAsyncSafePoint(gp, pc, ctxt.sigsp(), ctxt.siglr())

	if !ok {
		return false
	}

	in.wanted.Store(false)
	ctxt.pushCall(in.stub, resume)

	return true
}
