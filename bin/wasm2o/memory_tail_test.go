//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"slices"
	"testing"
)

// A load whose only use reads it as an operand is folded into the use:
// it keeps its trap ahead of a division's, reads memory before a store
// between them, compares at its width and, branched on, at the width of a
// narrow load.
func TestFoldedLoads(t *testing.T) {
	const (
		i32LoadByte = 0x2d
		i32DivS     = 0x6d
		i32LtS      = 0x48
	)
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: []ValType{I32, I32, I32}, results: i32, export: "loadDivide", code: cat(get(0), []byte{opI32Load, 2, 0}, get(1), get(2), []byte{i32DivS, opI32Add})},
		{params: []ValType{I32, I32}, results: i32, export: "loadStore", code: cat(get(0), []byte{opI32Load, 2, 0}, get(0), get(1), []byte{opI32Store, 2, 0}, get(1), []byte{opI32Add})},
		{params: i32, results: i32, export: "less", code: cat(get(0), []byte{opI32Load, 2, 0}, i32c(10), []byte{i32LtS})},
		{params: i32, results: i32, export: "branchByte", code: cat(get(0), []byte{i32LoadByte, 0, 0, opIf, byte(I32)}, i32c(1), []byte{opElse}, i32c(2), []byte{0x0b})},
		{params: []ValType{I32, I32}, results: i32, export: "sum", code: cat(get(1), get(0), []byte{opI32Load, 2, 0, opI32Add})},
	}}

	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		mem := i.memory.bytes()
		fn := func(name string) any {
			f, _ := i.export(name)
			return f.Interface()
		}
		loadDivide := fn("loadDivide").(func(uintptr, uint32, uint32, uint32) uint32)
		loadStore := fn("loadStore").(func(uintptr, uint32, uint32) uint32)
		less := fn("less").(func(uintptr, uint32) uint32)
		branchByte := fn("branchByte").(func(uintptr, uint32) uint32)
		sum := fn("sum").(func(uintptr, uint32, uint32) uint32)

		if trap := trapOf(func() { loadDivide(i.ctx(), 2*pageSize, 1, 0) }); trap != trapMemoryOOB {
			t.Fatalf("tail %t: a load out of bounds before a division by zero: trap %d", options.memoryTail, trap)
		}

		binary.LittleEndian.PutUint32(mem[2048:], 5)

		if got := loadStore(i.ctx(), 2048, 7); got != 12 || binary.LittleEndian.Uint32(mem[2048:]) != 7 {
			t.Fatalf("tail %t: load before a store: %d", options.memoryTail, got)
		}

		for _, c := range []struct{ value, want uint32 }{{9, 1}, {10, 0}, {0xffffffff, 1}, {11, 0}} {
			binary.LittleEndian.PutUint32(mem[2052:], c.value)

			if got := less(i.ctx(), 2052); got != c.want {
				t.Fatalf("tail %t: %d < 10 is %d", options.memoryTail, int32(c.value), got)
			}
		}

		binary.LittleEndian.PutUint32(mem[2056:], 0x0100)

		if got := branchByte(i.ctx(), 2056); got != 2 {
			t.Fatalf("tail %t: branch on a zero byte with a nonzero byte after it: %d", options.memoryTail, got)
		}

		mem[2056] = 3

		if got := branchByte(i.ctx(), 2056); got != 1 {
			t.Fatalf("tail %t: branch on a nonzero byte: %d", options.memoryTail, got)
		}

		binary.LittleEndian.PutUint32(mem[2060:], 40)

		if got := sum(i.ctx(), 2060, 2); got != 42 {
			t.Fatalf("tail %t: sum with a loaded operand: %d", options.memoryTail, got)
		}

		i.release()
	}
}

// A store of an update of what a load read from the same address updates
// memory in place: in the load's block, in the block after a branch on the
// loaded value, out of bounds trapping, and not across another store.
func TestUpdatedLoads(t *testing.T) {
	const i32LtS = 0x48
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: []ValType{I32, I32}, export: "add", code: cat(get(0), get(0), []byte{opI32Load, 2, 0}, get(1), []byte{opI32Add, opI32Store, 2, 0})},
		{params: i32, locals: i32, results: i32, export: "bounded", code: cat(
			get(0), []byte{opI32Load, 2, 0, opLocalTee, 1}, i32c(100), []byte{i32LtS, opIf, byte(I32)},
			get(0), get(1), i32c(1), []byte{opI32Add, opI32Store, 2, 0}, i32c(1), []byte{opElse}, i32c(0), []byte{0x0b})},
		{params: []ValType{I32, I32}, export: "between", code: cat(get(0), get(0), []byte{opI32Load, 2, 0}, get(1), i32c(9), []byte{opI32Store, 2, 0}, i32c(1), []byte{opI32Add, opI32Store, 2, 0})},
		{params: i32, locals: i32, results: i32, export: "live", code: cat(
			get(0), get(0), []byte{opI32Load, 2, 0}, i32c(-1), []byte{opI32Add, opLocalTee, 1, opI32Store, 2, 0},
			get(1), []byte{opIf, byte(I32)}, i32c(1), []byte{opElse}, i32c(0), []byte{0x0b})},
		{params: i32, locals: i32, results: i32, export: "dead", code: cat(
			get(0), get(0), []byte{opI32Load, 2, 0}, i32c(-1), []byte{opI32Add, opLocalTee, 1, opI32Store, 2, 0},
			get(1), []byte{opI32Eqz, opIf, byte(I32)}, i32c(1), []byte{opElse}, i32c(0), []byte{0x0b})},
	}}

	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		mem := i.memory.bytes()
		fn := func(name string) any {
			f, _ := i.export(name)
			return f.Interface()
		}
		add := fn("add").(func(uintptr, uint32, uint32))
		bounded := fn("bounded").(func(uintptr, uint32) uint32)
		between := fn("between").(func(uintptr, uint32, uint32))
		live := fn("live").(func(uintptr, uint32) uint32)
		dead := fn("dead").(func(uintptr, uint32) uint32)
		binary.LittleEndian.PutUint32(mem[3000:], 40)
		add(i.ctx(), 3000, 2)

		if got := binary.LittleEndian.Uint32(mem[3000:]); got != 42 {
			t.Fatalf("tail %t: update in place: %d", options.memoryTail, got)
		}

		if trap := trapOf(func() { add(i.ctx(), 2*pageSize, 1) }); trap != trapMemoryOOB {
			t.Fatalf("tail %t: update out of bounds: trap %d", options.memoryTail, trap)
		}

		binary.LittleEndian.PutUint32(mem[3004:], 99)

		if bounded(i.ctx(), 3004) != 1 || binary.LittleEndian.Uint32(mem[3004:]) != 100 || bounded(i.ctx(), 3004) != 0 || binary.LittleEndian.Uint32(mem[3004:]) != 100 {
			t.Fatalf("tail %t: update after a branch: %d", options.memoryTail, binary.LittleEndian.Uint32(mem[3004:]))
		}

		binary.LittleEndian.PutUint32(mem[3008:], 5)
		between(i.ctx(), 3008, 3008)

		if got := binary.LittleEndian.Uint32(mem[3008:]); got != 6 {
			t.Fatalf("tail %t: update across a store to the same address: %d", options.memoryTail, got)
		}

		// The branch on the decremented value takes the update's flags.
		binary.LittleEndian.PutUint32(mem[3012:], 2)
		binary.LittleEndian.PutUint32(mem[3016:], 2)

		if live(i.ctx(), 3012) != 1 || live(i.ctx(), 3012) != 0 || dead(i.ctx(), 3016) != 0 || dead(i.ctx(), 3016) != 1 || binary.LittleEndian.Uint32(mem[3012:]) != 0 || binary.LittleEndian.Uint32(mem[3016:]) != 0 {
			t.Fatalf("tail %t: branch on an update: %d %d", options.memoryTail, binary.LittleEndian.Uint32(mem[3012:]), binary.LittleEndian.Uint32(mem[3016:]))
		}

		if trap := trapOf(func() { live(i.ctx(), 2*pageSize) }); trap != trapMemoryOOB {
			t.Fatalf("tail %t: branch on an update out of bounds: trap %d", options.memoryTail, trap)
		}

		i.release()
	}
}

// A load shifted right by whole bytes is a load of its high bytes, unless
// memory changes between the two.
func TestNarrowedShifts(t *testing.T) {
	const opI32Load16U, opI32ShrU = 0x2f, 0x76
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: i32, results: i32, export: "high", code: cat(get(0), []byte{opI32Load16U, 1, 0}, i32c(8), []byte{opI32ShrU})},
		{params: i32, results: i32, export: "top", code: cat(get(0), []byte{opI32Load, 2, 4}, i32c(24), []byte{opI32ShrU})},
		{params: i32, results: i32, export: "half", code: cat(get(0), []byte{opI32Load, 2, 0}, i32c(16), []byte{opI32ShrU})},
		{params: i32, results: i32, locals: i32, export: "both", code: cat(get(0), []byte{opI32Load16U, 1, 0, opLocalTee, 1}, i32c(255), []byte{0x71}, get(1), i32c(8), []byte{opI32ShrU, opI32Add})},
		{params: i32, results: i32, locals: i32, export: "stored", code: cat(get(0), []byte{opI32Load16U, 1, 0, opLocalSet, 1}, get(0), i32c(0x7700), []byte{0x3b, 1, 0}, get(1), i32c(8), []byte{opI32ShrU})},
	}}

	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		mem := i.memory.bytes()
		fn := func(name string) func(uintptr, uint32) uint32 {
			f, _ := i.export(name)
			return f.Interface().(func(uintptr, uint32) uint32)
		}
		high, top, half, both, stored := fn("high"), fn("top"), fn("half"), fn("both"), fn("stored")
		binary.LittleEndian.PutUint32(mem[2000:], 0x89abcdef)
		binary.LittleEndian.PutUint32(mem[2004:], 0x12345678)
		binary.LittleEndian.PutUint16(mem[pageSize-2:], 0xa5c3)

		if high(i.ctx(), 2000) != 0xcd || top(i.ctx(), 2000) != 0x12 || half(i.ctx(), 2000) != 0x89ab || both(i.ctx(), 2000) != 0xef+0xcd || high(i.ctx(), pageSize-2) != 0xa5 {
			t.Fatalf("tail %t: high bytes %#x %#x %#x %#x", options.memoryTail, high(i.ctx(), 2000), top(i.ctx(), 2000), half(i.ctx(), 2000), both(i.ctx(), 2000))
		}

		if got := stored(i.ctx(), 2000); got != 0xcd || binary.LittleEndian.Uint16(mem[2000:]) != 0x7700 {
			t.Fatalf("tail %t: high byte across a store: %#x", options.memoryTail, got)
		}

		if !options.memoryTail {
			if trap := trapOf(func() { high(i.ctx(), pageSize-1) }); trap != trapMemoryOOB {
				t.Fatalf("strict: high byte past the end: trap %d", trap)
			}
		}

		if trap := trapOf(func() { high(i.ctx(), pageSize+4) }); trap != trapMemoryOOB {
			t.Fatalf("tail %t: high byte of a load past the end: trap %d", options.memoryTail, trap)
		}

		i.release()
	}
}

// A table indexed by a byte from a base the code does not know is checked
// at the base: the index's range keeps every entry within boundSlack of it.
func TestByteIndexedTable(t *testing.T) {
	const opI32And, opI32Shl = 0x71, 0x74
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: []ValType{I32, I32}, results: i32, export: "entry", code: cat(
			get(0), get(1), i32c(255), []byte{opI32And}, i32c(2), []byte{opI32Shl, opI32Add, opI32Load, 2, 0},
			get(0), get(1), i32c(3), []byte{opI32Add}, i32c(255), []byte{opI32And}, i32c(2), []byte{opI32Shl, opI32Add, opI32Load, 2, 0, opI32Add})},
	}}

	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		mem := i.memory.bytes()
		f, _ := i.export("entry")
		entry := f.Interface().(func(uintptr, uint32, uint32) uint32)

		for k := uint32(0); k < 256; k++ {
			binary.LittleEndian.PutUint32(mem[4096+4*k:], k*k)
		}

		for _, x := range []uint32{0, 1, 200, 252, 253, 255, 256, 1000, ^uint32(0)} {
			a, b := x&255, (x+3)&255

			if got := entry(i.ctx(), 4096, x); got != a*a+b*b {
				t.Fatalf("tail %t: entries %d and %d: %d", options.memoryTail, a, b, got)
			}
		}

		if trap := trapOf(func() { entry(i.ctx(), pageSize+8, 0) }); trap != trapMemoryOOB {
			t.Fatalf("tail %t: table past the end: trap %d", options.memoryTail, trap)
		}

		i.release()
	}
}

// An address that is the product of two loaded values is not proven in
// bounds: the bounds of the factors' product do not fit in 64 bits.
func TestRangeProduct(t *testing.T) {
	const opI32Mul = 0x6c
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: i32, results: i32, export: "product", code: cat(get(0), []byte{opI32Load, 2, 0}, get(0), []byte{opI32Load, 2, 4, opI32Mul, opI32Load, 2, 0})},
	}}

	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		mem := i.memory.bytes()
		f, _ := i.export("product")
		product := f.Interface().(func(uintptr, uint32) uint32)
		binary.LittleEndian.PutUint32(mem[100:], 3)
		binary.LittleEndian.PutUint32(mem[104:], 1000)
		binary.LittleEndian.PutUint32(mem[3000:], 77)

		if got := product(i.ctx(), 100); got != 77 {
			t.Fatalf("tail %t: load at a product in bounds: %d", options.memoryTail, got)
		}

		binary.LittleEndian.PutUint32(mem[104:], pageSize/3+10)

		if trap := trapOf(func() { product(i.ctx(), 100) }); trap != trapMemoryOOB {
			t.Fatalf("tail %t: load at a product past the end: trap %d", options.memoryTail, trap)
		}

		i.release()
	}
}

// An access at a negative constant offset from a base is covered by the
// base's check and addressed from the base: with the tail it reaches the
// slack before the memory where the sum wrapped, and traps only for a base
// past the end; strict compilation traps on the wrapped sum.
func TestMemoryTailBefore(t *testing.T) {
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: i32, results: i32, export: "before", code: cat(get(0), i32c(-8), []byte{opI32Add, opI32Load, 2, 0})},
		{params: []ValType{I32, I32}, export: "storeBefore", code: cat(get(0), i32c(-8), []byte{opI32Add}, get(1), []byte{opI32Store, 2, 0})},
		{params: i32, results: i32, export: "both", code: cat(
			get(0), []byte{opI32Load, 2, 4},
			get(0), i32c(-4), []byte{opI32Add, opI32Load, 2, 0, opI32Add})},
	}}

	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		mem := i.memory.bytes()
		f, _ := i.export("before")
		before := f.Interface().(func(uintptr, uint32) uint32)
		f, _ = i.export("storeBefore")
		storeBefore := f.Interface().(func(uintptr, uint32, uint32))
		f, _ = i.export("both")
		both := f.Interface().(func(uintptr, uint32) uint32)
		binary.LittleEndian.PutUint32(mem[100:], 7)
		binary.LittleEndian.PutUint32(mem[104:], 9)
		binary.LittleEndian.PutUint32(mem[108:], 11)

		if before(i.ctx(), 108) != 7 || both(i.ctx(), 104) != 18 {
			t.Fatalf("tail %t: wrong values at negative offsets", options.memoryTail)
		}

		for _, p := range []uint32{pageSize + 8, pageSize + boundSlack - 4} {
			if trap := trapOf(func() { before(i.ctx(), p) }); trap != trapMemoryOOB {
				t.Fatalf("tail %t: base %d past the end: trap %d", options.memoryTail, p, trap)
			}
		}

		if options.memoryTail {
			if before(i.ctx(), 4) != 0 {
				t.Fatal("the slack before the memory was not zero")
			}

			storeBefore(i.ctx(), 4, 0x5a5a5a5a)

			if before(i.ctx(), 4) != 0x5a5a5a5a || !bytes.Equal(mem[:8], make([]byte, 8)) {
				t.Fatal("a store before the memory reached the memory or was lost")
			}
		} else {
			if trap := trapOf(func() { before(i.ctx(), 4) }); trap != trapMemoryOOB {
				t.Fatalf("strict wrapped load: trap %d", trap)
			}

			if trap := trapOf(func() { storeBefore(i.ctx(), 4, 1) }); trap != trapMemoryOOB || !bytes.Equal(mem[:8], make([]byte, 8)) {
				t.Fatalf("strict wrapped store: trap %d, memory %x", trap, mem[:8])
			}
		}

		i.release()
	}
}

// A constant length of memory.copy or memory.fill compiles to loads and
// stores: every length up to the limit, overlapping either way, and a range
// crossing the logical end, which strict compilation refuses before any
// store and the tail admits as it admits a store.
func TestConstantCopy(t *testing.T) {
	const lengths = 32
	m := &testModule{memMin: 1, memMax: 1}

	for n := 1; n <= lengths; n++ {
		m.funcs = append(m.funcs,
			testFunc{params: []ValType{I32, I32}, export: fmt.Sprintf("copy%d", n), code: cat(get(0), get(1), i32c(int32(n)), []byte{0xfc, 10, 0, 0})},
			testFunc{params: i32, export: fmt.Sprintf("fill%d", n), code: cat(get(0), i32c(0xa5), i32c(int32(n)), []byte{0xfc, 11, 0})})
	}

	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		mem := i.memory.bytes()

		for n := 1; n <= lengths; n++ {
			f, _ := i.export(fmt.Sprintf("copy%d", n))
			copyBytes := f.Interface().(func(uintptr, uint32, uint32))
			f, _ = i.export(fmt.Sprintf("fill%d", n))
			fill := f.Interface().(func(uintptr, uint32))

			for _, pair := range [][2]uint32{{1000, 100}, {104, 100}, {100, 104}, {100, 100}} {
				for j := range 256 {
					mem[100+j] = byte(j + n)
				}

				want := slices.Clone(mem[:4096])
				copy(want[pair[0]:pair[0]+uint32(n)], want[pair[1]:pair[1]+uint32(n)])
				copyBytes(i.ctx(), pair[0], pair[1])

				if !bytes.Equal(mem[:4096], want) {
					t.Fatalf("tail %t: copy of %d bytes from %d to %d", options.memoryTail, n, pair[1], pair[0])
				}
			}

			fill(i.ctx(), 2000)

			if mem[1999] != 0 || mem[2000+n] != 0 || !bytes.Equal(mem[2000:2000+n], bytes.Repeat([]byte{0xa5}, n)) {
				t.Fatalf("tail %t: fill of %d bytes", options.memoryTail, n)
			}
		}

		f, _ := i.export("copy8")
		copy8 := f.Interface().(func(uintptr, uint32, uint32))
		f, _ = i.export("fill8")
		fill8 := f.Interface().(func(uintptr, uint32))
		source := []byte{1, 2, 3, 4, 5, 6, 7, 8}
		copy(mem, source)

		for _, p := range []uint32{pageSize + 1, pageSize + boundSlack - 4} {
			if trap := trapOf(func() { copy8(i.ctx(), p, 0) }); trap != trapMemoryOOB {
				t.Fatalf("tail %t: copy to %d past the end: trap %d", options.memoryTail, p, trap)
			}

			if trap := trapOf(func() { copy8(i.ctx(), 0, p) }); trap != trapMemoryOOB {
				t.Fatalf("tail %t: copy from %d past the end: trap %d", options.memoryTail, p, trap)
			}

			if trap := trapOf(func() { fill8(i.ctx(), p) }); trap != trapMemoryOOB {
				t.Fatalf("tail %t: fill at %d past the end: trap %d", options.memoryTail, p, trap)
			}
		}

		crossing := uint32(pageSize - 4)
		copy(mem[crossing:], []byte{0xee, 0xee, 0xee, 0xee})

		if options.memoryTail {
			copy8(i.ctx(), crossing, 0)

			if !bytes.Equal(i.memoryBytes()[crossing:crossing+8], source) {
				t.Fatal("copy across the logical end did not reach the tail")
			}

			copy8(i.ctx(), 16, crossing)

			if !bytes.Equal(mem[16:24], source) {
				t.Fatal("copy from across the logical end did not read the tail")
			}

			fill8(i.ctx(), crossing)

			if !bytes.Equal(i.memoryBytes()[crossing:crossing+8], bytes.Repeat([]byte{0xa5}, 8)) {
				t.Fatal("fill across the logical end did not reach the tail")
			}
		} else {
			if trap := trapOf(func() { copy8(i.ctx(), crossing, 0) }); trap != trapMemoryOOB || !bytes.Equal(mem[crossing:], []byte{0xee, 0xee, 0xee, 0xee}) {
				t.Fatalf("strict copy across the end: trap %d, memory %x", trap, mem[crossing:])
			}

			if trap := trapOf(func() { copy8(i.ctx(), 16, crossing) }); trap != trapMemoryOOB || !bytes.Equal(mem[16:24], make([]byte, 8)) {
				t.Fatalf("strict copy from across the end: trap %d, memory %x", trap, mem[16:24])
			}

			if trap := trapOf(func() { fill8(i.ctx(), crossing) }); trap != trapMemoryOOB || !bytes.Equal(mem[crossing:], []byte{0xee, 0xee, 0xee, 0xee}) {
				t.Fatalf("strict fill across the end: trap %d, memory %x", trap, mem[crossing:])
			}
		}

		i.release()
	}
}

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

	if obj.meta.memory.tail != boundSlack || i.memory.pages != 1 || len(i.memoryBytes()) != pageSize+boundSlack {
		t.Fatal("default compilation did not enable the accessible tail")
	}

	if got := chain(i.ctx(), pageSize-4, 37); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[pageSize+4:]) != 37 {
		t.Fatal("default compilation did not permit the tail access")
	}

	for _, r := range i.tailProfile().Functions {
		if r.Function == 6 {
			if r.Checks != 1 || r.Guards != 0 || r.Omitted != 0 || r.Fallbacks != 0 {
				t.Fatalf("one check of the base did not cover its chain of accesses: %+v", r)
			}

			return
		}
	}

	t.Fatal("default compilation did not count the chain's check")
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

		if size(i.ctx()) != 1 || len(i.memory.bytes()) != pageSize || len(i.memoryBytes()) != pageSize+boundSlack {
			t.Fatalf("optimized %t: logical and accessible sizes disagree", optimized)
		}

		for _, p := range []uint32{pageSize - 2, pageSize} {
			store(i.ctx(), p, 0x12345678)

			if got := read(i.ctx(), p); got != 0x12345678 {
				t.Fatalf("optimized %t, address %d: %x", optimized, p, got)
			}
		}

		// The tail extends accesses whose base is within the logical size;
		// a base past it traps without touching memory.
		for _, p := range []uint32{pageSize + 1, pageSize + boundSlack - 4, pageSize + boundSlack - 2} {
			before := string(i.memoryBytes()[pageSize:])

			if trap := trapOf(func() { store(i.ctx(), p, 99) }); trap != trapMemoryOOB {
				t.Fatalf("optimized %t, store based at %d past the logical end: trap %d", optimized, p, trap)
			}

			if trap := trapOf(func() { read(i.ctx(), p) }); trap != trapMemoryOOB {
				t.Fatalf("optimized %t, load based at %d past the logical end: trap %d", optimized, p, trap)
			}

			if string(i.memoryBytes()[pageSize:]) != before {
				t.Fatal("trapping store changed the tail")
			}
		}

		fill(i.ctx(), pageSize, 0x7b, boundSlack)
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

		// A store across the logical end leaves its upper half in the tail,
		// which growth turns into zeroed memory.
		store(i.ctx(), 2*pageSize-2, 0x51515151)

		if read(i.ctx(), 2*pageSize-2) != 0x51515151 || grow(i.ctx(), 1) != 2 || size(i.ctx()) != 3 {
			t.Fatal("tail did not move with growth")
		}

		if read(i.ctx(), 2*pageSize-2) != 0x5151 {
			t.Fatal("second growth exposed a dirty tail")
		}

		store(i.ctx(), 3*pageSize-2, 83<<16)

		if read(i.ctx(), 3*pageSize-2) != 83<<16 || grow(i.ctx(), 1) != math.MaxUint32 {
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

	p := uint32(pageSize + boundSlack - 12)

	if trap := trapOf(func() { chain(i.ctx(), p, 33) }); trap != trapMemoryOOB {
		t.Fatalf("chain based past the logical end: trap %d", trap)
	}

	if binary.LittleEndian.Uint32(i.memoryBytes()[p:]) != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[p+8:]) != 0 {
		t.Fatal("a trapping base stored into the tail")
	}

	f, _ = i.export("mixed")
	mixed := f.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	for _, q := range []uint32{pageSize - 4, pageSize} {
		clear(i.memoryBytes()[256:264])
		clear(i.memoryBytes()[q : q+8])

		if got := mixed(i.ctx(), 256, q, 55); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[q:]) != 55 {
			t.Fatal("a second base changed values")
		}
	}

	clear(i.memoryBytes()[256:264])

	if trap := trapOf(func() { mixed(i.ctx(), 256, pageSize+32, 57) }); trap != trapMemoryOOB {
		t.Fatalf("second base past the logical end: trap %d", trap)
	}

	if binary.LittleEndian.Uint32(i.memoryBytes()[256:]) != 57 {
		t.Fatal("a trap lost the store preceding it")
	}

	f, _ = i.export("wide")
	wide := f.Interface().(func(uintptr, uint64, uint32) uint32)
	clear(i.memoryBytes()[pageSize-16 : pageSize+16])

	if got := wide(i.ctx(), 0xabcdef010000fff0, 59); got != 0 || binary.LittleEndian.Uint32(i.memoryBytes()[pageSize-8:]) != 59 {
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
	f, _ = i.export("wide")
	wide := f.Interface().(func(uintptr, uint64, uint32) uint32)
	wide(i.ctx(), 256, 3)
	trapOf(func() { wide(i.ctx(), 2*pageSize-12, 4) })
	found := 0

	for _, r := range i.tailProfile().Functions {
		switch r.Function {
		case 6:
			// One check of the base covers the chain, and the second call traps there.
			if r.Checks != 2 || r.Guards != 0 || r.Fallbacks != 0 || r.Omitted != 0 {
				t.Fatalf("unexpected chain counters: %+v", r)
			}

			found++
		case 8:
			// Separately computed addresses share a range guard.
			if r.Guards != 2 || r.Fallbacks != 1 || r.Omitted != 3 || r.Checks != 1 {
				t.Fatalf("unexpected range counters: %+v", r)
			}

			found++
		}
	}

	if found != 2 {
		t.Fatal("missing chain or range profile")
	}
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

	for _, p := range []uint32{256, pageSize - 16} {
		if got := islands(i.ctx(), p, 19); got != 1 {
			t.Fatalf("shared continuation at %d: %d", p, got)
		}
	}
}
