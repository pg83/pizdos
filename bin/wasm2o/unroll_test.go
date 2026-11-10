//go:build linux && amd64

package main

import (
	"encoding/binary"
	"testing"
)

const (
	opI32Ne  = 0x47
	opI32Mul = 0x6c
	opI32Or  = 0x72
	opI32Shl = 0x74
)

// profiledChecks compiles m with the accessible tail, range versioning and
// per-function counters, calls run with the instance and gives the checks
// each function executed.
func profiledChecks(t *testing.T, m *testModule, run func(i *Instance)) map[int]uint64 {
	i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), CompileOptions{memoryTail: true, tailRanges: true, tailProfile: true})), nil, 8<<20)
	i.start()
	defer i.release()
	run(i)
	checks := map[int]uint64{}

	for _, r := range i.tailProfile().Functions {
		checks[r.Function] = r.Checks
	}

	return checks
}

// A loop whose constants decide that it runs eight times becomes eight
// copies of its body: each lane's addresses are a base and a constant, and
// one check of each base covers them. Loops that run a number of times
// known only at run time, or more than eight, stay loops and compute the
// same.
func TestUnrolledLanes(t *testing.T) {
	// lanes(src, dst, n): for i := 0; i != n; i++ { dst[i] = src[i]*3 + i }
	lanes := func(n []byte) []byte {
		return cat([]byte{opLoop, blockEmpty},
			get(1), get(3), i32c(2), []byte{opI32Shl, opI32Add},
			get(0), get(3), i32c(2), []byte{opI32Shl, opI32Add, opI32Load, 2, 0},
			i32c(3), []byte{opI32Mul}, get(3), []byte{opI32Add, opI32Store, 2, 0},
			get(3), i32c(1), []byte{opI32Add, opLocalTee, 3}, n, []byte{opI32Ne, opBrIf, 0, 0x0b})
	}
	params := []ValType{I32, I32, I32}
	locals := []ValType{I32}
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: params, locals: locals, export: "eight", code: lanes(i32c(8))},
		{params: params, locals: locals, export: "nine", code: lanes(i32c(9))},
		{params: params, locals: locals, export: "some", code: lanes(get(2))},
	}}
	checks := profiledChecks(t, m, func(i *Instance) {
		mem := i.memory.bytes()

		for k := range 16 {
			binary.LittleEndian.PutUint32(mem[1024+4*k:], uint32(100+k))
		}

		for name, n := range map[string]uint32{"eight": 8, "nine": 9, "some": 5} {
			f, _ := i.export(name)
			clear(mem[2048:2112])
			f.Interface().(func(uintptr, uint32, uint32, uint32))(i.ctx(), 1024, 2048, n)

			for k := range uint32(16) {
				want := uint32(0)

				if k < n {
					want = (100+k)*3 + k
				}

				if got := binary.LittleEndian.Uint32(mem[2048+4*k:]); got != want {
					t.Fatalf("%s: lane %d is %d, want %d", name, k, got, want)
				}
			}
		}
	})

	if checks[0] != 2 {
		t.Fatalf("eight lanes: %d checks, want one of each base", checks[0])
	}
}

// An or whose operands have no set bit in common is an add: the address
// it makes is a base and a constant, which shares the check of the base. A
// value's low zero bits come through a loop's parameter only when every
// edge passes it them: from an odd start the or keeps its meaning.
func TestDisjointOrs(t *testing.T) {
	// pair(x, y): load(x<<3 | 4) + load(x<<3) + load(y<<1 | 3)
	pair := cat(get(0), i32c(3), []byte{opI32Shl}, i32c(4), []byte{opI32Or, opI32Load, 2, 0},
		get(0), i32c(3), []byte{opI32Shl, opI32Load, 2, 0, opI32Add},
		get(1), i32c(1), []byte{opI32Shl}, i32c(3), []byte{opI32Or, opI32Load, 2, 0, opI32Add})
	// steps(p, n), locals acc 2, k 3: for k := 0; k != n; k++ { acc += load(p<<2 | 4); p += 2 }
	steps := cat([]byte{opLoop, blockEmpty},
		get(2), get(0), i32c(2), []byte{opI32Shl}, i32c(4), []byte{opI32Or, opI32Load, 2, 0, opI32Add, opLocalSet, 2},
		get(0), i32c(2), []byte{opI32Add, opLocalSet, 0},
		get(3), i32c(1), []byte{opI32Add, opLocalTee, 3}, get(1), []byte{opI32Ne, opBrIf, 0, 0x0b}, get(2))
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: []ValType{I32, I32}, results: i32, export: "pair", code: pair},
		{params: []ValType{I32, I32}, results: i32, locals: []ValType{I32, I32}, export: "steps", code: steps},
	}}
	checks := profiledChecks(t, m, func(i *Instance) {
		mem := i.memory.bytes()

		for k := range 256 {
			binary.LittleEndian.PutUint32(mem[4*k:], uint32(k*k+7))
		}

		word := func(at uint32) uint32 {
			return binary.LittleEndian.Uint32(mem[at:])
		}

		f, _ := i.export("pair")
		pairFn := f.Interface().(func(uintptr, uint32, uint32) uint32)

		if got, want := pairFn(i.ctx(), 200, 401), word(200<<3|4)+word(200<<3)+word(401<<1|3); got != want {
			t.Fatalf("pair = %d, want %d", got, want)
		}

		f, _ = i.export("steps")
		stepsFn := f.Interface().(func(uintptr, uint32, uint32) uint32)

		for _, start := range []uint32{0, 1} {
			want := uint32(0)

			for k, p := 0, start; k < 20; k, p = k+1, p+2 {
				want += word(p<<2 | 4)
			}

			if got := stepsFn(i.ctx(), start, 20); got != want {
				t.Fatalf("steps from %d = %d, want %d", start, got, want)
			}
		}
	})

	// x<<3 | 4 shares the check of x<<3; y<<1 | 3 is checked itself.
	if checks[0] != 2 {
		t.Fatalf("pair: %d checks, want 2", checks[0])
	}
}

// A constant moves out of a sum to its outside, through shifts: the
// element after base[i] is the address of base[i] and 4, which the check
// of that address covers.
func TestOuterConstants(t *testing.T) {
	// next(base, i): base[i] + base[i+1] + base[i+2], 32-bit elements
	next := cat(get(0), get(1), i32c(2), []byte{opI32Shl, opI32Add, opI32Load, 2, 0},
		get(0), get(1), i32c(1), []byte{opI32Add}, i32c(2), []byte{opI32Shl, opI32Add, opI32Load, 2, 0, opI32Add},
		get(0), get(1), i32c(2), []byte{opI32Add}, i32c(2), []byte{opI32Shl, opI32Add, opI32Load, 2, 0, opI32Add})
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: []ValType{I32, I32}, results: i32, export: "next", code: next},
	}}
	checks := profiledChecks(t, m, func(i *Instance) {
		mem := i.memory.bytes()

		for k := range 64 {
			binary.LittleEndian.PutUint32(mem[1024+4*k:], uint32(3*k+1))
		}

		f, _ := i.export("next")

		if got := f.Interface().(func(uintptr, uint32, uint32) uint32)(i.ctx(), 1024, 10); got != 31+34+37 {
			t.Fatalf("next = %d, want %d", got, 31+34+37)
		}
	})

	if checks[0] != 1 {
		t.Fatalf("next: %d checks, want 1", checks[0])
	}
}
