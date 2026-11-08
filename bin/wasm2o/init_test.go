//go:build linux && amd64

package main

import (
	"encoding/binary"
	"testing"
)

// Segment order, the start function, and active-segment drops must survive
// splitting a large generated initializer into several ordinary functions.
func TestSegmentedInit(t *testing.T) {
	const count = 385
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{code: cat(i32c(512), i32c(0), []byte{0x2d, 0, 0, 0x3a, 0, 0})},
		{export: "again", code: cat(i32c(1000), i32c(0), i32c(1), []byte{0xfc, 8, 0, 0})},
	}}
	wasm := m.encode()

	for at := 8; at < len(wasm); {
		id := wasm[at]
		size, n := binary.Uvarint(wasm[at+1:])

		if id == 10 {
			out := append([]byte(nil), wasm[:at]...)
			out = section(out, 8, []byte{0}) // start function
			out = section(out, 12, leb(nil, count))
			wasm = append(out, wasm[at:]...)

			break
		}

		at += 1 + n + int(size)
	}

	data := leb(nil, count)
	var want [257]byte

	for j := 0; j < count; j++ {
		offset := j * 17 % len(want)
		value := byte(j%251 + 1)

		if j == count-1 {
			offset, value = 0, 77
		}

		data = append(data, 0) // active segment, memory zero
		data = append(data, i32c(int32(offset))...)
		data = append(data, 0x0b, 1, value)
		want[offset] = value
	}

	wasm = section(wasm, 11, data)
	var i *Instance

	if e := try(func() {
		i = instantiate(t.Name(), readObject(compileModule(wasm)), nil, 8<<20)
		i.start()
	}); e != nil {
		t.Fatal(e)
	}

	for offset, value := range want {
		if got := i.memory.bytes()[offset]; got != value {
			t.Fatalf("data byte %d = %d, want %d", offset, got, value)
		}
	}

	if got := i.memory.bytes()[512]; got != want[0] {
		t.Fatalf("start function read %d, want last segment's %d", got, want[0])
	}

	again, _ := i.export("again")

	if code := trapOf(func() { again.Interface().(func(uintptr))(i.ctx()) }); code != trapMemoryOOB {
		t.Fatalf("memory.init after active-segment drop: trap %d", code)
	}
}
