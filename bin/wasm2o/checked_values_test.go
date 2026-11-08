//go:build linux && amd64

package main

import (
	"encoding/binary"
	"testing"
)

func TestCheckedAddressMerges(t *testing.T) {
	branch := func(width byte) []byte {
		return cat(get(0), []byte{opIf, byte(I32)}, get(1), []byte{width, 0, 0, 0x1a}, get(1),
			[]byte{opElse}, get(2), []byte{opI32Load, 2, 0, 0x1a}, get(2), []byte{0x0b, opLocalSet, 3},
			i32c(0), i32c(42), []byte{opI32Store, 2, 0}, get(3), []byte{opI32Load, 2, 0})
	}
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: []ValType{I32, I32, I32}, results: i32, locals: i32, export: "same", code: branch(opI32Load)},
		{params: []ValType{I32, I32, I32}, results: i32, locals: i32, export: "wider", code: branch(0x2d)},
		{params: []ValType{I32, I32}, results: i32, export: "cycle", code: cat(
			get(0), []byte{opI32Load, 2, 0, 0x1a, opLoop, blockEmpty},
			get(0), []byte{opI32Load, 2, 0, 0x1a}, i32c(0), i32c(99), []byte{opI32Store, 2, 0},
			get(0), i32c(pageSize), []byte{opI32Add, opLocalSet, 0},
			get(1), i32c(1), []byte{opI32Sub, opLocalTee, 1, opBrIf, 0, 0x0b}, i32c(0))},
	}}
	module := decodeModule(m.encode())

	for fn, want := range []int{2, 3} {
		f := buildFunction(module, fn)
		optimize(f)
		checkSSA(f)
		mf := lowerFunction(module, newLayout(module), f)
		checks := 0

		for _, b := range mf.blocks {
			for _, in := range b.insts {
				if in.op == mCheck {
					checks++
				}
			}
		}

		if checks != want {
			t.Fatalf("function %d: %d bounds checks, want %d", fn, checks, want)
		}
	}

	i := load(t, m, nil)
	binary.LittleEndian.PutUint32(i.memory.bytes()[256:], 17)
	binary.LittleEndian.PutUint32(i.memory.bytes()[pageSize-4:], 23)
	f, _ := i.export("same")
	same := f.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	for _, flag := range []uint32{0, 1} {
		want := uint32(23)

		if flag != 0 {
			want = 17
		}

		if got := same(i.ctx(), flag, 256, pageSize-4); got != want {
			t.Fatalf("merged address, flag %d: %d, want %d", flag, got, want)
		}
	}

	f, _ = i.export("wider")
	wider := f.Interface().(func(uintptr, uint32, uint32, uint32) uint32)
	binary.LittleEndian.PutUint32(i.memory.bytes(), 0)

	if code := trapOf(func() { wider(i.ctx(), 1, pageSize-1, 256) }); code != trapMemoryOOB {
		t.Fatalf("wider merged access: trap %d", code)
	}

	if got := binary.LittleEndian.Uint32(i.memory.bytes()); got != 42 {
		t.Fatalf("store before wider access trap = %d", got)
	}

	f, _ = i.export("cycle")
	binary.LittleEndian.PutUint32(i.memory.bytes(), 0)

	if code := trapOf(func() { f.Interface().(func(uintptr, uint32, uint32) uint32)(i.ctx(), 256, 2) }); code != trapMemoryOOB {
		t.Fatalf("new address on loop re-entry: trap %d", code)
	}

	if got := binary.LittleEndian.Uint32(i.memory.bytes()); got != 99 {
		t.Fatalf("store before next iteration's trap = %d", got)
	}
}
