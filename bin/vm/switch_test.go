//go:build linux && amd64

package main

import "testing"

// A br_table whose index the table covers, a mask of fewer bits than its
// cases, jumps without comparing the index with the table's size; one the
// table does not cover takes the default for the indexes past it.
func TestSwitchRange(t *testing.T) {
	const opBlock, opI32And, opBrTable = 0x02, 0x71, 0x0e
	switchOn := func(mask int32) []byte {
		code := []byte{}

		for k := 0; k < 5; k++ {
			code = append(code, opBlock, blockEmpty)
		}

		code = cat(code, get(0), i32c(mask), []byte{opI32And, opBrTable, 4, 0, 1, 2, 3, 4})

		for k := 0; k < 4; k++ {
			code = cat(code, []byte{0x0b}, i32c(int32(10+k)), []byte{opReturn})
		}

		return cat(code, []byte{0x0b}, i32c(99))
	}
	m := &testModule{funcs: []testFunc{
		{params: i32, results: i32, export: "covered", code: switchOn(3)},
		{params: i32, results: i32, export: "uncovered", code: switchOn(7)},
	}}
	i := load(t, m, nil)

	for _, name := range []string{"covered", "uncovered"} {
		f, _ := i.export(name)
		call := f.Interface().(func(uintptr, uint32) uint32)

		for _, x := range []uint32{0, 1, 2, 3, 4, 5, 6, 7, 8, 255, 1 << 31, ^uint32(0)} {
			mask := uint32(3)

			if name == "uncovered" {
				mask = 7
			}

			want := uint32(99)

			if x&mask < 4 {
				want = 10 + x&mask
			}

			if got := call(i.ctx(), x); got != want {
				t.Fatalf("%s(%d) = %d, want %d", name, x, got, want)
			}
		}
	}
}

// An interpreter's dispatch, a switch that every case returns to, is
// copied into the end of each case; all copies jump through one table.
func TestDuplicatedDispatch(t *testing.T) {
	const opBlock, opBr, opBrTable, opI32Load8U, opI32Mul = 0x02, 0x0c, 0x0e, 0x2d, 0x6c
	const cases = 20
	// params: pc; locals: acc 1, op 2
	next := cat(get(0), []byte{opI32Load8U, 0, 0, opLocalSet, 2}, get(0), i32c(1), []byte{opI32Add, opLocalSet, 0})
	code := cat(next, []byte{opLoop, blockEmpty, opBlock, blockEmpty})

	for k := 0; k < cases; k++ {
		code = append(code, opBlock, blockEmpty)
	}

	// The index is masked to the table's 32 entries, which cover it.
	code = cat(code, get(2), i32c(31), []byte{0x71, opBrTable, 32})

	for k := 0; k < 32; k++ {
		code = append(code, byte(min(k, cases)))
	}

	code = append(code, cases)

	for k := 0; k < cases; k++ {
		// case k: acc = acc*3 + k, then the next opcode, back to the loop
		code = cat(code, []byte{0x0b}, get(1), i32c(3), []byte{opI32Mul}, i32c(int32(k)), []byte{opI32Add, opLocalSet, 1}, next, []byte{opBr, byte(cases - k)})
	}

	code = cat(code, []byte{0x0b, 0x0b}, get(1))
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{params: i32, results: i32, locals: []ValType{I32, I32}, export: "run", code: code}}}

	for _, options := range []CompileOptions{{}, {memoryTail: true, tailRanges: true}} {
		i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), options)), nil, 8<<20)
		i.start()
		mem := i.memory.bytes()
		f, _ := i.export("run")
		run := f.Interface().(func(uintptr, uint32) uint32)
		program := []byte{3, 19, 0, 7, 7, 12, 1, 18, 5, 0, 19, 2, 11, 16, 9, 4, 13, 8, 17, 6, 10, 14, 15, 255}
		copy(mem[4000:], program)
		want := uint32(0)

		for _, b := range program {
			if b&31 >= cases {
				break
			}

			want = want*3 + uint32(b&31)
		}

		if got := run(i.ctx(), 4000); got != want {
			t.Fatalf("tail %t: interpreter computed %d, want %d", options.memoryTail, got, want)
		}

		i.release()
	}
}
