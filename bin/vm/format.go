package main

import "fmt"

// Types and constants shared by the ELF metadata and the loader.

type ValType uint8

const (
	I32       ValType = 0x7f
	I64       ValType = 0x7e
	F32       ValType = 0x7d
	F64       ValType = 0x7c
	V128      ValType = 0x7b
	FuncRef   ValType = 0x70
	ExternRef ValType = 0x6f
)

const (
	externFunc   = 0
	externTable  = 1
	externMemory = 2
	externGlobal = 3
)

type FuncType struct {
	params  []ValType
	results []ValType
}

// Trap codes, the argument of the loader's trap.
const (
	trapUnreachable   = 1
	trapMemoryOOB     = 2
	trapIntOverflow   = 3
	trapDivByZero     = 4
	trapConversion    = 5
	trapIndirectType  = 6
	trapUndefinedElem = 7
	trapUninitElem    = 8
	trapStack         = 9
	trapTableOOB      = 10
	trapNullRef       = 11
)

// spEntry records that from pc on the stack pointer is delta bytes below
// its value at the entry of the code region around pc.
type spEntry struct {
	pc    int32
	delta int32
}

func alignTo(x, n int32) int32 {
	return (x + n - 1) &^ (n - 1)
}

func typeName(t ValType) string {
	names := map[ValType]string{I32: "i32", I64: "i64", F32: "f32", F64: "f64", V128: "v128", FuncRef: "funcref", ExternRef: "externref"}

	if name, ok := names[t]; ok {
		return name
	}

	return fmt.Sprintf("t%d", t)
}
