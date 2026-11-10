//go:build linux && amd64

package main

// The addresses of the stubs of stubs_amd64.s, for the GOT and for the
// calls of jump.go.

func morestackStub() uintptr

func interruptStub() uintptr

func landStub() uintptr

func framesStub() uintptr

func resumeStub() uintptr

func resumeReturnStub() uintptr
