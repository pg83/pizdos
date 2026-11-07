//go:build linux && amd64

package main

import (
	"encoding/binary"
	"math"
	"reflect"
	"runtime"
	"testing"
	"time"
)

// Modules built here run on the test's goroutines through the loader, with
// Go functions as imports: the tests drive Go's runtime through native
// frames, its collector, stack growth, parking, preemption and panics.

type testImport struct {
	module, name    string
	params, results []ValType
}

type testFunc struct {
	params, results []ValType
	locals          []ValType
	code            []byte
	export          string
}

type testModule struct {
	imports []testImport
	funcs   []testFunc
	// memory, when max is not zero, is min..max pages.
	memMin, memMax uint32
}

func leb(out []byte, x uint64) []byte {
	return binary.AppendUvarint(out, x)
}

func sleb(out []byte, x int64) []byte {
	return appendSleb(out, x)
}

func section(out []byte, id byte, payload []byte) []byte {
	out = append(out, id)
	out = leb(out, uint64(len(payload)))

	return append(out, payload...)
}

func name(out []byte, s string) []byte {
	return append(leb(out, uint64(len(s))), s...)
}

func vals(out []byte, ts []ValType) []byte {
	out = leb(out, uint64(len(ts)))

	for _, t := range ts {
		out = append(out, byte(t))
	}

	return out
}

func (m *testModule) encode() []byte {
	out := []byte{0, 'a', 's', 'm', 1, 0, 0, 0}
	var types, imports, funcs, exports, code []byte
	n := uint64(len(m.imports) + len(m.funcs))
	types = leb(types, n)

	for _, imp := range m.imports {
		types = vals(vals(append(types, 0x60), imp.params), imp.results)
	}

	for _, f := range m.funcs {
		types = vals(vals(append(types, 0x60), f.params), f.results)
	}

	imports = leb(imports, uint64(len(m.imports)))

	for i, imp := range m.imports {
		imports = leb(append(name(name(imports, imp.module), imp.name), 0), uint64(i))
	}

	funcs = leb(funcs, uint64(len(m.funcs)))
	exported := 0

	for i, f := range m.funcs {
		funcs = leb(funcs, uint64(len(m.imports)+i))

		if f.export != "" {
			exported++
		}
	}

	exports = leb(exports, uint64(exported))
	code = leb(code, uint64(len(m.funcs)))

	for i, f := range m.funcs {
		if f.export != "" {
			exports = leb(append(name(exports, f.export), 0), uint64(len(m.imports)+i))
		}

		body := leb(nil, uint64(len(f.locals)))

		for _, t := range f.locals {
			body = append(leb(body, 1), byte(t))
		}

		body = append(append(body, f.code...), 0x0b)
		code = append(leb(code, uint64(len(body))), body...)
	}

	out = section(out, 1, types)
	out = section(out, 2, imports)
	out = section(out, 3, funcs)

	if m.memMax != 0 {
		out = section(out, 5, leb(leb([]byte{1, 1}, uint64(m.memMin)), uint64(m.memMax)))
	}

	out = section(out, 7, exports)

	return section(out, 10, code)
}

// wasm opcodes the tests use
const (
	opUnreachable = 0x00
	opLoop        = 0x03
	opIf          = 0x04
	opElse        = 0x05
	opBrIf        = 0x0d
	opCall        = 0x10
	opLocalGet    = 0x20
	opLocalSet    = 0x21
	opLocalTee    = 0x22
	opI32Load     = 0x28
	opI32Store    = 0x36
	opMemoryGrow  = 0x40
	opI32Const    = 0x41
	opI64Const    = 0x42
	opI32Eqz      = 0x45
	opI32Add      = 0x6a
	opI32Sub      = 0x6b
	opI64Add      = 0x7c
	opI64Mul      = 0x7e
	opF32Add      = 0x92
	opF64Add      = 0xa0
	opF64Mul      = 0xa2
	blockEmpty    = 0x40
)

func get(i int) []byte {
	return leb([]byte{opLocalGet}, uint64(i))
}

func i32c(x int32) []byte {
	return sleb([]byte{opI32Const}, int64(x))
}

func i64c(x int64) []byte {
	return sleb([]byte{opI64Const}, x)
}

func call(f int) []byte {
	return leb([]byte{opCall}, uint64(f))
}

func cat(parts ...[]byte) []byte {
	var out []byte

	for _, p := range parts {
		out = append(out, p...)
	}

	return out
}

// testImports maps "module.name" to Go function values.
type testImports map[string]any

func (t testImports) function(imp *ImportMeta) any {
	return t[imp.module+"."+imp.name]
}

func (t testImports) global(*ImportMeta) uintptr {
	return 0
}

func (t testImports) memory(*ImportMeta) *LinearMemory {
	return nil
}

func (t testImports) table(*ImportMeta) uintptr {
	return 0
}

func load(t *testing.T, m *testModule, imports testImports) *Instance {
	t.Helper()
	var i *Instance

	if e := try(func() {
		obj := readObject(compileModule(m.encode()))
		i = instantiate(t.Name(), obj, imports, 8<<20)
		i.start()
	}); e != nil {
		t.Fatal(e)
	}

	return i
}

// trapOf runs f and returns the code of the trap it ends with, or -1.
func trapOf(f func()) (code int) {
	defer func() {
		r := recover()

		if trap, ok := r.(*Trap); ok {
			code = int(trap.code)

			return
		}

		if r != nil {
			panic(r)
		}
	}()

	f()

	return -1
}

var i32 = []ValType{I32}

// growGo takes Go stack of its own, through big frames.
func growGo(n int) int {
	var pad [512]byte
	pad[n%512] = byte(n)

	if n == 0 {
		return int(pad[0])
	}

	return growGo(n-1) + int(pad[n%512]) - int(byte(n))
}

// A recursion 100000 frames deep calls Go at its bottom, which collects,
// grows its own stack and parks: every native frame is walked and copied,
// and the sum still comes out.
func TestDeepNativeFrames(t *testing.T) {
	calls := 0
	m := &testModule{
		imports: []testImport{{"env", "bottom", i32, i32}},
		funcs: []testFunc{{
			params: i32, results: i32, export: "rec",
			code: cat(get(0), []byte{opI32Eqz, opIf, byte(I32)}, i32c(7), call(0), []byte{opElse},
				get(0), i32c(1), []byte{opI32Sub}, call(1), i32c(1), []byte{opI32Add, 0x0b}),
		}},
	}
	i := load(t, m, testImports{"env.bottom": func(ctx uintptr, x uint32) uint32 {
		calls++
		runtime.GC()
		growGo(2000)
		done := make(chan uint32)

		go func() {
			time.Sleep(time.Millisecond)
			done <- 2 * x
		}()

		runtime.GC()

		return <-done
	}})
	rec := func(n uint32) uint32 {
		f, _ := i.export("rec")

		return f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), n)
	}

	for _, n := range []uint32{0, 10, 100000} {
		if got := rec(n); got != n+14 {
			t.Fatalf("rec(%d) = %d, want %d", n, got, n+14)
		}
	}

	if calls != 3 {
		t.Fatalf("bottom called %d times", calls)
	}

	runtime.KeepAlive(i)
}

// A loop without calls spins on one processor until another goroutine
// stores a flag in its memory: only asynchronous preemption lets that one
// run, and a collection in between must stop the spinning goroutine.
func TestPreemptLoop(t *testing.T) {
	defer runtime.GOMAXPROCS(runtime.GOMAXPROCS(1))

	m := &testModule{
		memMin: 1, memMax: 1,
		funcs: []testFunc{{
			results: i32, export: "spin",
			code: cat([]byte{opLoop, blockEmpty}, i32c(0), []byte{opI32Load, 2, 0, opI32Eqz, opBrIf, 0, 0x0b}, i32c(0), []byte{opI32Load, 2, 0}),
		}},
	}
	i := load(t, m, nil)
	spin, _ := i.export("spin")
	result := make(chan uint32)

	go func() {
		result <- spin.Interface().(func(uintptr) uint32)(i.ctx())
	}()

	time.Sleep(10 * time.Millisecond)
	runtime.GC()
	binary.LittleEndian.PutUint32(i.memory.bytes(), 42)

	select {
	case got := <-result:
		if got != 42 {
			t.Fatalf("spin = %d", got)
		}
	case <-time.After(30 * time.Second):
		t.Fatal("the spinning goroutine never saw the flag")
	}

	runtime.KeepAlive(i)
}

// A trap 1000 frames down panics through them to the caller, which goes on.
func TestTrapThroughFrames(t *testing.T) {
	m := &testModule{
		funcs: []testFunc{{
			params: i32, results: i32, export: "boom",
			code: cat(get(0), []byte{opI32Eqz, opIf, blockEmpty, opUnreachable, 0x0b}, get(0), i32c(1), []byte{opI32Sub}, call(0), i32c(1), []byte{opI32Add}),
		}},
	}
	i := load(t, m, nil)
	f, _ := i.export("boom")
	boom := f.Interface().(func(uintptr, uint32) uint32)

	for range 3 {
		if code := trapOf(func() { boom(i.ctx(), 1000) }); code != trapUnreachable {
			t.Fatalf("trap %d", code)
		}
	}

	runtime.KeepAlive(i)
}

// Unbounded recursion traps at the instance's stack limit.
func TestExhaustion(t *testing.T) {
	m := &testModule{
		funcs: []testFunc{{params: i32, results: i32, export: "inf", code: cat(get(0), i32c(1), []byte{opI32Add}, call(0))}},
	}
	i := load(t, m, nil)
	f, _ := i.export("inf")

	if code := trapOf(func() { f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), 0) }); code != trapStack {
		t.Fatalf("trap %d", code)
	}

	runtime.KeepAlive(i)
}

// wide types: five rounds of i32, i64, f32, f64
var wide = func() []ValType {
	var out []ValType

	for range 5 {
		out = append(out, I32, I64, F32, F64)
	}

	return out
}()

// twelve results: ten integers, more than Go's nine result registers and
// the internal convention's seven
var wideResults = []ValType{I64, I32, F64, I64, I64, I64, I64, I64, I64, I32, I64, F32}

// The export takes 20 values and returns them through an import of the same
// shape, Go to native to Go and back: stack arguments, spill space and
// stack results on both sides of both boundaries.
func TestWideSignatures(t *testing.T) {
	var body []byte

	for k := range wide {
		body = append(body, get(k)...)
	}

	m := &testModule{
		imports: []testImport{{"env", "wide", wide, wideResults}},
		funcs:   []testFunc{{params: wide, results: wideResults, export: "wide", code: cat(body, call(0))}},
	}
	in := []reflect.Type{reflect.TypeFor[uintptr]()}

	for _, v := range wide {
		in = append(in, goType(v))
	}

	var out []reflect.Type

	for _, v := range wideResults {
		out = append(out, goType(v))
	}

	// The import's results: per kind, a sum over the arguments of that kind
	// and a fingerprint of their order.
	fold := func(args []reflect.Value) []reflect.Value {
		var ints, order uint64
		var floats float64

		for k, a := range args[1:] {
			switch v := a.Interface().(type) {
			case uint32:
				ints += uint64(v)
				order = order*31 + uint64(v)
			case uint64:
				ints += v
				order = order*37 + v
			case float32:
				floats += float64(v)
			case float64:
				floats += v * float64(k)
			}
		}

		results := make([]reflect.Value, len(wideResults))

		for k, v := range wideResults {
			x := order + uint64(k)*ints

			switch v {
			case I32:
				results[k] = reflect.ValueOf(uint32(x))
			case I64:
				results[k] = reflect.ValueOf(x)
			case F32:
				results[k] = reflect.ValueOf(float32(floats))
			case F64:
				results[k] = reflect.ValueOf(floats + float64(k))
			}
		}

		return results
	}

	goWide := reflect.MakeFunc(reflect.FuncOf(in, out, false), fold)
	i := load(t, m, testImports{"env.wide": goWide.Interface()})
	f, _ := i.export("wide")
	args := []reflect.Value{reflect.ValueOf(i.ctx())}

	for k, v := range wide {
		x := uint64(k*1000003 + 17)

		switch v {
		case I32:
			args = append(args, reflect.ValueOf(uint32(x)))
		case I64:
			args = append(args, reflect.ValueOf(x<<33|x))
		case F32:
			args = append(args, reflect.ValueOf(float32(k)+0.5))
		case F64:
			args = append(args, reflect.ValueOf(float64(k)*1.25))
		}
	}

	got := f.Call(args)
	want := fold(args)

	for k := range want {
		if !reflect.DeepEqual(got[k].Interface(), want[k].Interface()) {
			t.Fatalf("result %d = %v, want %v", k, got[k], want[k])
		}
	}

	runtime.KeepAlive(i)
}

// memory.grow moves nothing and keeps old contents; growth past the
// maximum fails.
func TestMemoryGrow(t *testing.T) {
	m := &testModule{
		memMin: 1, memMax: 3,
		funcs: []testFunc{
			{params: i32, results: i32, export: "grow", code: cat(get(0), []byte{opMemoryGrow, 0})},
			{params: []ValType{I32, I32}, export: "store", code: cat(get(0), get(1), []byte{opI32Store, 2, 0})},
			{params: i32, results: i32, export: "load", code: cat(get(0), []byte{opI32Load, 2, 0})},
		},
	}
	i := load(t, m, nil)
	fn := func(name string) reflect.Value {
		f, _ := i.export(name)

		return f
	}
	grow := fn("grow").Interface().(func(uintptr, uint32) uint32)
	store := fn("store").Interface().(func(uintptr, uint32, uint32))
	read := fn("load").Interface().(func(uintptr, uint32) uint32)
	store(i.ctx(), 100, 7)

	if code := trapOf(func() { store(i.ctx(), pageSize, 1) }); code != trapMemoryOOB {
		t.Fatalf("store past the end: trap %d", code)
	}

	if old := grow(i.ctx(), 2); old != 1 {
		t.Fatalf("grow = %d", old)
	}

	store(i.ctx(), 3*pageSize-4, 9)

	if got := read(i.ctx(), 100) + read(i.ctx(), 3*pageSize-4); got != 16 {
		t.Fatalf("loads = %d", got)
	}

	if old := grow(i.ctx(), 1); old != math.MaxUint32 {
		t.Fatalf("grow past the maximum = %d", old)
	}

	runtime.KeepAlive(i)
}
