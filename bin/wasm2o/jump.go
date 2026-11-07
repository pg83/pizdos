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
// Jump; longjmp returns from that call again through the land stub, which
// checks that the frame is still on the stack. The Go frames between the
// stub's caller and the frame are dropped, not unwound: they must hold no
// defers and no locks.

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

	if j.frame[jumpPC] < uint64(i.code.base) || pc >= uint64(len(i.obj.text)) {
		return
	}

	k := sort.Search(len(regions), func(k int) bool { return uint64(regions[k].end) > pc })

	if k == len(regions) || uint64(regions[k].start) > pc {
		return
	}

	j.code = [2]uint64{uint64(i.code.base) + uint64(regions[k].start), uint64(i.code.base) + uint64(regions[k].end)}
	land := nativeFunc(reflect.TypeFor[func(uintptr, uint32, uintptr) uint32](), i.symbol(symLand))
	land.Interface().(func(uintptr, uint32, uintptr) uint32)(uintptr(unsafe.Pointer(j)), val, i.ctx())
	runtime.KeepAlive(j)
}
