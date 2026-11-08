//go:build linux && amd64

package main

import (
	"encoding/binary"
	"math"
	"reflect"
	"testing"
)

// A successful smaller access, or one in a sibling branch, does not prove
// a later access safe. A store before a later trap must remain observable.
func TestCheckedAddresses(t *testing.T) {
	m := &testModule{
		memMin: 1, memMax: 2,
		funcs: []testFunc{
			{params: i32, results: i32, export: "wider", code: cat(get(0), []byte{0x2d, 0, 0, 0x1a}, get(0), []byte{opI32Load, 2, 0})},
			{params: i32, results: i32, export: "offset", code: cat(get(0), []byte{opI32Load, 2, 0, 0x1a}, get(0), []byte{opI32Load, 2, 4})},
			{params: []ValType{I32, I32}, results: i32, export: "branch", code: cat(get(1), []byte{opIf, blockEmpty}, get(0), []byte{opI32Load, 2, 0, 0x1a, 0x0b}, get(0), []byte{opI32Load, 2, 0})},
			{params: i32, results: i32, export: "store", code: cat(get(0), i32c(19), []byte{opI32Store, 2, 0}, get(0), []byte{opI32Load, 2, 4})},
			{results: i32, export: "grow", code: cat(i32c(pageSize-4), []byte{opI32Load, 2, 0, 0x1a}, i32c(1), []byte{opMemoryGrow, 0, 0x1a}, i32c(pageSize), []byte{opI32Load, 2, 0})},
		},
	}
	i := load(t, m, nil)

	for _, tc := range []struct {
		name string
		addr uint32
	}{{"wider", pageSize - 1}, {"offset", pageSize - 4}, {"store", pageSize - 4}} {
		f, _ := i.export(tc.name)
		fn := f.Interface().(func(uintptr, uint32) uint32)

		if code := trapOf(func() { fn(i.ctx(), tc.addr) }); code != trapMemoryOOB {
			t.Fatalf("%s(%d): trap %d", tc.name, tc.addr, code)
		}
	}

	if got := binary.LittleEndian.Uint32(i.memory.bytes()[pageSize-4:]); got != 19 {
		t.Fatalf("store before trap = %d", got)
	}

	f, _ := i.export("branch")
	branch := f.Interface().(func(uintptr, uint32, uint32) uint32)

	for _, flag := range []uint32{0, 1} {
		if code := trapOf(func() { branch(i.ctx(), pageSize, flag) }); code != trapMemoryOOB {
			t.Fatalf("branch(%d): trap %d", flag, code)
		}
	}

	f, _ = i.export("grow")

	if got := f.Interface().(func(uintptr) uint32)(i.ctx()); got != 0 {
		t.Fatalf("load after grow = %d", got)
	}
}

func TestExpressionsAcrossCall(t *testing.T) {
	m := &testModule{
		memMin: 1, memMax: 1,
		imports: []testImport{{"env", "change", nil, nil}},
		funcs: []testFunc{{
			params: i32, results: i32, export: "sum",
			code: cat(get(0), []byte{opI32Load, 2, 0}, call(0), get(0), []byte{opI32Load, 2, 0, opI32Add}),
		}},
	}
	i := load(t, m, testImports{"env.change": func(ctx uintptr) {
		binary.LittleEndian.PutUint32(instanceAt(ctx).memory.bytes(), 11)
	}})
	binary.LittleEndian.PutUint32(i.memory.bytes(), 7)
	f, _ := i.export("sum")

	if got := f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), 0); got != 18 {
		t.Fatalf("loads across a call = %d", got)
	}
}

// Float selects preserve all bits, including NaN payloads and negative
// zero, when their condition is deferred to the select's flags.
func TestFloatSelectBits(t *testing.T) {
	for _, pair := range [][2]uint32{{0x7fc12345, 0x80000000}, {0x3f000000, 0xbf000000}} {
		m := &testModule{funcs: []testFunc{{
			params: []ValType{F32}, results: i32, export: "choose",
			code: cat(binary.LittleEndian.AppendUint32([]byte{0x43}, pair[0]), binary.LittleEndian.AppendUint32([]byte{0x43}, pair[1]),
				get(0), binary.LittleEndian.AppendUint32([]byte{0x43}, 0), []byte{0x5d, 0x1b, 0xbc}),
		}}}
		i := load(t, m, nil)
		f, _ := i.export("choose")
		choose := f.Interface().(func(uintptr, float32) uint32)

		for _, x := range []float32{-1, 0, 1, float32(math.NaN()), float32(math.Inf(-1)), float32(math.Inf(1))} {
			want := pair[1]

			if x < 0 {
				want = pair[0]
			}

			if got := choose(i.ctx(), x); got != want {
				t.Fatalf("select(%g) = %08x, want %08x", x, got, want)
			}
		}
	}
}

// The conversion result is discarded above the clamp. Below it, a
// dominating positive test excludes both negative inputs and NaN.
func TestSaturatingRanges(t *testing.T) {
	f32c := func(v float32) []byte {
		return binary.LittleEndian.AppendUint32([]byte{0x43}, math.Float32bits(v))
	}
	q := cat(get(0), f32c(257), []byte{0x95})
	m := &testModule{funcs: []testFunc{
		{params: []ValType{F32}, results: i32, export: "clamp", code: cat(
			get(0), f32c(0), []byte{0x5e, opIf, byte(I32)},
			i32c(-1), q, f32c(0.5), []byte{opF32Add, 0xfc, 1}, q, f32c(255), []byte{0x60, 0x1b},
			[]byte{opElse}, i32c(0), []byte{0x0b})},
		{params: []ValType{F32}, results: i32, export: "sat", code: cat(get(0), []byte{0xfc, 1})},
	}}
	i := load(t, m, nil)
	f, _ := i.export("clamp")
	clamp := f.Interface().(func(uintptr, float32) uint32)
	f, _ = i.export("sat")
	sat := f.Interface().(func(uintptr, float32) uint32)
	inputs := []float32{-1, -0.5, 0, math.SmallestNonzeroFloat32, 1, 257, 65406.5, math.Nextafter32(65535, 0), 65535, math.MaxFloat32,
		float32(math.Inf(-1)), float32(math.Inf(1)), math.Float32frombits(0x7fc12345), math.Float32frombits(0xffc12345),
		math.Nextafter32(2147483648, 0), 2147483648, math.Nextafter32(4294967296, 0), 4294967296}

	for _, x := range inputs {
		want := uint32(0)

		if x > 0 {
			q := x / 257
			want = math.MaxUint32

			if q < 255 {
				want = uint32(q + 0.5)
			}
		}

		if got := clamp(i.ctx(), x); got != want {
			t.Fatalf("clamp(%g) = %08x, want %08x", x, got, want)
		}

		want = 0

		if x > 0 {
			want = math.MaxUint32

			if x < 4294967296 {
				want = uint32(x)
			}
		}

		if got := sat(i.ctx(), x); got != want {
			t.Fatalf("sat(%g) = %08x, want %08x", x, got, want)
		}
	}
}

func TestFloatComparisonSelects(t *testing.T) {
	for _, input := range []ValType{F32, F64} {
		for _, output := range []ValType{F32, F64} {
			for pred := 0; pred < 6; pred++ {
				result, reinterpret, cmp := I32, byte(0xbc), byte(0x5b+pred)
				trueBits, falseBits := uint64(0x7fc12345), uint64(0x80000000)
				constant := func(bits uint64) []byte {
					if output == F32 {
						return binary.LittleEndian.AppendUint32([]byte{0x43}, uint32(bits))
					}

					return binary.LittleEndian.AppendUint64([]byte{0x44}, bits)
				}
				zero := binary.LittleEndian.AppendUint32([]byte{0x43}, 0)

				if input == F64 {
					zero = binary.LittleEndian.AppendUint64([]byte{0x44}, 0)
					cmp = byte(0x61 + pred)
				}

				if output == F64 {
					result, reinterpret = I64, 0xbd
					trueBits, falseBits = 0x7ff8123456789abc, 0x8000000000000000
				}

				m := &testModule{funcs: []testFunc{
					{params: []ValType{input}, results: []ValType{result}, export: "select", code: cat(constant(trueBits), constant(falseBits), get(0), zero, []byte{cmp, 0x1b, reinterpret})},
					{params: []ValType{input}, results: i32, export: "branch", code: cat(get(0), zero, []byte{cmp, opIf, byte(I32)}, i32c(17), []byte{opElse}, i32c(9), []byte{0x0b})},
					{params: []ValType{input}, results: i32, export: "integer", code: cat(i32c(17), i32c(9), get(0), zero, []byte{cmp, 0x1b})},
				}}
				i := load(t, m, nil)
				f, _ := i.export("select")
				branch, _ := i.export("branch")
				integer, _ := i.export("integer")

				for _, x := range []float64{-1, math.Copysign(0, -1), 0, 1, math.Inf(-1), math.Inf(1), math.NaN()} {
					condition := []bool{x == 0, x != 0, x < 0, x > 0, x <= 0, x >= 0}[pred]
					want := falseBits

					if condition {
						want = trueBits
					}

					arg := reflect.ValueOf(x)

					if input == F32 {
						arg = reflect.ValueOf(float32(x))
					}

					if got := f.Call([]reflect.Value{reflect.ValueOf(i.ctx()), arg})[0].Uint(); got != want {
						t.Fatalf("select %s -> %s predicate %d (%g) = %x, want %x", typeName(input), typeName(output), pred, x, got, want)
					}

					branchWant := uint64(9)

					if condition {
						branchWant = 17
					}

					if got := branch.Call([]reflect.Value{reflect.ValueOf(i.ctx()), arg})[0].Uint(); got != branchWant {
						t.Fatalf("branch %s predicate %d (%g) = %d, want %d", typeName(input), pred, x, got, branchWant)
					}

					if got := integer.Call([]reflect.Value{reflect.ValueOf(i.ctx()), arg})[0].Uint(); got != branchWant {
						t.Fatalf("integer select %s predicate %d (%g) = %d, want %d", typeName(input), pred, x, got, branchWant)
					}
				}
			}
		}
	}
}

func TestRoundingCorrection(t *testing.T) {
	f32c := func(v float32) []byte {
		return binary.LittleEndian.AppendUint32([]byte{0x43}, math.Float32bits(v))
	}
	negative := cat(get(0), f32c(0), []byte{0x5d})
	m := &testModule{funcs: []testFunc{{params: []ValType{F32}, results: i32, locals: i32, export: "round",
		code: cat(get(0), f32c(-0.5), f32c(0.5), negative, []byte{0x1b, opF32Add, 0xfc, 0, opLocalSet, 1},
			get(1), i32c(1), []byte{0x71, opIf, blockEmpty},
			get(1), []byte{0xb2}, get(0), []byte{0x93, 0x8b}, f32c(0.5), []byte{0x5b, opIf, blockEmpty},
			get(1), i32c(1), i32c(-1), negative, []byte{0x1b, opI32Add, opLocalSet, 1, 0x0b, 0x0b}, get(1))}}}
	f := buildFunction(decodeModule(m.encode()), 0)
	optimize(f)
	checkSSA(f)
	i := load(t, m, nil)
	fn, _ := i.export("round")
	round := fn.Interface().(func(uintptr, float32) uint32)
	inputs := []float32{float32(math.NaN()), float32(math.Inf(1)), float32(math.Inf(-1)), math.MaxFloat32,
		-math.MaxFloat32, 2147483648, -2147483648, math.SmallestNonzeroFloat32, -math.SmallestNonzeroFloat32, float32(math.Copysign(0, -1))}

	for j := -4096; j <= 4096; j++ {
		inputs = append(inputs, float32(j)/4)
	}

	for _, x := range inputs {
		want := int32(0)

		if !math.IsNaN(float64(x)) {
			y := math.RoundToEven(float64(x))
			want = math.MaxInt32

			if y < math.MinInt32 {
				want = math.MinInt32
			} else if y <= math.MaxInt32 {
				want = int32(y)
			}
		}

		if got := int32(round(i.ctx(), x)); got != want {
			t.Fatalf("round(%g) = %d, want %d", x, got, want)
		}
	}
}

func TestMaskedAddresses(t *testing.T) {
	address := func(base int32) []byte {
		return cat(get(0), i32c(255), []byte{0x71}, i32c(2), []byte{0x74}, i32c(base), []byte{opI32Add, opI32Load, 2, 0})
	}
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{params: i32, results: i32, export: "table", code: address(pageSize - 1024)},
		{params: i32, results: i32, export: "past", code: address(pageSize - 1023)},
		{params: i32, results: i32, export: "wrap", code: cat(get(0), i32c(1), []byte{opI32Add, opI32Load, 2, 0})},
	}}
	i := load(t, m, nil)
	binary.LittleEndian.PutUint32(i.memory.bytes()[pageSize-4:], 42)
	binary.LittleEndian.PutUint32(i.memory.bytes(), 19)
	f, _ := i.export("table")
	table := f.Interface().(func(uintptr, uint32) uint32)

	for _, x := range []uint32{255, 65535, math.MaxUint32} {
		if got := table(i.ctx(), x); got != 42 {
			t.Fatalf("table(%d) = %d", x, got)
		}
	}

	f, _ = i.export("past")

	if code := trapOf(func() { f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), 255) }); code != trapMemoryOOB {
		t.Fatalf("table crossing memory end: trap %d", code)
	}

	f, _ = i.export("wrap")

	if got := f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), math.MaxUint32); got != 19 {
		t.Fatalf("load after wrapping address = %d", got)
	}
}

func TestGuardedLoopFallback(t *testing.T) {
	var stores []byte

	for offset := byte(0); offset < 16; offset += 4 {
		stores = append(stores, cat(get(0), i32c(int32(offset/4+1)), []byte{opI32Store, 2, offset})...)
	}

	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: []ValType{I32, I32}, results: i32, export: "fill",
		code: cat([]byte{opLoop, blockEmpty}, stores, get(0), i32c(16), []byte{opI32Add, opLocalSet, 0},
			get(1), i32c(1), []byte{opI32Sub, opLocalTee, 1, opBrIf, 0, 0x0b}, get(0))}}}
	f := buildFunction(decodeModule(m.encode()), 0)
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

	if proved != 4 {
		t.Fatalf("guarded loop has %d proved stores", proved)
	}

	i := load(t, m, nil)
	fn, _ := i.export("fill")
	fill := fn.Interface().(func(uintptr, uint32, uint32) uint32)

	if got := fill(i.ctx(), 0, 16); got != 256 {
		t.Fatalf("pointer after fast loop = %d", got)
	}

	for _, tc := range []struct {
		start, count uint32
		want         [4]uint32
	}{{pageSize - 16, 2, [4]uint32{1, 2, 3, 4}}, {pageSize - 12, 1, [4]uint32{0, 1, 2, 3}}, {0, 0, [4]uint32{1, 2, 3, 4}}} {
		clear(i.memory.bytes()[pageSize-16:])

		if code := trapOf(func() { fill(i.ctx(), tc.start, tc.count) }); code != trapMemoryOOB {
			t.Fatalf("fill(%d, %d): trap %d", tc.start, tc.count, code)
		}

		for j := uint32(0); j < 4; j++ {
			if got := binary.LittleEndian.Uint32(i.memory.bytes()[pageSize-16+4*j:]); got != tc.want[j] {
				t.Fatalf("fill(%d, %d): store %d before trapping iteration = %d, want %d", tc.start, tc.count, j, got, tc.want[j])
			}
		}
	}
}

// A store can change a metadata field before the next use of that field.
// The disjoint-range guard must then select the original loop, while a
// separate output array may use the cached field in the fast loop.
func TestLoopMetadataAlias(t *testing.T) {
	var stores []byte

	for offset := byte(0); offset < 4; offset++ {
		value := int32(0)

		if offset == 0 {
			value = 4
		}

		stores = append(stores, cat(get(0), i32c(value), []byte{0x3a, 0, offset})...)
	}

	body := cat([]byte{opLoop, blockEmpty}, stores, get(0), i32c(0), []byte{opI32Load, 2, 0, opI32Add, opLocalSet, 0},
		get(1), i32c(1), []byte{opI32Sub, opLocalTee, 1, opBrIf, 0, 0x0b})

	for _, bypass := range []bool{false, true} {
		code := body

		if bypass {
			code = cat(get(1), []byte{opIf, blockEmpty}, body, []byte{0x0b})
		}

		m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: []ValType{I32, I32}, results: i32, export: "fill", code: cat(code, get(0))}}}
		f := buildFunction(decodeModule(m.encode()), 0)
		optimize(f)
		checkSSA(f)
		i := load(t, m, nil)
		fn, _ := i.export("fill")
		fill := fn.Interface().(func(uintptr, uint32, uint32) uint32)
		binary.LittleEndian.PutUint32(i.memory.bytes(), 12)

		if got := fill(i.ctx(), 256, 16); got != 448 {
			t.Fatalf("non-aliasing loop, bypass %t: pointer %d", bypass, got)
		}

		if got := fill(i.ctx(), 0, 16); got != 64 {
			t.Fatalf("aliasing loop, bypass %t: pointer %d", bypass, got)
		}

		if bypass {
			if got := fill(i.ctx(), 19, 0); got != 19 {
				t.Fatalf("empty loop: pointer %d", got)
			}
		}
	}
}

func TestLoopMetadataTrapOrder(t *testing.T) {
	var stores []byte

	for offset := byte(0); offset < 16; offset += 4 {
		stores = append(stores, cat(get(0), i32c(int32(offset/4+1)), []byte{opI32Store, 2, offset})...)
	}

	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: []ValType{I32, I32, I32}, results: i32, export: "fill",
		code: cat([]byte{opLoop, blockEmpty}, stores, get(0), get(2), []byte{opI32Load, 2, 0, opI32Add, opLocalSet, 0},
			get(1), i32c(1), []byte{opI32Sub, opLocalTee, 1, opBrIf, 0, 0x0b}, get(0))}}}
	i := load(t, m, nil)
	f, _ := i.export("fill")
	fill := f.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	for _, count := range []uint32{0, 16} {
		clear(i.memory.bytes()[:16])

		if code := trapOf(func() { fill(i.ctx(), 0, count, pageSize) }); code != trapMemoryOOB {
			t.Fatalf("metadata load: trap %d", code)
		}

		for j := uint32(0); j < 4; j++ {
			if got := binary.LittleEndian.Uint32(i.memory.bytes()[4*j:]); got != j+1 {
				t.Fatalf("count %d: store %d before metadata trap = %d", count, j, got)
			}
		}
	}
}

// The upper limit is read after the stores. An overlapping output must
// observe the changed limit; a bad limit pointer must trap after the stores.
func TestAscendingLoop(t *testing.T) {
	var stores []byte

	for offset := byte(0); offset < 16; offset += 4 {
		stores = append(stores, cat(get(0), i32c(int32(offset/4+1)), []byte{opI32Store, 2, offset})...)
	}

	for _, signed := range []bool{false, true} {
		less := byte(0x49)

		if signed {
			less = 0x48
		}

		m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: []ValType{I32, I32, I32}, results: i32, export: "fill",
			code: cat([]byte{opLoop, blockEmpty}, stores, get(0), i32c(16), []byte{opI32Add, opLocalSet, 0},
				get(1), i32c(1), []byte{opI32Add, opLocalTee, 1}, get(2), []byte{opI32Load, 2, 0, less, opBrIf, 0, 0x0b}, get(0))}}}
		f := buildFunction(decodeModule(m.encode()), 0)
		optimize(f)
		checkSSA(f)
		i := load(t, m, nil)
		fn, _ := i.export("fill")
		fill := fn.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

		for _, tc := range []struct {
			out, start, limitPtr, limit, want uint32
			trap                              bool
		}{
			{256, 0, 0, 16, 512, false},
			{256, 9, 0, 25, 512, false},
			{0, 0, 0, 16, 16, false},
			{256, 19, 0, 16, 272, false},
			{256, math.MaxUint32, 0, 16, 528, false},
			{pageSize - 16, 0, 0, 16, 0, true},
			{256, 0, pageSize, 16, 0, true},
		} {
			if tc.limitPtr < pageSize {
				binary.LittleEndian.PutUint32(i.memory.bytes()[tc.limitPtr:], tc.limit)
			}

			var got uint32
			code := trapOf(func() { got = fill(i.ctx(), tc.out, tc.start, tc.limitPtr) })

			if tc.trap {
				if code != trapMemoryOOB {
					t.Fatalf("signed %t, %+v: trap %d", signed, tc, code)
				}
			} else if code != -1 || got != tc.want {
				t.Fatalf("signed %t, %+v: pointer %d, trap %d", signed, tc, got, code)
			}

			for j := uint32(0); j < 4; j++ {
				if got := binary.LittleEndian.Uint32(i.memory.bytes()[tc.out+4*j:]); got != j+1 {
					t.Fatalf("signed %t, %+v: store %d = %d", signed, tc, j, got)
				}
			}
		}
	}
}

func TestLoopFormatDispatch(t *testing.T) {
	choose := func(offset byte) []byte {
		store := func(value int32) []byte {
			return cat(get(0), i32c(value), []byte{0x3a, 0, offset}, get(0), i32c(value+1), []byte{0x3a, 0, offset + 1})
		}

		return cat([]byte{0x02, blockEmpty, 0x02, blockEmpty, 0x02, blockEmpty, 0x02, blockEmpty},
			get(1), []byte{opI32Load, 2, 0, 0x0e, 4, 0, 1, 2, 2, 2, 0x0b},
			store(10), []byte{0x0c, 2, 0x0b}, store(20), []byte{0x0c, 1, 0x0b}, store(30), []byte{0x0b})
	}
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: []ValType{I32, I32, I32}, results: i32, export: "fill",
		code: cat([]byte{opLoop, blockEmpty}, choose(0), choose(2), get(0), i32c(4), []byte{opI32Add, opLocalSet, 0},
			get(2), i32c(1), []byte{opI32Sub, opLocalTee, 2, opBrIf, 0, 0x0b}, get(0))}}}
	f := buildFunction(decodeModule(m.encode()), 0)
	optimize(f)
	checkSSA(f)
	i := load(t, m, nil)
	fn, _ := i.export("fill")
	fill := fn.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

	for _, format := range []uint32{0, 1, 2, 3, math.MaxUint32} {
		binary.LittleEndian.PutUint32(i.memory.bytes(), format)

		if got := fill(i.ctx(), 256, 0, 16); got != 320 {
			t.Fatalf("format %d: pointer %d", format, got)
		}

		value := byte(30)

		if format < 2 {
			value = 10 + 10*byte(format)
		}

		for j := uint32(0); j < 64; j++ {
			if got := i.memory.bytes()[256+j]; got != value+byte(j%2) {
				t.Fatalf("format %d: byte %d = %d", format, j, got)
			}
		}
	}

	clear(i.memory.bytes()[:64])

	if got := fill(i.ctx(), 0, 0, 16); got != 64 {
		t.Fatalf("aliased format: pointer %d", got)
	}

	for j := uint32(0); j < 64; j++ {
		want := byte(30 + j%2)

		if j < 2 {
			want = 10 + byte(j)
		}

		if got := i.memory.bytes()[j]; got != want {
			t.Fatalf("aliased format: byte %d = %d, want %d", j, got, want)
		}
	}
}

func TestFloatConstantStores(t *testing.T) {
	for _, bits := range [][2]uint64{{0, 0}, {0x80000000, 0x8000000000000000}, {0x7fc12345, 0xffffffffffffffff}, {0x3f800000, 0x3ff0000000000000}} {
		m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: i32, results: []ValType{I64}, export: "store",
			code: cat(get(0), binary.LittleEndian.AppendUint32([]byte{0x43}, uint32(bits[0])), []byte{0x38, 2, 0},
				get(0), binary.LittleEndian.AppendUint64([]byte{0x44}, bits[1]), []byte{0x39, 3, 8}, get(0), []byte{0x29, 3, 8})}}}
		i := load(t, m, nil)
		f, _ := i.export("store")

		if got := f.Interface().(func(uintptr, uint32) uint64)(i.ctx(), 0); got != bits[1] {
			t.Fatalf("f64 store = %016x, want %016x", got, bits[1])
		}

		if got := binary.LittleEndian.Uint32(i.memory.bytes()); uint64(got) != bits[0] {
			t.Fatalf("f32 store = %08x, want %08x", got, bits[0])
		}
	}
}

func TestArithmeticConditions(t *testing.T) {
	for _, opcode := range []byte{0x71, 0x72, 0x73, opI32Sub} {
		for _, mask := range []int32{-1, 1, 255, 256} {
			condition := cat(get(0), i32c(mask), []byte{opcode})
			m := &testModule{funcs: []testFunc{
				{params: i32, results: i32, export: "branch", code: cat(condition, []byte{opIf, byte(I32)}, i32c(17), []byte{opElse}, i32c(9), []byte{0x0b}, get(0), []byte{opI32Add})},
				{params: i32, results: i32, export: "select", code: cat(i32c(17), i32c(9), condition, []byte{0x1b}, get(0), []byte{opI32Add})},
			}}
			i := load(t, m, nil)

			for _, x := range []uint32{0, 1, 255, 256, 0x80000000, 0xffffff00, math.MaxUint32} {
				value := x & uint32(mask)

				switch opcode {
				case 0x72:
					value = x | uint32(mask)
				case 0x73:
					value = x ^ uint32(mask)
				case opI32Sub:
					value = x - uint32(mask)
				}

				want := x + 9

				if value != 0 {
					want = x + 17
				}

				for _, name := range []string{"branch", "select"} {
					f, _ := i.export(name)

					if got := f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), x); got != want {
						t.Fatalf("%s opcode %x mask %d input %x = %x, want %x", name, opcode, mask, x, got, want)
					}
				}
			}
		}
	}
}
