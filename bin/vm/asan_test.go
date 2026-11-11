//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"strings"
	"testing"
)

// An asan object checks every access against the shadow map: the bytes of
// the access must all be shadowOK, whatever the memory's size, and the trap
// names the access and carries its stack; a range a string function reads
// or writes is checked whole. The same module compiled strictly admits the
// accesses the map refuses.
func TestAsan(t *testing.T) {
	const opI64Load = 0x29
	m := &testModule{memMin: 2, memMax: 2,
		imports: []testImport{{module: "kernel", name: "__kernel_memcpy", params: []ValType{I32, I32, I32}, results: i32}},
		funcs: []testFunc{
			{params: i32, results: []ValType{I64}, export: "load8", code: cat(get(0), []byte{opI64Load, 3, 0})},
			{params: []ValType{I32, I32}, export: "store4", code: cat(get(0), get(1), []byte{opI32Store, 2, 0})},
			{params: i32, results: i32, export: "near", code: cat(get(0), []byte{opI32Load, 2}, leb(nil, 1000))},
			{params: i32, results: i32, export: "far", code: cat(get(0), []byte{opI32Load, 2}, leb(nil, 70000))},
			{results: i32, export: "fixed", code: cat(i32c(0x1800), []byte{opI32Load, 2, 0})},
			{results: i32, export: "fixedBad", code: cat(i32c(0x2000), []byte{opI32Load, 2, 0})},
			{params: []ValType{I32, I32, I32}, results: i32, export: "copy", code: cat(get(0), get(1), get(2), call(0))},
		}}
	asan := instantiate(t.Name(), readObject(compileModuleWithOptions(m.encode(), CompileOptions{asan: true})), nil, 8<<20)
	asan.start()
	sh := asan.memory.shadow

	if len(sh) != shadowSize {
		t.Fatalf("shadow map of %d bytes", len(sh))
	}

	fill(sh[0x1000:0x2000], shadowOK)
	mem := asan.memory.bytes()
	ctx := asan.ctx()
	fn := func(i *Instance, name string) any {
		f, _ := i.export(name)
		return f.Interface()
	}
	load8 := fn(asan, "load8").(func(uintptr, uint32) uint64)
	store4 := fn(asan, "store4").(func(uintptr, uint32, uint32))
	near := fn(asan, "near").(func(uintptr, uint32) uint32)
	far := fn(asan, "far").(func(uintptr, uint32) uint32)
	fixed := fn(asan, "fixed").(func(uintptr) uint32)
	fixedBad := fn(asan, "fixedBad").(func(uintptr) uint32)
	copyFn := fn(asan, "copy").(func(uintptr, uint32, uint32, uint32) uint32)
	// The module names no function: the trace names them by index, the
	// import first.
	expect := func(what string, index int, f func(), addr uint64, width, kind uint8) {
		trap := asanTrapOf(f)

		if trap == nil || trap.code != trapAsan || trap.addr != addr || trap.width != width || trap.kind != kind {
			t.Fatalf("%s: trap %+v, want asan at %#x, width %d, kind %d", what, trap, addr, width, kind)
		}

		if lines := traceLines(trap.stack); len(lines) == 0 || !strings.HasPrefix(lines[len(lines)-1], fmt.Sprintf("f%d+0x", index)) {
			t.Fatalf("%s: trace %q does not end in the function", what, lines)
		}
	}

	binary.LittleEndian.PutUint64(mem[0x1ff8:], 0x0102030405060708)

	if got := load8(ctx, 0x1ff8); got != 0x0102030405060708 {
		t.Fatalf("load8 at the end: %#x", got)
	}

	expect("load8", 1, func() { load8(ctx, 0x1ff9) }, 0x1ff9, 8, asanLoad)
	store4(ctx, 0x1ffc, 7)

	if binary.LittleEndian.Uint32(mem[0x1ffc:]) != 7 {
		t.Fatal("store4 at the end lost")
	}

	expect("store4", 2, func() { store4(ctx, 0xffe, 1) }, 0xffe, 4, asanStore)
	near(ctx, 0xc18)
	expect("near", 3, func() { near(ctx, 0xc17) }, 0xfff, 4, asanLoad)
	expect("far", 4, func() { far(ctx, 0x1000) }, 0x1000+70000, 4, asanLoad)
	fill(sh[0x1000+70000:0x1000+70004], shadowOK)
	far(ctx, 0x1000)

	if trap := asanTrapOf(func() { far(ctx, 0x20000) }); trap == nil || trap.code != trapMemoryOOB {
		t.Fatalf("far past the memory: trap %+v, want out of bounds", trap)
	}

	fixed(ctx)
	expect("fixedBad", 6, func() { fixedBad(ctx) }, 0x2000, 4, asanLoad)
	copyFn(ctx, 0x1800, 0x1000, 0x100)
	expect("copy", 7, func() { copyFn(ctx, 0x1f80, 0x1000, 0x100) }, 0x2000, 1, asanRange)
	expect("copy", 7, func() { copyFn(ctx, 0x1000, 0x1f80, 0x100) }, 0x2000, 1, asanRange)

	strict := instantiate(t.Name()+"Strict", readObject(compileModuleWithOptions(m.encode(), CompileOptions{})), nil, 8<<20)
	strict.start()

	if strict.memory.shadow != nil {
		t.Fatal("a strict object with a shadow map")
	}

	ctx = strict.ctx()
	fn(strict, "load8").(func(uintptr, uint32) uint64)(ctx, 0x1ff9)
	fn(strict, "store4").(func(uintptr, uint32, uint32))(ctx, 0xffe, 1)
	fn(strict, "fixedBad").(func(uintptr) uint32)(ctx)
	fn(strict, "copy").(func(uintptr, uint32, uint32, uint32) uint32)(ctx, 0x1f80, 0x1000, 0x100)
}

// asanTrapOf runs f and returns the trap that ended it, or nil.
func asanTrapOf(f func()) (trap *Trap) {
	defer func() {
		if e := recover(); e != nil {
			trap = e.(*Trap)
		}
	}()

	f()

	return nil
}
