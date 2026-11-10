//go:build linux && amd64

package main

import (
	"sync"
	"unsafe"
)

// The addresses of the stubs of stubs_amd64.s, for the GOT and for the
// calls of jump.go.

func morestackStub() uintptr

func interruptStub() uintptr

func landStub() uintptr

func framesStub() uintptr

func resumeStub() uintptr

func resumeReturnStub() uintptr

func memmoveStub() uintptr

func memsetStub() uintptr

// The native memory functions of stubs_amd64.s need AVX2; without it the
// code's w2o.memcpy, w2o.memmove and w2o.memset are Go's.

func cpuHasAVX2() bool

//go:noescape
func copyMemory(dst, src unsafe.Pointer, n uintptr)

var useAVX2 = cpuHasAVX2()

// memmoveTarget is Go's memmove, which w2oMemmove leaves backward and
// huge copies to.
var memmoveTarget uintptr

var memmoveFound sync.Once

func findMemmove() {
	memmoveFound.Do(func() { memmoveTarget = goSymbol("runtime.memmove") })
}

// moveMemory copies src to the start of dst, which holds it, through
// w2oMemmove when there is AVX2: the copy the code's memcpy makes.
func moveMemory(dst, src []byte) {
	if len(src) > len(dst) {
		throwFmt("a copy of %d bytes into %d", len(src), len(dst))
	}

	if len(src) == 0 {
		return
	}

	if !useAVX2 {
		copy(dst, src)

		return
	}

	findMemmove()
	copyMemory(unsafe.Pointer(&dst[0]), unsafe.Pointer(&src[0]), uintptr(len(src)))
}
