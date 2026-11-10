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

// An unrolled loop addresses its bytes as sums of a base and a constant,
// as clang does for wasm: in the guarded copy each sum is addressed from
// its base, which an access of the same block proves within bounds.
func TestUnrolledSums(t *testing.T) {
	const opI32LtU = 0x49
	inc := cat([]byte{0x2d, 0, 0}, i32c(1), []byte{opI32Add, 0x3a, 0, 0})
	body := cat(get(0), get(3), []byte{opI32Add, opLocalTee, 4}, get(1), get(3), []byte{opI32Add, opLocalTee, 5}, inc)

	for k := int32(1); k < 4; k++ {
		body = cat(body, get(4), i32c(k), []byte{opI32Add}, get(5), i32c(k), []byte{opI32Add}, inc)
	}

	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: []ValType{I32, I32, I32}, locals: []ValType{I32, I32, I32}, export: "copy",
			code: cat([]byte{opLoop, blockEmpty}, body, get(3), i32c(4), []byte{opI32Add, opLocalTee, 3},
				get(2), []byte{opI32LtU, opBrIf, 0, 0x0b})},
	}}
	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		fn, _ := i.export("copy")
		copyBytes := fn.Interface().(func(uintptr, uint32, uint32, uint32))
		fill := func() []byte {
			for j := range i.memory.bytes() {
				i.memory.bytes()[j] = byte(j*37 + 11)
			}

			return append([]byte(nil), i.memory.bytes()...)
		}
		add := func(want []byte, dst, n uint32) {
			for j := uint32(0); j < n && dst+j < pageSize; j++ {
				want[dst+j] = want[2048+j] + 1
			}
		}

		for _, n := range []uint32{4, 16, 64, 256} {
			for _, dst := range []uint32{1024, 1025, 3000, pageSize - 256} {
				want := fill()
				add(want, dst, n)
				copyBytes(i.ctx(), dst, 2048, n)

				if !bytes.Equal(i.memory.bytes(), want) {
					t.Fatalf("tail %t: copy(%d, %d) wrote different bytes", options.memoryTail, dst, n)
				}
			}
		}

		// Past the end the guarded copy falls back: the bytes within the
		// memory are written before the trap.
		want := fill()
		add(want, pageSize-8, 64)

		if code := trapOf(func() { copyBytes(i.ctx(), pageSize-8, 2048, 64) }); code != trapMemoryOOB {
			t.Fatalf("tail %t: copy past the end: trap %d", options.memoryTail, code)
		}

		if !bytes.Equal(i.memory.bytes(), want) {
			t.Fatalf("tail %t: copy past the end wrote different bytes", options.memoryTail)
		}

		i.release()
	}
}

// hitHost counts the calls of countHit.
type hitHost struct {
	hits int
}

func countHit(ctx uintptr) {
	instanceAt(ctx).host.(*hitHost).hits++
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
	host := &hitHost{}
	i := load(t, m, testImports{"env.hit": countHit})
	i.host = host
	fn, _ := i.export("copy")
	copyBytes := fn.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	for j := range i.memory.bytes() {
		i.memory.bytes()[j] = 1
	}

	if got := copyBytes(i.ctx(), 512, 256, 20); got != 0 || host.hits != 0 {
		t.Fatalf("normal exit = %d, callbacks %d", got, host.hits)
	}

	i.memory.bytes()[256] = 42

	if got := copyBytes(i.ctx(), pageSize-1, 256, 20); got != 1 || host.hits != 1 || i.memory.bytes()[pageSize-1] != 42 {
		t.Fatalf("early exit = %d, callbacks %d, stored %d", got, host.hits, i.memory.bytes()[pageSize-1])
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
