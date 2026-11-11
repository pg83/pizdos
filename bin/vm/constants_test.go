//go:build linux && amd64

package main

import (
	"encoding/binary"
	"math"
	"testing"
)

// A float constant used on both sides of a call lives in a constant slot
// across it: the code reads the constant where it would read the slot, as
// a reload and, in a loop, as an operand.
func TestConstantSlots(t *testing.T) {
	const opF32Mul, opBrIfLoop = 0x94, 0x0d
	f64c := func(x float64) []byte {
		return binary.LittleEndian.AppendUint64([]byte{0x44}, math.Float64bits(x))
	}
	f32c := func(x float32) []byte {
		return binary.LittleEndian.AppendUint32([]byte{0x43}, math.Float32bits(x))
	}
	f64s := []ValType{F64}
	f32s := []ValType{F32}
	m := &testModule{funcs: []testFunc{
		{params: f64s, results: f64s, code: get(0)},
		{params: f32s, results: f32s, code: get(0)},
		{params: f64s, results: f64s, export: "across", code: cat(get(0), f64c(1.1), []byte{opF64Mul}, call(0), f64c(1.1), []byte{opF64Mul})},
		{params: f32s, results: f32s, export: "across32", code: cat(get(0), f32c(1.1), []byte{opF32Mul}, call(1), f32c(1.1), []byte{opF32Mul})},
		{params: []ValType{F64, I32}, results: f64s, export: "loop", code: cat(
			[]byte{opLoop, blockEmpty}, get(0), f64c(1.25), []byte{opF64Mul}, call(0), f64c(1.25), []byte{opF64Add, opLocalSet, 0},
			get(1), i32c(1), []byte{opI32Sub, opLocalTee, 1, opBrIfLoop, 0, 0x0b}, get(0))},
	}}
	i := load(t, m, nil)
	f, _ := i.export("across")
	across := f.Interface().(func(uintptr, float64) float64)
	f, _ = i.export("across32")
	across32 := f.Interface().(func(uintptr, float32) float32)
	f, _ = i.export("loop")
	loop := f.Interface().(func(uintptr, float64, uint32) float64)

	x, c := 3.0, 1.1

	if got := across(i.ctx(), x); got != x*c*c {
		t.Fatalf("f64 constant across a call: %v, want %v", got, x*c*c)
	}

	x32, c32 := float32(3), float32(1.1)

	if got := across32(i.ctx(), x32); got != x32*c32*c32 {
		t.Fatalf("f32 constant across a call: %v, want %v", got, x32*c32*c32)
	}

	want := 1.0

	for k := 0; k < 5; k++ {
		want = want*1.25 + 1.25
	}

	if got := loop(i.ctx(), 1, 5); got != want {
		t.Fatalf("constant across a call in a loop: %v, want %v", got, want)
	}
}
