//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"testing"
)

func TestFloatIntegerMerges(t *testing.T) {
	f64c := func(x float64) []byte {
		return binary.LittleEndian.AppendUint64([]byte{0x44}, math.Float64bits(x))
	}
	constants := []float64{0, 0.5, 65535, 2147483648, 4294967295, math.NaN()}
	m := &testModule{}

	for j, k := range constants {
		for pred := 0; pred < 6; pred++ {
			m.funcs = append(m.funcs, testFunc{params: []ValType{I32, I32}, results: i32, export: fmt.Sprintf("cmp%d_%d", j, pred),
				code: cat(get(0), []byte{opIf, byte(F64)}, get(1), []byte{0xb8, opElse}, f64c(4294967295),
					[]byte{0x0b}, f64c(k), []byte{byte(0x61 + pred)})})
		}
	}

	m.funcs = append(m.funcs, testFunc{params: []ValType{I32, I32}, results: []ValType{I32, I64}, locals: []ValType{F64}, export: "bits",
		code: cat(get(0), []byte{opIf, byte(F64)}, get(1), []byte{0xb8, opElse}, f64c(math.Copysign(0, -1)),
			[]byte{0x0b, opLocalTee, 2}, f64c(0), []byte{0x61}, get(2), []byte{0xbd})})
	module := decodeModule(m.encode())

	for fn := range module.funcs {
		f := buildFunction(module, fn)
		optimize(f)
		checkSSA(f)
	}

	i := load(t, m, nil)

	for j, k := range constants {
		for pred := 0; pred < 6; pred++ {
			f, _ := i.export(fmt.Sprintf("cmp%d_%d", j, pred))
			compare := f.Interface().(func(uintptr, uint32, uint32) uint32)

			for _, flag := range []uint32{0, 1} {
				for _, x := range []uint32{0, 1, 65535, 65536, math.MaxInt32, 1 << 31, math.MaxUint32} {
					y := float64(math.MaxUint32)

					if flag != 0 {
						y = float64(x)
					}

					want := []bool{y == k, y != k, y < k, y > k, y <= k, y >= k}[pred]

					if got := compare(i.ctx(), flag, x); (got != 0) != want {
						t.Fatalf("predicate %d, flag %d, x %d, k %g: %d, want %t", pred, flag, x, k, got, want)
					}
				}
			}
		}
	}

	f, _ := i.export("bits")
	bits := f.Interface().(func(uintptr, uint32, uint32) (uint32, uint64))

	for _, flag := range []uint32{0, 1} {
		got, raw := bits(i.ctx(), flag, 0)
		want := uint64(0)

		if flag == 0 {
			want = 1 << 63
		}

		if got != 1 || raw != want {
			t.Fatalf("flag %d: compare %d, original bits %016x, want %016x", flag, got, raw, want)
		}
	}
}
