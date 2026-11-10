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
