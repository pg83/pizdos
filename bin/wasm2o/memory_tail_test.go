//go:build linux && amd64

package main

import (
	"encoding/binary"
	"math"
	"testing"
)

func loadMemoryTail(t *testing.T, m *testModule, optimized bool) *Instance {
	t.Helper()
	obj := readObject(compileModuleWithOptions(m.encode(), CompileOptions{memoryTail: true, tailRanges: optimized}))
	i := instantiate(t.Name(), obj, testImports{}, 8<<20)
	i.start()
	t.Cleanup(i.release)

	return i
}

func memoryTailModule() *testModule {
	return &testModule{memMin: 1, memMax: 3, funcs: []testFunc{
		{results: i32, export: "size", code: []byte{0x3f, 0}},
		{params: i32, results: i32, export: "grow", code: cat(get(0), []byte{opMemoryGrow, 0})},
		{params: i32, results: i32, export: "load", code: cat(get(0), []byte{opI32Load, 2, 0})},
		{params: []ValType{I32, I32}, export: "store", code: cat(get(0), get(1), []byte{opI32Store, 2, 0})},
		{params: []ValType{I32, I32, I32}, export: "fill", code: cat(get(0), get(1), get(2), []byte{0xfc, 11, 0})},
		{params: []ValType{I32, I32, I32}, export: "copy", code: cat(get(0), get(1), get(2), []byte{0xfc, 10, 0, 0})},
		{params: []ValType{I32, I32}, results: i32, export: "chain", code: cat(
			get(0), get(1), []byte{opI32Store, 2, 0},
			get(0), get(1), []byte{opI32Store, 2, 8},
			get(0), []byte{opI32Load, 2, 16})},
		{params: []ValType{I32, I32, I32}, results: i32, locals: i32, export: "mixed", code: cat(
			get(0), get(2), []byte{opI32Store, 2, 0},
			get(0), []byte{opI32Load, 2, 4, opLocalSet, 3},
			get(1), get(2), []byte{opI32Store, 2, 0},
			get(1), []byte{opI32Load, 2, 4}, get(3), []byte{opI32Add})},
		{params: []ValType{I64, I32}, results: i32, export: "wide", code: cat(
			get(0), []byte{0xa7}, get(1), []byte{opI32Store, 2, 0},
			get(0), i64c(8), []byte{opI64Add, 0xa7}, get(1), []byte{opI32Store, 2, 0},
			get(0), i64c(16), []byte{opI64Add, 0xa7, opI32Load, 2, 0})},
	}}
}

func TestMemoryTailDefault(t *testing.T) {
	t.Setenv("W2X_MEMORY_TAIL", "")
	t.Setenv("W2X_TAIL_PROFILE", "1")
	obj := readObject(compileModule(memoryTailModule().encode()))
	i := instantiate(t.Name(), obj, testImports{}, 8<<20)
	i.start()
	t.Cleanup(i.release)
	f, _ := i.export("chain")
	chain := f.Interface().(func(uintptr, uint32, uint32) uint32)

	if obj.meta.memory.tail != boundSlack || i.memory.pages != 1 || len(i.memoryBytes()) != 2*pageSize {
		t.Fatal("default compilation did not enable the accessible tail")
	}

	if got := chain(i.ctx(), pageSize-4, 37); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[pageSize+4:]) != 37 {
		t.Fatal("default compilation did not permit the tail access")
	}

	for _, r := range i.tailProfile().Functions {
		if r.Function == 6 {
			if r.Guards != 1 || r.Omitted != 3 || r.Checks != 0 || r.Fallbacks != 0 {
				t.Fatalf("default compilation did not optimize the range: %+v", r)
			}

			return
		}
	}

	t.Fatal("default compilation did not produce range guards")
}

func TestMemoryTail(t *testing.T) {
	for _, optimized := range []bool{false, true} {
		i := loadMemoryTail(t, memoryTailModule(), optimized)
		f, _ := i.export("load")
		read := f.Interface().(func(uintptr, uint32) uint32)
		f, _ = i.export("store")
		store := f.Interface().(func(uintptr, uint32, uint32))
		f, _ = i.export("size")
		size := f.Interface().(func(uintptr) uint32)
		f, _ = i.export("grow")
		grow := f.Interface().(func(uintptr, uint32) uint32)
		f, _ = i.export("fill")
		fill := f.Interface().(func(uintptr, uint32, uint32, uint32))
		f, _ = i.export("copy")
		copyBytes := f.Interface().(func(uintptr, uint32, uint32, uint32))

		if size(i.ctx()) != 1 || len(i.memory.bytes()) != pageSize || len(i.memoryBytes()) != 2*pageSize {
			t.Fatalf("optimized %t: logical and accessible sizes disagree", optimized)
		}

		for _, p := range []uint32{pageSize - 2, pageSize, 2*pageSize - 4} {
			store(i.ctx(), p, 0x12345678)

			if got := read(i.ctx(), p); got != 0x12345678 {
				t.Fatalf("optimized %t, address %d: %x", optimized, p, got)
			}
		}

		before := binary.LittleEndian.Uint32(i.memoryBytes()[2*pageSize-4:])

		if trap := trapOf(func() { store(i.ctx(), 2*pageSize-2, 99) }); trap != trapMemoryOOB {
			t.Fatalf("store across physical end: trap %d", trap)
		}

		if got := binary.LittleEndian.Uint32(i.memoryBytes()[2*pageSize-4:]); got != before {
			t.Fatal("trapping store changed its in-range prefix")
		}

		fill(i.ctx(), pageSize, 0x7b, pageSize)
		copyBytes(i.ctx(), pageSize+128, pageSize, 32)

		if grow(i.ctx(), 0) != 1 || read(i.ctx(), pageSize) != 0x7b7b7b7b {
			t.Fatal("zero growth changed the tail")
		}

		if grow(i.ctx(), 3) != math.MaxUint32 || read(i.ctx(), pageSize) != 0x7b7b7b7b {
			t.Fatal("failed growth changed the tail")
		}

		if grow(i.ctx(), 1) != 1 || size(i.ctx()) != 2 {
			t.Fatal("growth changed logical page accounting")
		}

		for j, value := range i.memory.bytes()[pageSize:] {
			if value != 0 {
				t.Fatalf("old tail was not zeroed at %d", j)
			}
		}

		store(i.ctx(), 3*pageSize-4, 81)

		if read(i.ctx(), 3*pageSize-4) != 81 || grow(i.ctx(), 1) != 2 || size(i.ctx()) != 3 {
			t.Fatal("tail did not move with growth")
		}

		if read(i.ctx(), 3*pageSize-4) != 0 {
			t.Fatal("second growth exposed a dirty tail")
		}

		store(i.ctx(), 4*pageSize-4, 83)

		if read(i.ctx(), 4*pageSize-4) != 83 || grow(i.ctx(), 1) != math.MaxUint32 {
			t.Fatal("maximum memory did not retain its tail")
		}
	}
}

func TestMemoryTailRanges(t *testing.T) {
	i := loadMemoryTail(t, memoryTailModule(), true)
	f, _ := i.export("chain")
	chain := f.Interface().(func(uintptr, uint32, uint32) uint32)

	if got := chain(i.ctx(), pageSize-4, 27); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[pageSize+4:]) != 27 {
		t.Fatal("fast region did not write across the logical end")
	}

	p := uint32(2*pageSize - 12)

	if trap := trapOf(func() { chain(i.ctx(), p, 33) }); trap != trapMemoryOOB {
		t.Fatalf("cold region: trap %d", trap)
	}

	if binary.LittleEndian.Uint32(i.memoryBytes()[p:]) != 33 || binary.LittleEndian.Uint32(i.memoryBytes()[p+8:]) != 33 {
		t.Fatal("cold region lost stores preceding a later trap")
	}

	f, _ = i.export("mixed")
	mixed := f.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	for _, q := range []uint32{pageSize - 4, pageSize + 32} {
		clear(i.memoryBytes()[256:264])
		clear(i.memoryBytes()[q : q+8])

		if got := mixed(i.ctx(), 256, q, 55); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[q:]) != 55 {
			t.Fatal("transition from a fast region to a cold region changed values")
		}
	}

	f, _ = i.export("wide")
	wide := f.Interface().(func(uintptr, uint64, uint32) uint32)
	clear(i.memoryBytes()[pageSize : pageSize+32])

	if got := wide(i.ctx(), 0xabcdef0100010000, 59); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[pageSize+8:]) != 59 {
		t.Fatal("i64 address normalization changed wrapped addresses")
	}

}

func TestMemoryTailClone(t *testing.T) {
	i := loadMemoryTail(t, memoryTailModule(), true)
	i.memoryBytes()[pageSize+27] = 91
	c := i.clone(testImports{}, 8<<20, 0, pageSize)
	t.Cleanup(c.release)

	if c.memoryBytes()[pageSize+27] != 91 {
		t.Fatal("clone lost the accessible tail")
	}

	if c.memory.grow(1) != 1 || c.memoryBytes()[pageSize+27] != 0 || i.memoryBytes()[pageSize+27] != 91 {
		t.Fatal("growth did not zero only the child's old tail")
	}
}

func TestMemoryTailFullAddressSpace(t *testing.T) {
	for _, optimized := range []bool{false, true} {
		m := memoryTailModule()
		m.memMin, m.memMax = 65536, 65536
		m.funcs = append(m.funcs, testFunc{params: []ValType{I32, I32}, results: i32, export: "wrapped", code: cat(
			get(0), get(1), []byte{opI32Store, 2, 0},
			get(0), i32c(8), []byte{opI32Add}, get(1), []byte{opI32Store, 2, 0},
			get(0), i32c(16), []byte{opI32Add, opI32Load, 2, 0})})
		i := loadMemoryTail(t, m, optimized)
		f, _ := i.export("chain")
		chain := f.Interface().(func(uintptr, uint32, uint32) uint32)
		end := uint64(1) << 32

		if got := chain(i.ctx(), math.MaxUint32-3, 71); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[end+4:]) != 71 {
			t.Fatal("static offsets did not reach the tail above 4 GiB")
		}

		f, _ = i.export("wrapped")
		wrapped := f.Interface().(func(uintptr, uint32, uint32) uint32)

		if got := wrapped(i.ctx(), math.MaxUint32-3, 73); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[4:]) != 73 || binary.LittleEndian.Uint32(i.memoryBytes()[end+4:]) != 71 {
			t.Fatal("i32 arithmetic stopped wrapping at 4 GiB")
		}
	}
}

func TestMemoryTailProfile(t *testing.T) {
	m := memoryTailModule()
	obj := readObject(compileModuleWithOptions(m.encode(), CompileOptions{memoryTail: true, tailRanges: true, tailProfile: true}))
	i := instantiate(t.Name(), obj, testImports{}, 8<<20)
	i.start()
	t.Cleanup(i.release)
	f, _ := i.export("chain")
	chain := f.Interface().(func(uintptr, uint32, uint32) uint32)
	chain(i.ctx(), 256, 1)
	trapOf(func() { chain(i.ctx(), 2*pageSize-12, 2) })
	profile := i.tailProfile()

	for _, r := range profile.Functions {
		if r.Function == 6 {
			if r.Guards != 2 || r.Fallbacks != 1 || r.Omitted != 3 || r.Checks != 3 {
				t.Fatalf("unexpected exact counters: %+v", r)
			}

			return
		}
	}

	t.Fatal("missing chain profile")
}

func TestMemoryTailSharedContinuation(t *testing.T) {
	m := memoryTailModule()
	m.funcs = append(m.funcs, testFunc{params: []ValType{I32, I32}, results: i32, locals: i32, export: "islands", code: cat(
		get(0), get(1), []byte{opI32Store, 2, 0},
		get(0), get(1), []byte{opI32Store, 2, 8},
		get(0), []byte{opI32Load, 2, 16, opLocalSet, 2},
		call(0), get(2), []byte{opI32Add, opLocalSet, 2},
		get(0), get(1), []byte{opI32Store, 2, 24},
		get(0), []byte{opI32Load, 2, 32}, get(2), []byte{opI32Add})})
	i := loadMemoryTail(t, m, true)
	f, _ := i.export("islands")
	islands := f.Interface().(func(uintptr, uint32, uint32) uint32)

	for _, p := range []uint32{256, pageSize + 32} {
		if got := islands(i.ctx(), p, 19); got != 1 {
			t.Fatalf("shared continuation at %d: %d", p, got)
		}
	}

}
