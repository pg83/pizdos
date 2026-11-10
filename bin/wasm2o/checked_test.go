//go:build linux && amd64

package main

import (
	"encoding/binary"
	"testing"
)

// A join's parameter that every predecessor accessed is known to reach
// only as far as those accesses did, short of the whole slack its own check
// admits: the sum of it and a constant, accessed within that reach, reaches
// no further, and an access of the sum far past it checks the parameter,
// which is past the size.
func TestJoinedReach(t *testing.T) {
	const opI64Load = 0x29
	// params: x, sel; locals: p 2, q 3
	code := cat(get(0), []byte{opI32Load, 2, 0, opDrop}, get(1), []byte{opIf, blockEmpty},
		get(0), i32c(1000), []byte{opI32Add, opLocalSet, 2}, get(2), []byte{opI64Load, 3, 0, opDrop, opElse},
		get(0), i32c(1001), []byte{opI32Add, opLocalSet, 2}, get(2), []byte{opI64Load, 3, 0, opDrop, 0x0b},
		get(2), []byte{opI32Load, 2, 0, opDrop},
		get(2), i32c(4), []byte{opI32Add, opLocalSet, 3},
		get(3), []byte{opI32Load, 2, 0}, get(3), []byte{opI32Load, 2, 0xe8, 0x07, opI32Add})
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: []ValType{I32, I32}, results: i32, locals: []ValType{I32, I32}, export: "reach", code: code}}}
	i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), CompileOptions{memoryTail: true, tailRanges: true})), nil, 8<<20)
	i.start()
	defer i.release()
	f, _ := i.export("reach")
	reach := f.Interface().(func(uintptr, uint32, uint32) uint32)

	if got := reach(i.ctx(), 100, 1); got != 0 {
		t.Fatalf("reach within the memory: %d", got)
	}

	for _, sel := range []uint32{0, 1} {
		var got uint32

		if trap := trapOf(func() { got = reach(i.ctx(), pageSize-4, sel) }); trap != trapMemoryOOB {
			t.Fatalf("sel %d: an access 1 KiB past a base beyond the size: trap %d, result %d", sel, trap, got)
		}
	}
}

// So does the sum of such a parameter and an index whose range keeps the
// access within the slack: the sum reaches as far as the parameter, less
// the index.
func TestJoinedIndex(t *testing.T) {
	const opI32Load8U, opI32And = 0x2d, 0x71
	// params: x, sel, i; locals: p 3, q 4
	code := cat(get(0), []byte{opI32Load, 2, 0, opDrop}, get(1), []byte{opIf, blockEmpty},
		get(0), i32c(700), []byte{opI32Add, opLocalSet, 3}, get(3), []byte{opI32Load, 2, 0xac, 0x02, opDrop, opElse},
		get(0), i32c(701), []byte{opI32Add, opLocalSet, 3}, get(3), []byte{opI32Load, 2, 0xac, 0x02, opDrop, 0x0b},
		get(3), []byte{opI32Load, 2, 0, opDrop},
		get(3), get(2), i32c(255), []byte{opI32And, opI32Add, opLocalSet, 4},
		get(4), []byte{opI32Load8U, 0, 0}, get(4), []byte{opI32Load8U, 0, 0xbc, 0x05, opI32Add})
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: []ValType{I32, I32, I32}, results: i32, locals: []ValType{I32, I32}, export: "index", code: code}}}
	i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), CompileOptions{memoryTail: true, tailRanges: true})), nil, 8<<20)
	i.start()
	defer i.release()
	f, _ := i.export("index")
	index := f.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	if got := index(i.ctx(), 100, 1, 255); got != 0 {
		t.Fatalf("index within the memory: %d", got)
	}

	for _, sel := range []uint32{0, 1} {
		var got uint32

		if trap := trapOf(func() { got = index(i.ctx(), pageSize-4, sel, 255) }); trap != trapMemoryOOB {
			t.Fatalf("sel %d: an indexed access 1 KiB past a base beyond the size: trap %d, result %d", sel, trap, got)
		}
	}
}

// A sum of a base and a negative constant, accessed from the base, may
// have wrapped around: the access lands below the memory, in its slack,
// while the sum itself is far past the size. Joined, the sum is not
// checked by that access.
func TestJoinedWrap(t *testing.T) {
	// params: x, sel; locals: p 2
	code := cat(get(0), []byte{opI32Load, 2, 0, opDrop}, get(1), []byte{opIf, blockEmpty},
		get(0), i32c(-8), []byte{opI32Add, opLocalSet, 2}, get(2), []byte{opI32Load, 2, 0, opDrop, opElse},
		get(0), i32c(-16), []byte{opI32Add, opLocalSet, 2}, get(2), []byte{opI32Load, 2, 0, opDrop, 0x0b},
		get(2), []byte{opI32Load, 2, 0})
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: []ValType{I32, I32}, results: i32, locals: []ValType{I32}, export: "wrap", code: code}}}
	i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), CompileOptions{memoryTail: true, tailRanges: true})), nil, 8<<20)
	i.start()
	defer i.release()
	f, _ := i.export("wrap")
	wrap := f.Interface().(func(uintptr, uint32, uint32) uint32)

	if got := wrap(i.ctx(), 2048, 1); got != 0 {
		t.Fatalf("wrap within the memory: %d", got)
	}

	for _, sel := range []uint32{0, 1} {
		var got uint32

		if trap := trapOf(func() { got = wrap(i.ctx(), 4, sel) }); trap != trapMemoryOOB {
			t.Fatalf("sel %d: an access at a sum that wrapped around: trap %d, result %d", sel, trap, got)
		}
	}
}

// A check carries through a join as far as every predecessor's accesses
// reach: through the base of a sum of a constant and the joined value,
// through an argument that adds a constant to a value checked further on,
// and around a loop whose back edge passes the value on untouched. Each
// function checks only the values new to it.
func TestJoinedFacts(t *testing.T) {
	const opI32And, opI32LtS = 0x71, 0x48
	// base(sel), local p 1: either branch loads a pointer and stores past it;
	// the join reads the pointer.
	base := cat(get(0), []byte{opIf, blockEmpty},
		i32c(256), []byte{opI32Load, 2, 0, opLocalSet, 1}, get(1), i32c(16), []byte{opI32Add}, i32c(1), []byte{opI32Store, 2, 0, opElse},
		i32c(260), []byte{opI32Load, 2, 0, opLocalSet, 1}, get(1), i32c(8), []byte{opI32Add}, i32c(2), []byte{opI32Store, 2, 0, 0x0b},
		get(1), []byte{opI32Load, 2, 0})
	// derived(sel), locals p 1, q 2: a pointer checked 16 bytes past it is
	// passed on 4 or 8 past it, and the join reads 8 bytes there.
	derived := cat(i32c(256), []byte{opI32Load, 2, 0, opLocalSet, 1}, get(1), i32c(12), []byte{opI32Add, opI32Load, 2, 0, opDrop},
		get(0), []byte{opIf, blockEmpty}, get(1), i32c(4), []byte{opI32Add, opLocalSet, 2, opElse}, get(1), i32c(8), []byte{opI32Add, opLocalSet, 2, 0x0b},
		get(2), []byte{opI32Load, 2, 0}, get(2), []byte{opI32Load, 2, 4, opI32Add})
	// cycle(p, n), locals i 2, acc 3: odd iterations read past p and move it
	// to the pointer it holds, checked anew; even ones leave it.
	cycle := cat(get(0), i32c(4), []byte{opI32Add, opI32Load, 2, 0, opDrop, opLoop, blockEmpty},
		get(2), i32c(1), []byte{opI32And, opIf, blockEmpty},
		get(3), get(0), i32c(4), []byte{opI32Add, opI32Load, 2, 0, opI32Add, opLocalSet, 3},
		get(0), []byte{opI32Load, 2, 0, opLocalSet, 0}, get(0), i32c(4), []byte{opI32Add, opI32Load, 2, 0, opDrop, 0x0b},
		get(2), i32c(1), []byte{opI32Add, opLocalTee, 2}, get(1), []byte{opI32LtS, opBrIf, 0, 0x0b},
		get(3))
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: i32, results: i32, locals: []ValType{I32}, export: "base", code: base},
		{params: i32, results: i32, locals: []ValType{I32, I32}, export: "derived", code: derived},
		{params: []ValType{I32, I32}, results: i32, locals: []ValType{I32, I32}, export: "cycle", code: cycle},
	}}
	i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), CompileOptions{memoryTail: true, tailRanges: true, tailProfile: true})), nil, 8<<20)
	i.start()
	defer i.release()
	mem := i.memory.bytes()
	binary.LittleEndian.PutUint32(mem[256:], 1024)
	binary.LittleEndian.PutUint32(mem[260:], 2048)
	binary.LittleEndian.PutUint32(mem[1028:], 3)
	binary.LittleEndian.PutUint32(mem[1032:], 5)
	binary.LittleEndian.PutUint32(mem[1036:], 9)
	binary.LittleEndian.PutUint32(mem[4096:], 4096)
	binary.LittleEndian.PutUint32(mem[4100:], 7)
	fn := func(name string) any {
		f, _ := i.export(name)
		return f.Interface()
	}
	baseFn := fn("base").(func(uintptr, uint32) uint32)
	derivedFn := fn("derived").(func(uintptr, uint32) uint32)
	cycleFn := fn("cycle").(func(uintptr, uint32, uint32) uint32)

	if a, b := baseFn(i.ctx(), 1), baseFn(i.ctx(), 0); a != 0 || b != 0 || binary.LittleEndian.Uint32(mem[1040:]) != 1 || binary.LittleEndian.Uint32(mem[2056:]) != 2 {
		t.Fatalf("base: %d %d", a, b)
	}

	if a, b := derivedFn(i.ctx(), 1), derivedFn(i.ctx(), 0); a != 3+5 || b != 5+9 {
		t.Fatalf("derived: %d %d", a, b)
	}

	if got := cycleFn(i.ctx(), 4096, 8); got != 4*7 {
		t.Fatalf("cycle: %d", got)
	}

	checks := map[int]uint64{}

	for _, r := range i.tailProfile().Functions {
		checks[r.Function] = r.Checks
	}

	// base and derived check their pointer once a call, cycle its first
	// pointer and each pointer it moves to.
	if checks[0] != 2 || checks[1] != 2 || checks[2] != 5 {
		t.Fatalf("checks: base %d, derived %d, cycle %d", checks[0], checks[1], checks[2])
	}
}
