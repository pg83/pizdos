//go:build linux && amd64

package main

import (
	"encoding/binary"
	"reflect"
	"runtime"
	"sort"
	"unsafe"
)

// setjmp and longjmp, as an embedder builds them: the import that returns
// twice takes the jump frame its adapter wrote (abi.go, goabi.go) into a
// Jump; longjmp returns from that call again through the land stub
// (stubs_amd64.s), which checks that the frame is still on the stack. The
// Go frames between the stub's caller and the frame are dropped, not
// unwound: they must hold no defers and no locks.

// Jump is a frame to return to: the jump frame as the adapter wrote it,
// then the code of its function, the land stub's to read.
type Jump struct {
	frame [jumpWords]uint64
	code  [2]uint64
}

// setjmp takes the jump frame of the call in progress, the one Go is in;
// false tells that the call came through a table or an export, whose
// adapter writes no frame.
func (i *Instance) setjmp(j *Jump) bool {
	for k := range j.frame {
		j.frame[k] = binary.LittleEndian.Uint64(i.block[offJump+8*k:])
	}

	clear(i.block[offJump : offJump+8*jumpWords])

	return j.frame[jumpPC] != 0
}

// sp is the frame's stack pointer as an offset below the stack's top: a
// frame deeper in the stack has a larger one.
func (j *Jump) sp() uint64 {
	return j.frame[jumpSP]
}

// longjmp returns from the call of j with val; it returns itself only when
// j's frame is not on the stack any more.
func (i *Instance) longjmp(j *Jump, val uint32) {
	regions := i.obj.meta.regions
	pc := j.frame[jumpPC] - uint64(i.code.base)
	// Only direct returns-twice adapters create these private frames. Their
	// saved PC follows a call inside this Code's function region; the guest's
	// jmp_buf never contains or supplies the native frame (kernel_jump.go).
	k := sort.Search(len(regions), func(k int) bool { return uint64(regions[k].end) > pc })
	j.code = [2]uint64{uint64(i.code.base) + uint64(regions[k].start), uint64(i.code.base) + uint64(regions[k].end)}
	land := nativeFunc(reflect.TypeFor[func(uintptr, uint32, uintptr) uint32](), landStub())
	land.Interface().(func(uintptr, uint32, uintptr) uint32)(uintptr(unsafe.Pointer(j)), val, i.ctx())
	runtime.KeepAlive(j)
}

// fork: the frames of a call that returns twice, copied by the frames stub
// from its caller up to the process's entry, run again on another
// goroutine by the resume stub (stubs_amd64.s).

type framesRequest struct {
	ctx, sp, bp          uint64
	funcs, entry, resume [2]uint64
	buf, cap             uint64
	size, top            uint64
}

type resumeRequest struct {
	ctx, buf, size, top, bp, sp, pc uint64
	regs                            [jumpSaved]uint64
}

// Frames are copied frames: top is the offset of the top one's saved RBP.
// The resume stub's request is in the heap with them: the stub grows the
// stack, which a request on it would move under the stub.
type Frames struct {
	bytes []byte
	top   uint64
	jump  Jump
	req   resumeRequest
}

func isFunctionRegion(name string) bool {
	return len(name) > 1 && name[0] == 'f' && name[1] >= '0' && name[1] <= '9'
}

// codeRange is the code of regions first to last.
func (i *Instance) codeRange(first, last int) [2]uint64 {
	regions := i.obj.meta.regions
	base := uint64(i.code.base)

	return [2]uint64{base + uint64(regions[first].start), base + uint64(regions[last].end)}
}

// functionCode is the code of the module's functions, whose regions follow
// one another (compile.go).
func (i *Instance) functionCode() ([2]uint64, bool) {
	first, last := -1, -1

	for k, r := range i.obj.meta.regions {
		switch {
		case !isFunctionRegion(r.name):
		case first < 0:
			first, last = k, k
		case last != k-1:
			return [2]uint64{}, false
		default:
			last = k
		}
	}

	if first < 0 {
		return [2]uint64{}, false
	}

	return i.codeRange(first, last), true
}

// exportCode is the code of an exported function's thunk.
func (i *Instance) exportCode(name string) ([2]uint64, bool) {
	_, e := i.export(name)

	return i.symbolCode(e.sym)
}

// symbolCode is the code of the region a symbol starts.
func (i *Instance) symbolCode(sym string) ([2]uint64, bool) {
	at := i.obj.defined[sym]

	for k, r := range i.obj.meta.regions {
		if r.start == at {
			return i.codeRange(k, k), true
		}
	}

	return [2]uint64{}, false
}

// frames copies the frames of j's call up to the export entry the process
// entered by, or the resume stub a child of fork did; false tells that
// they lead elsewhere, as from a signal handler, through Go.
func (i *Instance) frames(j *Jump, entry string) (*Frames, bool) {
	funcs, ok := i.functionCode()
	thunk, found := i.exportCode(entry)

	if !ok || !found {
		return nil, false
	}

	// The copied frames return into the resume stub's continuation.
	lo, hi := resumeReturnRange()
	resume := [2]uint64{uint64(lo), uint64(hi)}

	buf := make([]byte, 16<<10)
	req := &framesRequest{ctx: uint64(i.ctx()), sp: j.frame[jumpSP], bp: j.frame[jumpBP], funcs: funcs, entry: thunk, resume: resume}
	stub := nativeFunc(reflect.TypeFor[func(uintptr)](), framesStub()).Interface().(func(uintptr))

	for {
		req.buf, req.cap = uint64(uintptr(unsafe.Pointer(unsafe.SliceData(buf)))), uint64(len(buf))
		stub(uintptr(unsafe.Pointer(req)))
		runtime.KeepAlive(buf)

		if req.size == 0 {
			return nil, false
		}

		if req.size <= req.cap {
			return &Frames{bytes: buf[:req.size], top: req.top, jump: *j}, true
		}

		buf = make([]byte, req.size)
	}
}

// resume runs copied frames in i, on this goroutine: their call returns 0.
// It returns when the top frame does, with what it left in RAX.
func (i *Instance) resume(f *Frames) uint64 {
	j := &f.jump.frame
	req := &f.req
	*req = resumeRequest{ctx: uint64(i.ctx()), buf: uint64(uintptr(unsafe.Pointer(unsafe.SliceData(f.bytes)))), size: uint64(len(f.bytes)),
		top: f.top, bp: j[jumpSP] - j[jumpBP], sp: j[jumpSP], pc: j[jumpPC], regs: [jumpSaved]uint64(j[jumpRegs : jumpRegs+jumpSaved])}
	stub := nativeFunc(reflect.TypeFor[func(uintptr) uint64](), resumeStub()).Interface().(func(uintptr) uint64)
	r := stub(uintptr(unsafe.Pointer(req)))
	runtime.KeepAlive(f)

	return r
}
