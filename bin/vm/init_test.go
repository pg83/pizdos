//go:build linux && amd64

package main

import (
	"encoding/binary"
	"strings"
	"testing"
)

// Keep segment order and drops across descriptor pages; start must see the
// final result of overlapping active segments. The segments start at
// boundSlack, where the compiler admits data.
func TestSegmentedInit(t *testing.T) {
	const count = 5001
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{code: cat(i32c(boundSlack+512), i32c(boundSlack), []byte{0x2d, 0, 0, 0x3a, 0, 0})},
		{export: "again", code: cat(i32c(1000), i32c(0), i32c(1), []byte{0xfc, 8, 0, 0})},
		{export: "last", code: cat(i32c(1000), i32c(0), i32c(1), []byte{0xfc, 8}, leb(nil, count-1), []byte{0})},
	}}

	data := leb(nil, count)
	var want [257]byte

	for j := 0; j < count; j++ {
		offset := j * 17 % len(want)
		value := byte(j%251 + 1)

		if j == count-1 {
			offset, value = 0, 77
		}

		data = append(data, 0) // active segment, memory zero
		data = append(data, i32c(boundSlack+int32(offset))...)
		data = append(data, 0x0b, 1, value)
		want[offset] = value
	}

	wasm := initDataModule(m, data, true)
	var i *Instance

	if e := try(func() {
		i = instantiate(t.Name(), readObject(compileModule(wasm)), nil, 8<<20)
		i.start()
	}); e != nil {
		t.Fatal(e)
	}
	defer i.release()

	for offset, value := range want {
		if got := i.memory.bytes()[boundSlack+offset]; got != value {
			t.Fatalf("data byte %d = %d, want %d", offset, got, value)
		}
	}

	if got := i.memory.bytes()[boundSlack+512]; got != want[0] {
		t.Fatalf("start function read %d, want last segment's %d", got, want[0])
	}

	for _, name := range []string{"again", "last"} {
		again, _ := i.export(name)

		if code := trapOf(func() { again.Interface().(func(uintptr))(i.ctx()) }); code != trapMemoryOOB {
			t.Fatalf("%s: memory.init after active-segment drop: trap %d", name, code)
		}
	}
}

func TestInitPassiveAndEmptySegments(t *testing.T) {
	const count = 130
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{
		{export: "passive", code: cat(i32c(1000), i32c(0), i32c(1), []byte{0xfc, 8, 7, 0})},
		{export: "end", code: cat(i32c(65536), i32c(0), i32c(0), []byte{0xfc, 8}, leb(nil, count-1), []byte{0})},
	}}
	data := leb(nil, count)

	for j := 0; j < count; j++ {
		if j == 7 {
			data = append(data, 1, 1, 0x7f) // passive
		} else if j == count-1 {
			data = append(data, 0)
			data = append(data, i32c(65536)...)
			data = append(data, 0x0b, 0) // empty at the memory boundary
		} else {
			data = append(data, 0)
			data = append(data, i32c(boundSlack+int32(j))...)
			data = append(data, 0x0b, 1, byte(j))
		}
	}

	var i *Instance

	if e := try(func() {
		i = instantiate(t.Name(), readObject(compileModule(initDataModule(m, data, false))), nil, 8<<20)
		i.start()
	}); e != nil {
		t.Fatal(e)
	}
	defer i.release()

	for _, name := range []string{"passive", "end"} {
		entry, _ := i.export(name)

		if e := try(func() { entry.Interface().(func(uintptr))(i.ctx()) }); e != nil {
			t.Fatalf("%s: %v", name, e)
		}
	}

	if got := i.memory.bytes(); got[boundSlack+6] != 6 || got[boundSlack+7] != 0 || got[boundSlack+8] != 8 || got[1000] != 0x7f {
		t.Fatal("passive data was initialized or dropped with the active data")
	}
}

// Compilation with the tail refuses a module keeping data in the first
// boundSlack bytes of its memory, where an address that wrapped around 2^32
// would land; strict compilation takes it.
func TestInitLowDataRefused(t *testing.T) {
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{}}}
	data := leb(nil, 1)
	data = append(data, 0)
	data = append(data, i32c(boundSlack-1)...)
	data = append(data, 0x0b, 1, 0x7f)
	wasm := initDataModule(m, data, false)

	if e := try(func() { compileModule(wasm) }); e == nil || !strings.Contains(e.message(), "first KiB") {
		t.Fatalf("data below boundSlack: %v", e)
	}

	readObject(compileModuleWithOptions(wasm, CompileOptions{}))
}

// An unsafe destination keeps its original checks and the writes preceding
// its trap, even when the module has enough segments for batch initialization.
func TestInitOutOfBoundsSegment(t *testing.T) {
	const count = 130
	m := &testModule{memMin: 1, memMax: 1, funcs: []testFunc{{}}}
	data := leb(nil, count)

	for j := 0; j < count; j++ {
		offset := int32(j)
		if j == count-1 {
			offset = 65536
		}
		data = append(data, 0)
		data = append(data, i32c(offset)...)
		data = append(data, 0x0b, 1, 0x7f)
	}

	obj := readObject(compileModuleWithOptions(initDataModule(m, data, false), CompileOptions{}))
	i := instantiate(t.Name(), obj, nil, 8<<20)
	defer i.release()

	if code := trapOf(i.start); code != trapMemoryOOB {
		t.Fatalf("active segment outside memory: trap %d", code)
	}
	if i.memory.bytes()[0] != 0x7f {
		t.Fatal("lost the data writes preceding the trap")
	}
}

func initDataModule(m *testModule, data []byte, start bool) []byte {
	wasm := m.encode()
	count, _ := binary.Uvarint(data)

	for at := 8; at < len(wasm); {
		id := wasm[at]
		size, n := binary.Uvarint(wasm[at+1:])

		if id == 10 {
			out := append([]byte(nil), wasm[:at]...)
			if start {
				out = section(out, 8, []byte{0})
			}
			out = section(out, 12, leb(nil, count))
			wasm = append(out, wasm[at:]...)
			break
		}

		at += 1 + n + int(size)
	}

	return section(wasm, 11, data)
}
