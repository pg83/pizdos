//go:build linux && amd64

package main

import (
	"bytes"
	"math/rand"
	"testing"
	"unsafe"
)

// memory.copy and memory.fill of a length known only at run time call the
// native memory functions: every length up to 300 and some far longer,
// from every alignment, overlapping either way, and a copy past 16 MiB that
// w2oMemmove leaves to Go's memmove, all as copy and a loop write them.
func TestMemoryFunctions(t *testing.T) {
	const pages = 600
	m := &testModule{memMin: pages, memMax: pages, funcs: []testFunc{
		{params: []ValType{I32, I32, I32}, export: "copy", code: cat(get(0), get(1), get(2), []byte{0xfc, 10, 0, 0})},
		{params: []ValType{I32, I32, I32}, export: "fill", code: cat(get(0), get(1), get(2), []byte{0xfc, 11, 0})},
	}}
	i := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), CompileOptions{memoryTail: true, tailRanges: true})), nil, 8<<20)
	i.start()
	defer i.release()
	f, _ := i.export("copy")
	copyFn := f.Interface().(func(uintptr, uint32, uint32, uint32))
	f, _ = i.export("fill")
	fillFn := f.Interface().(func(uintptr, uint32, uint32, uint32))
	mem := i.memory.bytes()
	want := make([]byte, len(mem))
	random := rand.New(rand.NewSource(1))
	random.Read(mem)
	copy(want, mem)
	lengths := []uint32{511, 512, 513, 1000, 4095, 4096, 4097, 65536, 100003, 1<<20 - 1, 1 << 20, 1<<20 + 77}

	for n := range uint32(301) {
		lengths = append(lengths, n)
	}

	// check compares the memory 4 KiB around the destination, or all of
	// it.
	check := func(what string, d, s, n uint32, all bool) {
		lo, hi := 0, len(mem)

		if !all {
			lo, hi = int(d)-4096, int(d+n)+4096
		}

		if !bytes.Equal(mem[lo:hi], want[lo:hi]) {
			at := lo

			for mem[at] == want[at] {
				at++
			}

			t.Fatalf("%s of %d bytes from %d to %d: byte %d differs", what, n, s, d, at)
		}
	}

	for _, n := range lengths {
		for _, pair := range [][2]uint32{{8192, 3 << 21}, {8195, 3<<21 + 5}, {3<<21 + 1, 8199}, {8192, 8193}, {8193, 8192}, {8192, 8192 + 33}, {8192 + 31, 8192}, {8192, 8192 + 100}, {8192 + 128, 8192}} {
			d, s := pair[0], pair[1]
			copyFn(i.ctx(), d, s, n)
			copy(want[d:d+n], want[s:s+n])
			check("copy", d, s, n, false)
		}

		for _, d := range []uint32{8192, 8197, 8223} {
			v := byte(n*7 + d)
			fillFn(i.ctx(), d, uint32(v)|0x5a00, n)

			for k := range n {
				want[d+k] = v
			}

			check("fill", d, 0, n, false)
		}
	}

	// Past 16 MiB, forward and overlapping.
	const huge = 17 << 20
	copyFn(i.ctx(), 4096, 1<<21, huge)
	copy(want[4096:4096+huge], want[1<<21:1<<21+huge])
	check("copy", 4096, 1<<21, huge, true)
	copyFn(i.ctx(), 1<<21, 4096, huge)
	copy(want[1<<21:1<<21+huge], want[4096:4096+huge])
	check("copy", 1<<21, 4096, huge, true)

	// The stand's copy into a module's memory.
	for _, n := range []int{0, 1, 31, 129, 4096, 1<<20 + 3} {
		src := make([]byte, n)
		random.Read(src)
		moveMemory(mem[12345:], src)
		copy(want[12345:], src)
		check("moveMemory", 12345, 0, uint32(n), false)
	}

	if useAVX2 {
		copyMemory(unsafe.Pointer(&mem[5000]), unsafe.Pointer(&mem[5001]), 200)
		copy(want[5000:5200], want[5001:5201])
		check("copyMemory", 5000, 5001, 200, true)
	}
}
