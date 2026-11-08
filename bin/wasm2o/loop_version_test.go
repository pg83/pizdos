//go:build linux && amd64

package main

import (
	"bytes"
	"testing"
)

func TestUnrolledLoopFallback(t *testing.T) {
	stores := func(width int) []byte {
		var code []byte

		for j := 0; j < width; j++ {
			code = append(code, cat(get(0), get(3), []byte{opI32Add}, get(1), get(3),
				[]byte{opI32Add, 0x2d, 0, byte(j), 0x3a, 0, byte(j)})...)
		}

		return code
	}
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: []ValType{I32, I32, I32}, results: i32, locals: []ValType{I32, I32}, export: "triple",
			code: cat([]byte{opLoop, blockEmpty}, stores(3), get(3), i32c(3), []byte{opI32Add, opLocalSet, 3},
				get(4), i32c(3), []byte{opI32Sub, opLocalTee, 4}, get(2), []byte{opI32Add}, i32c(2),
				[]byte{0x4b, opBrIf, 0, 0x0b}, get(3))},
		{params: []ValType{I32, I32, I32}, results: i32, locals: i32, export: "four",
			code: cat([]byte{opLoop, blockEmpty}, stores(4), get(3), i32c(4), []byte{opI32Add, opLocalSet, 3},
				get(2), i32c(4), []byte{opI32Sub, opLocalTee, 2, opBrIf, 0, 0x0b}, get(3))},
	}}
	i := load(t, m, nil)

	for _, name := range []string{"triple", "four"} {
		fn, _ := i.export(name)
		copyBytes := fn.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

		for _, n := range []uint32{4, 12, 16, 24, 256} {
			length := n

			if name == "triple" {
				length = n / 3 * 3
			}

			for _, dst := range []uint32{127, 128, 129, 130, 512} {
				for j := range i.memory.bytes() {
					i.memory.bytes()[j] = byte(j*37 + 11)
				}

				want := append([]byte(nil), i.memory.bytes()...)

				for j := uint32(0); j < length; j++ {
					want[dst+j] = want[128+j]
				}

				if got := copyBytes(i.ctx(), dst, 128, n); got != length || !bytes.Equal(i.memory.bytes(), want) {
					t.Fatalf("%s(%d, %d): copied %d, want %d or different bytes", name, dst, n, got, length)
				}
			}
		}

		// A non-multiple of four never reaches zero. Its guarded copy must
		// fall back, preserving all stores before the eventual bounds trap.
		for j := range i.memory.bytes() {
			i.memory.bytes()[j] = byte(j*37 + 11)
		}

		want := append([]byte(nil), i.memory.bytes()...)

		for j := 0; j < 128; j++ {
			want[pageSize-128+j] = want[128+j]
		}

		count := uint32(17)

		if name == "triple" {
			count = 1
		}

		if code := trapOf(func() { copyBytes(i.ctx(), pageSize-128, 128, count) }); code != trapMemoryOOB {
			t.Fatalf("%s crossing memory end: trap %d", name, code)
		}

		if !bytes.Equal(i.memory.bytes(), want) {
			t.Fatalf("%s changed stores before trap", name)
		}
	}
}

func TestLoopEarlyExit(t *testing.T) {
	var tail []byte

	for j := byte(1); j < 4; j++ {
		tail = append(tail, cat(get(0), get(1), []byte{0x2d, 0, j, 0x3a, 0, j})...)
	}
	m := &testModule{memMin: 1, memMax: 1, imports: []testImport{{"env", "hit", nil, nil}}, funcs: []testFunc{{
		params: []ValType{I32, I32, I32}, results: i32, export: "copy",
		code: cat([]byte{opLoop, blockEmpty}, get(0), get(1), []byte{0x2d, 0, 0, 0x3a, 0, 0},
			get(1), []byte{0x2d, 0, 0}, i32c(42), []byte{0x46, opIf, blockEmpty}, call(0), i32c(1), []byte{0x0f, 0x0b},
			tail, get(0), i32c(4), []byte{opI32Add, opLocalSet, 0}, get(1), i32c(4), []byte{opI32Add, opLocalSet, 1},
			get(2), i32c(1), []byte{opI32Sub, opLocalTee, 2, opBrIf, 0, 0x0b}, i32c(0)),
	}}}
	f := buildFunction(decodeModule(m.encode()), 1)
	optimize(f)
	checkSSA(f)
	proved := 0

	for _, b := range f.rpo() {
		for _, v := range f.blocks[b].instrs {
			if f.values[v].op == OpStore && memKind(f.values[v].aux2)&memProven != 0 {
				proved++
			}
		}
	}

	if proved < 4 {
		t.Fatalf("early-exit loop has %d proved stores", proved)
	}

	hits := 0
	i := load(t, m, testImports{"env.hit": func(uintptr) { hits++ }})
	fn, _ := i.export("copy")
	copyBytes := fn.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	for j := range i.memory.bytes() {
		i.memory.bytes()[j] = 1
	}

	if got := copyBytes(i.ctx(), 512, 256, 20); got != 0 || hits != 0 {
		t.Fatalf("normal exit = %d, callbacks %d", got, hits)
	}

	i.memory.bytes()[256] = 42

	if got := copyBytes(i.ctx(), pageSize-1, 256, 20); got != 1 || hits != 1 || i.memory.bytes()[pageSize-1] != 42 {
		t.Fatalf("early exit = %d, callbacks %d, stored %d", got, hits, i.memory.bytes()[pageSize-1])
	}
}

func TestGuardedAddressDisplacements(t *testing.T) {
	var body []byte

	for _, offset := range []int32{4, 8} {
		body = append(body, cat(get(0), get(3), []byte{opI32Add}, i32c(offset), []byte{opI32Add},
			get(1), get(3), []byte{opI32Add}, i32c(offset), []byte{opI32Add, 0x2d, 0, 0, 0x3a, 0, 0})...)
	}

	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{
		params: []ValType{I32, I32, I32}, results: i32, locals: i32, export: "offsets",
		code: cat([]byte{opLoop, blockEmpty}, body, get(3), i32c(8), []byte{opI32Add, opLocalSet, 3},
			get(2), i32c(1), []byte{opI32Sub, opLocalTee, 2, opBrIf, 0, 0x0b}, get(3)),
	}}}
	f := buildFunction(decodeModule(m.encode()), 0)
	optimize(f)
	checkSSA(f)
	folded := 0

	for _, b := range f.rpo() {
		for _, v := range f.blocks[b].instrs {
			val := f.values[v]

			if (val.op == OpLoad || val.op == OpStore) && memKind(val.aux2)&memProven != 0 && (val.aux == 4 || val.aux == 8) {
				folded++
			}
		}
	}

	if folded != 4 {
		t.Fatalf("guarded accesses with folded displacements = %d, want 4", folded)
	}

	i := load(t, m, nil)
	fn, _ := i.export("offsets")
	offsets := fn.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	// Wrapped addresses are valid wasm accesses too. Their entry guards
	// must select the original arithmetic, rather than a widened address.
	for _, pair := range [][2]uint32{{1024, 128}, {1024, 0xfffffffc}, {0xfffffffc, 1024}, {129, 128}} {
		for j := range i.memory.bytes() {
			i.memory.bytes()[j] = byte(j*37 + 11)
		}

		want := append([]byte(nil), i.memory.bytes()...)

		for j := uint32(0); j < 32; j++ {
			for _, off := range []uint32{4, 8} {
				want[pair[0]+j*8+off] = want[pair[1]+j*8+off]
			}
		}

		if got := offsets(i.ctx(), pair[0], pair[1], 32); got != 256 || !bytes.Equal(i.memory.bytes(), want) {
			t.Fatalf("addresses %08x: result %d or different bytes", pair, got)
		}
	}
}
