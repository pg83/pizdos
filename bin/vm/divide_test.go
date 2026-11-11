//go:build linux && amd64

package main

import (
	"fmt"
	"math"
	"testing"
)

// wasm opcodes of integer division
const (
	opI32DivS = 0x6d
	opI32DivU = 0x6e
	opI32RemS = 0x6f
	opI32RemU = 0x70
	opI64DivS = 0x7f
	opI64DivU = 0x80
	opI64RemS = 0x81
	opI64RemU = 0x82
)

// divisionValues are dividends around the edges of both widths and near
// multiples of the divisor, plus a fixed pseudo-random sample.
func divisionValues(d uint64, wide bool) []uint64 {
	out := []uint64{0, 1, 2, 3, 7, 10, 255, 256, 1000, 65535, 65536}
	if wide {
		out = append(out, math.MaxInt64, 1<<63, 1<<63+1, math.MaxUint64, math.MaxUint64-1, 1<<32, 1<<32-1, 1<<32+1)
	} else {
		out = append(out, math.MaxInt32, 1<<31, 1<<31+1, math.MaxUint32, math.MaxUint32-1)
	}
	for _, k := range []uint64{1, 2, 3, 100, 12345, 1 << 20} {
		for _, delta := range []uint64{0, 1, ^uint64(0)} {
			out = append(out, d*k+delta, -(d*k + delta))
		}
	}
	x := uint64(0x9e3779b97f4a7c15)
	for range 200 {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		out = append(out, x, x>>33, x>>48)
	}
	if !wide {
		for i := range out {
			out[i] = uint64(uint32(out[i]))
		}
	}

	return out
}

// divisionWant is Wasm's result of op, or the trap it ends with.
func divisionWant(op byte, x, d uint64) (uint64, int) {
	switch op {
	case opI32DivS, opI32RemS:
		a, b := int32(x), int32(d)
		if b == 0 {
			return 0, trapDivByZero
		}
		if op == opI32RemS {
			if b == -1 {
				return 0, -1
			}
			return uint64(uint32(a % b)), -1
		}
		if a == math.MinInt32 && b == -1 {
			return 0, trapIntOverflow
		}
		return uint64(uint32(a / b)), -1
	case opI32DivU, opI32RemU:
		a, b := uint32(x), uint32(d)
		if b == 0 {
			return 0, trapDivByZero
		}
		if op == opI32RemU {
			return uint64(a % b), -1
		}
		return uint64(a / b), -1
	case opI64DivS, opI64RemS:
		a, b := int64(x), int64(d)
		if b == 0 {
			return 0, trapDivByZero
		}
		if op == opI64RemS {
			if b == -1 {
				return 0, -1
			}
			return uint64(a % b), -1
		}
		if a == math.MinInt64 && b == -1 {
			return 0, trapIntOverflow
		}
		return uint64(a / b), -1
	default:
		if d == 0 {
			return 0, trapDivByZero
		}
		if op == opI64RemU {
			return x % d, -1
		}
		return x / d, -1
	}
}

// Constant divisors divide by shifts and reciprocal multiplications; their
// results and traps must equal Wasm's for every width, sign and kind.
func TestDivisionByConstants(t *testing.T) {
	narrow := []int64{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 25, 100, 125, 255, 256, 641, 1000, 3600,
		65535, 65536, 65537, 1 << 30, math.MaxInt32, -1, -2, -3, -4, -5, -7, -8, -10, -255, -256, -1000,
		-math.MaxInt32, math.MinInt32, math.MinInt32 + 1, -2, 0x7ffffffe, -0x40000000}
	wide := []int64{0, 1, 2, 3, 7, 8, 10, 1000, 1 << 32, 1<<32 + 1, 1 << 62, math.MaxInt64, -1, -2, -3, -8,
		-1000, -(1 << 62), math.MinInt64, math.MinInt64 + 1}
	type kernel struct {
		op   byte
		d    uint64
		wide bool
	}
	var kernels []kernel
	m := &testModule{}
	for _, d := range narrow {
		for _, op := range []byte{opI32DivS, opI32DivU, opI32RemS, opI32RemU} {
			kernels = append(kernels, kernel{op, uint64(uint32(d)), false})
			m.funcs = append(m.funcs, testFunc{params: i32, results: i32, export: fmt.Sprint(len(m.funcs)), code: cat(get(0), i32c(int32(d)), []byte{op})})
		}
	}
	for _, d := range wide {
		for _, op := range []byte{opI64DivS, opI64DivU, opI64RemS, opI64RemU} {
			kernels = append(kernels, kernel{op, uint64(d), true})
			m.funcs = append(m.funcs, testFunc{params: []ValType{I64}, results: []ValType{I64}, export: fmt.Sprint(len(m.funcs)), code: cat(get(0), i64c(d), []byte{op})})
		}
	}
	for _, optimized := range []bool{false, true} {
		var i *Instance
		if optimized {
			i = loadMemoryTail(t, m, true)
		} else {
			i = load(t, m, testImports{})
			t.Cleanup(i.release)
		}
		for n, k := range kernels {
			f, _ := i.export(fmt.Sprint(n))
			for _, x := range divisionValues(k.d, k.wide) {
				want, wantTrap := divisionWant(k.op, x, k.d)
				var got uint64
				trap := trapOf(func() {
					if k.wide {
						got = f.Interface().(func(uintptr, uint64) uint64)(i.ctx(), x)
					} else {
						got = uint64(f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), uint32(x)))
					}
				})
				if trap != wantTrap || trap < 0 && got != want {
					t.Fatalf("optimized %t, op %#x, %#x by %#x: got %#x trap %d, want %#x trap %d", optimized, k.op, x, k.d, got, trap, want, wantTrap)
				}
			}
		}
	}
}
