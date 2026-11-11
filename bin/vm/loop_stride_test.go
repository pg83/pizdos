//go:build linux && amd64

package main

import (
	"bytes"
	"testing"
)

// Loops advancing a pointer by a stride up to a limit, a limit included,
// or until the pointer equals it: their versions run unchecked when the
// pointer's whole range is in bounds, which the guard decides from the
// limit, the stride and the signed offsets of the accesses, and the
// original loop otherwise, with its traps where they belong.
func TestStridedLimitLoops(t *testing.T) {
	const opI32Ne, opI32LtS, opI32LtU, opI32LeU = 0x47, 0x48, 0x49, 0x4d
	body := func(step []byte, compare byte) []byte {
		return cat([]byte{opLoop, blockEmpty},
			get(0), get(0), get(2), []byte{opI32Add, opI32Load, 2, 0, opI32Store, 2, 0},
			get(4), i32c(1), []byte{opI32Add, opLocalSet, 4},
			get(0), step, []byte{opI32Add, opLocalTee, 0}, get(1), []byte{compare, opBrIf, 0, 0x0b},
			get(4))
	}
	signature := testFunc{params: []ValType{I32, I32, I32, I32}, results: i32, locals: i32}
	define := func(name string, step []byte, compare byte) testFunc {
		fn := signature
		fn.export = name
		fn.code = body(step, compare)

		return fn
	}
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		define("stride", get(3), opI32LtU),
		define("signed", get(3), opI32LtS),
		define("four", i32c(4), opI32LtU),
		define("inclusive", i32c(4), opI32LeU),
		define("exact", i32c(4), opI32Ne),
	}}
	t.Setenv("W2X_TAIL_PROFILE", "1")
	i := load(t, m, nil)

	type run struct {
		start, end, offset, stride uint32
		fast                       bool
	}
	cases := map[string][]run{
		"stride": {
			{0, 4096, 16, 4, true},
			{64, 4096, 0xfffffff0, 4, true},
			{65532, 65536, 0, 4, true},
			{0, 4096, 0xfffffff0, 4, false},
			{pageSize - 64, pageSize - 8, 8, 4, false},
			{100, 50, 0, 0, false},
			{16, 4096, 0, 0xfffffffc, false},
			{0, 4096, 0, 0x80000000, false},
		},
		"signed": {
			{0, 4096, 0, 4, true},
			{0, 0xffffffff, 0, 4, false},
			// The second step takes the pointer past the signed limit's
			// sign bit, where the loop goes on at wrapped addresses.
			{0x7fff0000, 0x7ffffff0, 0x80010000, 0x10000, false},
		},
		"four": {
			{0, 4096, 16, 4, true},
			{pageSize - 64, pageSize - 8, 8, 4, false},
			{4096, 0, 0, 4, false},
		},
		"inclusive": {
			{0, 4092, 0, 4, true},
			{0, pageSize - 4, 0, 4, true},
			{0, 0xffffffff, 0, 4, false},
			{8, 4, 0, 4, false},
		},
		"exact": {
			{0, 4096, 0, 4, true},
			{0, pageSize, 0, 4, true},
			{0, 4098, 0, 4, false},
			{4096, 0, 0, 4, false},
		},
	}
	// model runs the loop with wasm's semantics on a copy of the memory.
	model := func(name string, r run, mem []byte) (count uint32, trap bool) {
		p := r.start
		step := uint32(4)

		if name == "stride" || name == "signed" {
			step = r.stride
		}

		for {
			addr := p + r.offset

			if uint64(addr)+4 > pageSize {
				return count, true
			}

			if uint64(p)+4 > pageSize {
				return count, true
			}

			copy(mem[p:], mem[addr:addr+4])
			count++
			p += step

			switch name {
			case "inclusive":
				if p > r.end {
					return count, false
				}
			case "exact":
				if p == r.end {
					return count, false
				}
			case "signed":
				if int32(p) >= int32(r.end) {
					return count, false
				}
			default:
				if p >= r.end {
					return count, false
				}
			}

			if count > 1<<20 {
				t.Fatalf("%s%v: the model does not terminate", name, r)
			}
		}
	}

	for _, name := range []string{"stride", "signed", "four", "inclusive", "exact"} {
		fn, meta := i.export(name)
		walk := fn.Interface().(func(uintptr, uint32, uint32, uint32, uint32) uint32)

		for _, r := range cases[name] {
			for j := range i.memory.bytes() {
				i.memory.bytes()[j] = byte(j*7 + 3)
			}

			want := append([]byte(nil), i.memory.bytes()[:pageSize]...)
			wantCount, wantTrap := model(name, r, want)
			var got uint32
			code := trapOf(func() { got = walk(i.ctx(), r.start, r.end, r.offset, r.stride) })

			if wantTrap {
				if code != trapMemoryOOB {
					t.Fatalf("%s%v: trap %d, want out of bounds after %d iterations", name, r, code, wantCount)
				}
			} else if code != -1 || got != wantCount {
				t.Fatalf("%s%v: trap %d, count %d, want %d", name, r, code, got, wantCount)
			}

			if !bytes.Equal(i.memory.bytes()[:pageSize], want) {
				t.Fatalf("%s%v: memory differs from the original loop's", name, r)
			}

			checks := uint64(0)

			for _, p := range i.tailProfile().Functions {
				if p.Function == int(meta.index) {
					checks = p.Checks
				}
			}

			if r.fast && checks != 0 {
				t.Fatalf("%s%v: %d checks ran in a loop whose range is in bounds", name, r, checks)
			}
		}
	}
}
