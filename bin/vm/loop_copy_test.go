//go:build linux && amd64

package main

import (
	"bytes"
	"fmt"
	"testing"
)

func TestForwardByteLoops(t *testing.T) {
	type CopyCase struct {
		name                string
		width, form, stride int
		bulk                bool
	}
	m := &testModule{memMin: 1, memMax: 1, imports: []testImport{{"env", "noop", nil, nil}}}
	var cases []CopyCase

	for _, width := range []int{1, 2, 3, 4, 7, 8, 16} {
		for form := 0; form < 4; form++ {
			c := CopyCase{fmt.Sprintf("copy_%d_%d", width, form), width, form, width, true}
			var body []byte

			for j := 0; j < width; j++ {
				dst, src := get(0), get(1)

				if form != 2 {
					dst = cat(dst, get(3), []byte{opI32Add})
					src = cat(src, get(3), []byte{opI32Add})
				}

				body = append(body, cat(dst, src, []byte{byte(0x2c + j%2), 0, byte(j), 0x3a, 0, byte(j)})...)
			}

			var tail []byte

			if form == 2 {
				tail = cat(get(0), i32c(int32(width)), []byte{opI32Add, opLocalSet, 0},
					get(1), i32c(int32(width)), []byte{opI32Add, opLocalSet, 1})
			} else {
				tail = cat(get(3), i32c(int32(width)), []byte{opI32Add, opLocalSet, 3})
			}

			if form == 1 {
				tail = cat(tail, get(4), i32c(1), []byte{opI32Add, opLocalTee, 4}, get(2), []byte{0x49})
			} else {
				tail = cat(tail, get(2), i32c(1), []byte{opI32Sub, opLocalTee, 2})

				if form == 3 {
					tail = cat(tail, i32c(0), []byte{0x4a})
				}
			}

			result := get(3)

			if form == 2 {
				result = get(0)
			}

			m.funcs = append(m.funcs, testFunc{params: []ValType{I32, I32, I32}, results: i32,
				locals: []ValType{I32, I32}, export: c.name,
				code: cat([]byte{opLoop, blockEmpty}, body, tail, []byte{opBrIf, 0, 0x0b}, result)})
			cases = append(cases, c)
		}
	}

	// The same address bounds do not justify changing a backward traversal,
	// a group of loads before stores, or a traversal with gaps into a copy.
	m.funcs = append(m.funcs, testFunc{params: []ValType{I32, I32, I32}, results: i32, export: "reverse",
		code: cat([]byte{opLoop, blockEmpty}, get(2), i32c(1), []byte{opI32Sub, opLocalSet, 2},
			get(0), get(2), []byte{opI32Add}, get(1), get(2), []byte{opI32Add, 0x2d, 0, 0, 0x3a, 0, 0},
			get(2), []byte{opBrIf, 0, 0x0b}, i32c(0))})
	cases = append(cases, CopyCase{"reverse", 1, 4, 1, false})
	m.funcs = append(m.funcs, testFunc{params: []ValType{I32, I32, I32}, results: i32,
		locals: []ValType{I32, I32, I32}, export: "batched",
		code: cat([]byte{opLoop, blockEmpty}, get(1), get(3), []byte{opI32Add, 0x2d, 0, 0, opLocalSet, 4},
			get(1), get(3), []byte{opI32Add, 0x2d, 0, 1, opLocalSet, 5},
			get(0), get(3), []byte{opI32Add}, get(4), []byte{0x3a, 0, 0},
			get(0), get(3), []byte{opI32Add}, get(5), []byte{0x3a, 0, 1},
			get(3), i32c(2), []byte{opI32Add, opLocalSet, 3},
			get(2), i32c(1), []byte{opI32Sub, opLocalTee, 2, opBrIf, 0, 0x0b}, get(3))})
	cases = append(cases, CopyCase{"batched", 2, 5, 2, false})
	var gapped []byte

	for _, off := range []byte{0, 2} {
		gapped = append(gapped, cat(get(0), get(3), []byte{opI32Add}, get(1), get(3),
			[]byte{opI32Add, 0x2d, 0, off, 0x3a, 0, off})...)
	}

	m.funcs = append(m.funcs, testFunc{params: []ValType{I32, I32, I32}, results: i32, locals: i32, export: "gapped",
		code: cat([]byte{opLoop, blockEmpty}, gapped, get(3), i32c(4), []byte{opI32Add, opLocalSet, 3},
			get(2), i32c(1), []byte{opI32Sub, opLocalTee, 2, opBrIf, 0, 0x0b}, get(3))})
	cases = append(cases, CopyCase{"gapped", 2, 6, 4, false})
	i := load(t, m, testImports{"env.noop": func(uintptr) {}})

	for _, c := range cases {
		f, _ := i.export(c.name)
		run := f.Interface().(func(uintptr, uint32, uint32, uint32) uint32)

		for _, n := range []uint32{1, 3, 8, 17, 64} {
			for _, dst := range []uint32{127, 128, 129, 130, 132, 145, 2048} {
				for j := range i.memory.bytes() {
					i.memory.bytes()[j] = byte(j*37 + 11)
				}

				want := append([]byte(nil), i.memory.bytes()...)

				for j := uint32(0); j < n; j++ {
					index := j * uint32(c.stride)

					if c.form == 4 {
						index = n - j - 1
					}

					var batch [2]byte

					if c.form == 5 {
						copy(batch[:], want[128+index:128+index+2])
					}

					for k := uint32(0); k < uint32(c.width); k++ {
						off := k

						if c.form == 6 {
							off *= 2
						}

						value := want[128+index+off]

						if c.form == 5 {
							value = batch[k]
						}

						want[dst+index+off] = value
					}
				}

				result := n * uint32(c.stride)

				if c.form == 2 {
					result += dst
				} else if c.form == 4 {
					result = 0
				}

				if got := run(i.ctx(), dst, 128, n); got != result || !bytes.Equal(i.memory.bytes(), want) {
					t.Fatalf("%s(%d, %d): result %d, want %d or different bytes", c.name, dst, n, got, result)
				}
			}
		}

		if !c.bulk {
			continue
		}

		for _, pair := range [][2]uint32{{pageSize - 17, 128}, {128, pageSize - 17}, {pageSize - 17, pageSize - 18}} {
			for j := range i.memory.bytes() {
				i.memory.bytes()[j] = byte(j*37 + 11)
			}

			want := append([]byte(nil), i.memory.bytes()...)

			for j := uint32(0); j < 17; j++ {
				want[pair[0]+j] = want[pair[1]+j]
			}

			if trap := trapOf(func() { run(i.ctx(), pair[0], pair[1], 64) }); trap != trapMemoryOOB {
				t.Fatalf("%s crossing memory end: trap %d", c.name, trap)
			}

			if !bytes.Equal(i.memory.bytes(), want) {
				t.Fatalf("%s changed the stores preceding a trap", c.name)
			}
		}
	}
}
