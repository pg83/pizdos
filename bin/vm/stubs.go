//go:build linux && amd64

package main

import "unsafe"

// The addresses of the stubs of stubs_amd64.s, for the GOT and for the
// calls of jump.go.

func morestackStub() uintptr

func interruptStub() uintptr

func landStub() uintptr

func framesStub() uintptr

func resumeStub() uintptr

func resumeReturnStub() uintptr

// moveMemory copies src to the start of dst, which holds it, through
// lib/str's memmove: the copy the code's memcpy makes.
func moveMemory(dst, src []byte) {
	if len(src) > len(dst) {
		throwFmt("a copy of %d bytes into %d", len(src), len(dst))
	}

	if len(src) == 0 {
		return
	}

	strMove(strFunction(symStr+"memmove"), unsafe.Pointer(&dst[0]), unsafe.Pointer(&src[0]), uintptr(len(src)))
}

// strCall0 calls fn, a function of lib/str, with no arguments, as the
// module's functions call each other; it returns what fn leaves in AX.
func strCall0(fn uintptr) uintptr

// strCall3 calls fn, a function of lib/str, with a, b and c in AX, CX and
// DX, as strCall0 calls one.
func strCall3(fn, a, b, c uintptr) uintptr

// strMove calls fn, lib/str's memmove, with dst, src and n in AX, CX and
// DX.
//
//go:noescape
func strMove(fn uintptr, dst, src unsafe.Pointer, n uintptr)
