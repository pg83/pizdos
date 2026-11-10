//go:build linux && amd64

package main

import (
	"encoding/binary"
	"math"
	"reflect"
	"runtime"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
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
	// importExports exports imports by index under these names
	importExports map[int]string
}

func leb(out []byte, x uint64) []byte {
	return binary.AppendUvarint(out, x)
}

func appendSleb(b []byte, v int64) []byte {
	for {
		c := byte(v & 0x7f)
		v >>= 7

		if (v == 0 && c&0x40 == 0) || (v == -1 && c&0x40 != 0) {
			return append(b, c)
		}

		b = append(b, c|0x80)
	}
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

	exported += len(m.importExports)

	for i, f := range m.funcs {
		funcs = leb(funcs, uint64(len(m.imports)+i))

		if f.export != "" {
			exported++
		}
	}

	exports = leb(exports, uint64(exported))

	for i, export := range m.importExports {
		exports = leb(append(name(exports, export), 0), uint64(i))
	}

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
		// These modules include assertions on exact Wasm trap boundaries.
		// The default padded compiler is exercised by memory_tail_test
		// and by the self-compilation, decoder and BusyBox tests.
		obj := readObject(compileModuleWithOptions(m.encode(), CompileOptions{}))
		i = instantiate(t.Name(), obj, imports, 8<<20)
		i.start()
	}); e != nil {
		t.Fatal(e)
	}

	return i
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
// deepHost counts the calls of deepBottom.
type deepHost struct {
	calls int
}

// deepBottom collects, grows Go's stack and waits on another goroutine,
// under the frames of the code that called it.
func deepBottom(ctx uintptr, x uint32) uint32 {
	instanceAt(ctx).host.(*deepHost).calls++
	runtime.GC()
	growGo(2000)
	done := make(chan uint32)

	go func() {
		time.Sleep(time.Millisecond)
		done <- 2 * x
	}()

	runtime.GC()

	return <-done
}

func TestDeepNativeFrames(t *testing.T) {
	host := &deepHost{}
	m := &testModule{
		imports: []testImport{{"env", "bottom", i32, i32}},
		funcs: []testFunc{{
			params: i32, results: i32, export: "rec",
			code: cat(get(0), []byte{opI32Eqz, opIf, byte(I32)}, i32c(7), call(0), []byte{opElse},
				get(0), i32c(1), []byte{opI32Sub}, call(1), i32c(1), []byte{opI32Add, 0x0b}),
		}},
	}
	i := load(t, m, testImports{"env.bottom": deepBottom})
	i.host = host
	rec := func(n uint32) uint32 {
		f, _ := i.export("rec")

		return f.Interface().(func(uintptr, uint32) uint32)(i.ctx(), n)
	}

	for _, n := range []uint32{0, 10, 100000} {
		if got := rec(n); got != n+14 {
			t.Fatalf("rec(%d) = %d, want %d", n, got, n+14)
		}
	}

	if host.calls != 3 {
		t.Fatalf("bottom called %d times", host.calls)
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

// A function whose calls are all on one path checks its stack where that
// path begins rather than on entry: recursion through it grows the stack
// there, the values the body holds in registers intact, and recursion
// without end traps there.
func TestCheckBeforeCalls(t *testing.T) {
	m := &testModule{
		funcs: []testFunc{
			// rec(n, k) = n == 0 ? k : rec(n-1, 3k+1) + n
			{params: []ValType{I32, I64}, results: []ValType{I64}, export: "rec",
				code: cat(get(0), []byte{opI32Eqz, opIf, byte(I64)}, get(1), []byte{opElse},
					get(0), i32c(1), []byte{opI32Sub}, get(1), i64c(3), []byte{opI64Mul}, i64c(1), []byte{opI64Add}, call(0),
					get(0), []byte{opI64ExtendU, opI64Add, 0x0b})},
			// inf(n) = n == -1 ? 0 : inf(n+1)
			{params: i32, results: i32, export: "inf",
				code: cat(get(0), i32c(1), []byte{opI32Add, opI32Eqz, opIf, byte(I32)}, i32c(0), []byte{opElse},
					get(0), i32c(1), []byte{opI32Add}, call(1), []byte{0x0b})},
		},
	}
	i := load(t, m, nil)
	f, _ := i.export("rec")
	rec := f.Interface().(func(uintptr, uint32, uint64) uint64)

	for _, n := range []uint32{0, 1, 1000, 100000} {
		want := uint64(5)

		for range n {
			want = want*3 + 1
		}

		want += uint64(n) * uint64(n+1) / 2

		if got := rec(i.ctx(), n, 5); got != want {
			t.Fatalf("rec(%d) = %d, want %d", n, got, want)
		}
	}

	f, _ = i.export("inf")

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

// wideImport is the import of TestWideSignatures: per kind of result, a
// sum over the arguments of that kind and a fingerprint of their order.
func wideImport(ctx uintptr, a0 uint32, a1 uint64, a2 float32, a3 float64, a4 uint32, a5 uint64, a6 float32, a7 float64, a8 uint32, a9 uint64, a10 float32, a11 float64, a12 uint32, a13 uint64, a14 float32, a15 float64, a16 uint32, a17 uint64, a18 float32, a19 float64) (uint64, uint32, float64, uint64, uint64, uint64, uint64, uint64, uint64, uint32, uint64, float32) {
	var ints, order uint64
	var floats float64

	ints += uint64(a0)
	order = order*31 + uint64(a0)
	ints += a1
	order = order*37 + a1
	floats += float64(a2)
	floats += a3 * 3
	ints += uint64(a4)
	order = order*31 + uint64(a4)
	ints += a5
	order = order*37 + a5
	floats += float64(a6)
	floats += a7 * 7
	ints += uint64(a8)
	order = order*31 + uint64(a8)
	ints += a9
	order = order*37 + a9
	floats += float64(a10)
	floats += a11 * 11
	ints += uint64(a12)
	order = order*31 + uint64(a12)
	ints += a13
	order = order*37 + a13
	floats += float64(a14)
	floats += a15 * 15
	ints += uint64(a16)
	order = order*31 + uint64(a16)
	ints += a17
	order = order*37 + a17
	floats += float64(a18)
	floats += a19 * 19

	return order, uint32(order + 1*ints), floats + 2, order + 3*ints, order + 4*ints, order + 5*ints, order + 6*ints, order + 7*ints, order + 8*ints, uint32(order + 9*ints), order + 10*ints, float32(floats)
}

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
	i := load(t, m, testImports{"env.wide": wideImport})
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
	want := reflect.ValueOf(wideImport).Call(args)

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

const (
	opDrop        = 0x1a
	opI64ExtendU  = 0xad
	jumpTestBuf   = 16
	jumpTestValue = 7
)

func put(i int) []byte {
	return leb([]byte{opLocalSet}, uint64(i))
}

// jumpTest is a module whose imports are setjmp, longjmp, bottom and
// callback, functions 0 to 3, with jumps keyed by the buffer address.
type jumpTest struct {
	t                *testing.T
	jumps            map[uint32]*Jump
	failed           int
	i                *Instance
	bottom, callback func()
}

var jumpImports = []testImport{
	{"env", "setjmp", i32, i32},
	{"env", "longjmp", []ValType{I32, I32}, nil},
	{"env", "bottom", nil, nil},
	{"env", "callback", nil, nil},
}

func jumpSetjmp(ctx uintptr, buf uint32) uint32 {
	jt := instanceAt(ctx).host.(*jumpTest)
	j := new(Jump)

	if !instanceAt(ctx).setjmp(j) {
		jt.t.Error("setjmp without a jump frame")
	}

	jt.jumps[buf] = j

	return 0
}

func jumpLongjmp(ctx uintptr, buf, val uint32) {
	jt := instanceAt(ctx).host.(*jumpTest)
	instanceAt(ctx).longjmp(jt.jumps[buf], val)
	jt.failed++
}

func jumpBottom(ctx uintptr) {
	instanceAt(ctx).host.(*jumpTest).bottom()
}

func jumpCallback(ctx uintptr) {
	instanceAt(ctx).host.(*jumpTest).callback()
}

func loadJumps(t *testing.T, funcs []testFunc, bottom, callback func()) *jumpTest {
	jt := &jumpTest{t: t, jumps: map[uint32]*Jump{}, bottom: bottom, callback: callback}
	jt.i = load(t, &testModule{imports: jumpImports, funcs: funcs}, testImports{
		"env.setjmp": jumpSetjmp, "env.longjmp": jumpLongjmp, "env.bottom": jumpBottom, "env.callback": jumpCallback,
	})
	jt.i.host = jt

	return jt
}

func (jt *jumpTest) export(name string) reflect.Value {
	f, _ := jt.i.export(name)

	return f
}

// run(n, depth) keeps six values of n across setjmp, more than R10-R12
// hold, and goes depth frames down, each keeping three values across its
// call, before bottom runs and longjmp returns from setjmp again with 7.
var jumpRun = func() testFunc {
	var code []byte

	for k := range 6 {
		code = cat(code, get(0), i64c(int64(k+3)), []byte{opI64Mul}, i64c(int64(k*1000+1)), []byte{opI64Add}, put(2+k))
	}

	code = cat(code, i32c(jumpTestBuf), call(0), put(8),
		get(8), []byte{opI32Eqz, opIf, blockEmpty}, get(1), call(5), []byte{opDrop, opUnreachable, 0x0b},
		get(2), get(3), []byte{opI64Add}, get(4), []byte{opI64Add}, get(5), []byte{opI64Add}, get(6), []byte{opI64Add},
		get(7), []byte{opI64Add}, get(8), []byte{opI64ExtendU, opI64Add})

	return testFunc{params: []ValType{I64, I32}, results: []ValType{I64}, export: "run",
		locals: []ValType{I64, I64, I64, I64, I64, I64, I32}, code: code}
}()

var jumpDeep = testFunc{
	params: i32, results: i32, locals: []ValType{I32, I32, I32},
	code: cat(get(0), []byte{opI32Eqz, opIf, blockEmpty}, call(2), i32c(jumpTestBuf), i32c(jumpTestValue), call(1), []byte{opUnreachable, 0x0b},
		get(0), i32c(7), []byte{opI32Add}, put(1), get(0), i32c(11), []byte{opI32Add}, put(2), get(0), i32c(13), []byte{opI32Add}, put(3),
		get(0), i32c(1), []byte{opI32Sub}, call(5), get(1), []byte{opI32Add}, get(2), []byte{opI32Add}, get(3), []byte{opI32Add}),
}

func jumpWant(n uint64) uint64 {
	sum := uint64(jumpTestValue)

	for k := range uint64(6) {
		sum += n*(k+3) + k*1000 + 1
	}

	return sum
}

// longjmp from 100000 frames down, after Go collected, grew its own stack
// and parked: the stack was copied between setjmp and longjmp, and the
// values setjmp's caller kept come back.
func TestLongjmp(t *testing.T) {
	jt := loadJumps(t, []testFunc{jumpRun, jumpDeep}, func() {
		runtime.GC()
		growGo(2000)
		done := make(chan bool)

		go func() {
			time.Sleep(time.Millisecond)
			done <- true
		}()

		<-done
	}, nil)
	run := jt.export("run").Interface().(func(uintptr, uint64, uint32) uint64)

	for _, depth := range []uint32{0, 1, 10, 100000} {
		n := uint64(depth)*977 + 5

		if got := run(jt.i.ctx(), n, depth); got != jumpWant(n) {
			t.Fatalf("run(%d, %d) = %d, want %d", n, depth, got, jumpWant(n))
		}
	}

	if jt.failed != 0 {
		t.Fatalf("%d jumps failed", jt.failed)
	}

	runtime.KeepAlive(jt.i)
}

// Jumps while other goroutines collect: Go preempts the code anywhere, the
// land stub included, and scans the stack it is on.
func TestLongjmpUnderCollection(t *testing.T) {
	jt := loadJumps(t, []testFunc{jumpRun, jumpDeep}, func() {}, nil)
	run := jt.export("run").Interface().(func(uintptr, uint64, uint32) uint64)
	stop := make(chan bool)
	defer close(stop)

	for range 2 {
		go func() {
			for {
				select {
				case <-stop:
					return
				default:
					runtime.GC()
				}
			}
		}()
	}

	var stats runtime.MemStats
	runtime.ReadMemStats(&stats)
	collections := stats.NumGC
	end := time.Now().Add(300 * time.Millisecond)

	for k := uint32(0); time.Now().Before(end); k++ {
		n := uint64(k)

		if got := run(jt.i.ctx(), n, k%50); got != jumpWant(n) {
			t.Fatalf("run(%d, %d) = %d, want %d", n, k%50, got, jumpWant(n))
		}
	}

	runtime.ReadMemStats(&stats)

	if stats.NumGC-collections < 10 {
		t.Fatalf("only %d collections", stats.NumGC-collections)
	}

	runtime.KeepAlive(jt.i)
}

// longjmp from native code Go called from native code, as from a signal
// handler: the jump drops the Go frames between.
func TestLongjmpThroughGo(t *testing.T) {
	var jt *jumpTest
	jt = loadJumps(t, []testFunc{
		{results: i32, locals: i32, export: "run",
			code: cat(i32c(32), call(0), []byte{opLocalTee, 0, opI32Eqz, opIf, blockEmpty}, call(3), []byte{opUnreachable, 0x0b}, get(0))},
		{export: "inner", code: cat(i32c(32), i32c(9), call(1))},
	}, nil, func() {
		inner := jt.export("inner").Interface().(func(uintptr))
		growGo(100)
		inner(jt.i.ctx())
		t.Error("inner returned")
	})
	run := jt.export("run").Interface().(func(uintptr) uint32)

	for range 3 {
		if got := run(jt.i.ctx()); got != 9 {
			t.Fatalf("run = %d", got)
		}

		runtime.GC()
	}

	runtime.KeepAlive(jt.i)
}

// A jmp_buf whose frame returned lands nowhere, though a frame of the same
// function, then of another, takes its place.
func TestStaleJump(t *testing.T) {
	jt := loadJumps(t, []testFunc{
		{params: i32, results: i32, export: "arm",
			code: cat(get(0), []byte{opI32Eqz, opIf, byte(I32)}, i32c(48), call(0), []byte{opElse}, i32c(48), i32c(5), call(1), i32c(1), []byte{0x0b})},
		{results: i32, export: "other", code: cat(i32c(48), i32c(5), call(1), i32c(2))},
	}, nil, nil)
	arm := jt.export("arm").Interface().(func(uintptr, uint32) uint32)
	other := jt.export("other").Interface().(func(uintptr) uint32)

	if got := arm(jt.i.ctx(), 0); got != 0 {
		t.Fatalf("arm(0) = %d", got)
	}

	if got := arm(jt.i.ctx(), 1); got != 1 || jt.failed != 1 {
		t.Fatalf("arm(1) = %d, %d failed", got, jt.failed)
	}

	if got := other(jt.i.ctx()); got != 2 || jt.failed != 2 {
		t.Fatalf("other() = %d, %d failed", got, jt.failed)
	}

	runtime.KeepAlive(jt.i)
}

const opReturn = 0x0f

// forkHost is what forkTest's imports keep, in the parent and the child.
type forkHost struct {
	t       *testing.T
	imports testImports
	jumps   map[uint32]*Jump
	done    chan uint64
}

func forkSetjmp(ctx uintptr, buf uint32) uint32 {
	i := instanceAt(ctx)
	j := new(Jump)
	i.setjmp(j)
	i.host.(*forkHost).jumps[buf] = j

	return 0
}

func forkLongjmp(ctx uintptr, buf, val uint32) {
	i := instanceAt(ctx)
	h := i.host.(*forkHost)
	i.longjmp(h.jumps[buf], val)
	h.t.Error("the jump failed")
}

func forkFork(ctx uintptr) uint32 {
	i := instanceAt(ctx)
	h := i.host.(*forkHost)
	j := new(Jump)
	i.setjmp(j)
	f, ok := i.frames(j, "main")

	if !ok {
		h.t.Error("no frames to fork")

		return 0
	}

	c := i.clone(h.imports, 64<<20, 0, i.memory.pages*pageSize)
	c.host = h

	go func() {
		h.done <- c.resume(f)
		c.release()
	}()

	return 77
}

// forkTest runs main(n, depth) of a module whose imports are setjmp,
// longjmp and fork: main keeps six values across setjmp and goes depth
// frames down to fork, the child stores 42 at 0 and longjmps to main's
// setjmp with 5. fork returns 77 to the parent and runs the child on a
// goroutine of its own, in a clone.
func forkTest(t *testing.T, imports testImports, done chan uint64) *Instance {
	main := jumpRun
	main.code = nil

	for k := range 6 {
		main.code = cat(main.code, get(0), i64c(int64(k+3)), []byte{opI64Mul}, i64c(int64(k*1000+1)), []byte{opI64Add}, put(2+k))
	}

	sum := cat(get(2), get(3), []byte{opI64Add}, get(4), []byte{opI64Add}, get(5), []byte{opI64Add}, get(6), []byte{opI64Add}, get(7), []byte{opI64Add})
	main.code = cat(main.code, i32c(jumpTestBuf), call(0), put(8),
		get(8), []byte{opIf, blockEmpty}, sum, get(8), []byte{opI64ExtendU, opI64Add, opReturn, 0x0b},
		sum, get(1), call(4), []byte{opI64ExtendU, opI64Add})
	main.export = "main"
	down := testFunc{
		params: i32, results: i32, locals: []ValType{I32, I32, I32, I32},
		code: cat(get(0), []byte{opI32Eqz, opIf, byte(I32)},
			call(2), []byte{opLocalTee, 4, opI32Eqz, opIf, blockEmpty}, i32c(0), i32c(42), []byte{opI32Store, 2, 0},
			i32c(jumpTestBuf), i32c(5), call(1), []byte{opUnreachable, 0x0b}, get(4),
			[]byte{opElse}, get(0), i32c(7), []byte{opI32Add}, put(1), get(0), i32c(11), []byte{opI32Add}, put(2), get(0), i32c(13), []byte{opI32Add}, put(3),
			get(0), i32c(1), []byte{opI32Sub}, call(4), get(1), []byte{opI32Add}, get(2), []byte{opI32Add}, get(3), []byte{opI32Add}, []byte{0x0b}),
	}
	imports["env.setjmp"], imports["env.longjmp"], imports["env.fork"] = forkSetjmp, forkLongjmp, forkFork
	i := load(t, &testModule{
		imports: []testImport{{"env", "setjmp", i32, i32}, {"env", "longjmp", []ValType{I32, I32}, nil}, {"env", "fork", nil, i32}},
		funcs:   []testFunc{main, down}, memMin: 1, memMax: 1,
	}, imports)
	i.host = &forkHost{t: t, imports: imports, jumps: map[uint32]*Jump{}, done: done}

	return i
}

// fork from 10000 frames down: the child's frames are copies, its memory
// too, and its jump to a setjmp of the parent's lands.
func TestFork(t *testing.T) {
	done := make(chan uint64, 1)
	i := forkTest(t, testImports{}, done)
	run, _ := i.export("main")

	for _, depth := range []uint32{0, 3, 10000} {
		n := uint64(depth) + 9
		base := jumpWant(n) - jumpTestValue
		want := base + 77

		for d := uint64(1); d <= uint64(depth); d++ {
			want += 3*d + 31
		}

		if got := run.Interface().(func(uintptr, uint64, uint32) uint64)(i.ctx(), n, depth); got != want {
			t.Fatalf("parent at depth %d: %d, want %d", depth, got, want)
		}

		if got := <-done; got != base+5 {
			t.Fatalf("child at depth %d: %d, want %d", depth, got, base+5)
		}

		if got := binary.LittleEndian.Uint32(i.memory.bytes()); got != 0 {
			t.Fatalf("the child wrote %d to the parent's memory", got)
		}
	}

	runtime.KeepAlive(i)
}

// Forks while other goroutines collect, the children landing as the
// parents go on.
func TestForkUnderCollection(t *testing.T) {
	done := make(chan uint64, 1)
	i := forkTest(t, testImports{}, done)
	run, _ := i.export("main")
	stop := make(chan bool)
	defer close(stop)

	go func() {
		for {
			select {
			case <-stop:
				return
			default:
				runtime.GC()
			}
		}
	}()

	end := time.Now().Add(300 * time.Millisecond)

	for k := uint32(0); time.Now().Before(end); k++ {
		n := uint64(k)
		base := jumpWant(n) - jumpTestValue
		run.Interface().(func(uintptr, uint64, uint32) uint64)(i.ctx(), n, k%40)

		if got := <-done; got != base+5 {
			t.Fatalf("child %d: %d, want %d", k, got, base+5)
		}
	}

	runtime.KeepAlive(i)
}

// interruptHost counts the interruptions of its instance's code.
type interruptHost struct {
	calls atomic.Int64
}

func (h *interruptHost) interrupt() {
	h.calls.Add(1)
}

// A loop without calls, interrupted over and over while Go collects, keeps
// its registers and flags: the interrupt stub restores them, the host runs
// in between.
func TestInterrupt(t *testing.T) {
	const opI64Xor, opI64Sub, opI64GtU, opBrIfLoop = 0x85, 0x7d, 0x56, 0x0d
	m := &testModule{
		funcs: []testFunc{{
			params: []ValType{I64}, results: []ValType{I64}, locals: []ValType{I64, I64}, export: "mix",
			// acc = acc*31 ^ i, i from n down to 1
			code: cat([]byte{opLoop, blockEmpty},
				get(1), i64c(31), []byte{opI64Mul}, get(0), []byte{opI64Xor}, put(1),
				get(0), i64c(1), []byte{opI64Sub}, []byte{opLocalTee, 0}, i64c(0), []byte{opI64GtU, opBrIfLoop, 0, 0x0b},
				get(1)),
		}},
	}
	i := load(t, m, nil)
	host := &interruptHost{}
	i.host = host
	funcs, _ := i.functionCode()
	mix, _ := i.export("mix")
	const n = 300_000_000
	want := uint64(0)

	for k := uint64(n); k > 0; k-- {
		want = want*31 ^ k
	}

	stop := make(chan bool)
	defer close(stop)

	go func() {
		for {
			select {
			case <-stop:
				return
			default:
				runtime.GC()
			}
		}
	}()

	tokens := make(chan uintptr)
	result := make(chan uint64)

	go func() {
		tokens <- runtime.SetInterrupt(uintptr(funcs[0]), uintptr(funcs[1]), interruptStub())
		result <- mix.Interface().(func(uintptr, uint64) uint64)(i.ctx(), n)
		runtime.SetInterrupt(0, 0, 0)
	}()

	token := <-tokens

	for {
		select {
		case got := <-result:
			if got != want {
				t.Fatalf("mix = %#x, want %#x", got, want)
			}

			if host.calls.Load() == 0 {
				t.Fatal("the loop was never interrupted")
			}

			runtime.KeepAlive(i)

			return
		default:
			runtime.Interrupt(token)
			time.Sleep(50 * time.Microsecond)
		}
	}
}

// returnHost is interrupted on the way out of imports: it collects and
// grows the stack through the adapter's frame, then clears the word.
type returnHost struct {
	i     *Instance
	calls int
	ticks uint32
}

func (h *returnHost) interrupt() {
	h.calls++
	runtime.GC()
	growGo(200)
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&h.i.block[offInterrupt])), 0)
}

// returnTick asks for an interruption on every 7th call.
func returnTick(ctx uintptr) uint32 {
	h := instanceAt(ctx).host.(*returnHost)
	h.ticks++

	if h.ticks%7 == 0 {
		atomic.StoreUint32((*uint32)(unsafe.Pointer(&instanceAt(ctx).block[offInterrupt])), 1)
	}

	return h.ticks
}

// Every 7th call of an import asks for an interruption, which runs on the
// way out of the import, its result kept.
func TestInterruptOnReturn(t *testing.T) {
	const opBrIfLoop = 0x0d
	host := &returnHost{}
	m := &testModule{
		imports: []testImport{{"env", "tick", nil, i32}},
		funcs: []testFunc{{
			params: i32, results: i32, locals: i32, export: "count",
			code: cat([]byte{opLoop, blockEmpty}, get(1), call(0), []byte{opI32Add}, put(1),
				get(0), i32c(1), []byte{opI32Sub, opLocalTee, 0, opBrIfLoop, 0, 0x0b}, get(1)),
		}},
	}
	i := load(t, m, testImports{"env.tick": returnTick})
	host.i = i
	i.host = host
	count, _ := i.export("count")

	if got, want := count.Interface().(func(uintptr, uint32) uint32)(i.ctx(), 1000), uint32(1000*1001/2); got != want {
		t.Fatalf("count = %d, want %d", got, want)
	}

	if host.calls != 1000/7 {
		t.Fatalf("%d interruptions, want %d", host.calls, 1000/7)
	}

	runtime.KeepAlive(i)
}

// A state machine as Go compiles to wasm: a loop that dispatches on a
// local, every case setting the next state and the locals, one as a copy
// of another. Threading sends each case straight to the next, and repair
// gives the cases the locals as parameters.
func TestThreadedStateMachine(t *testing.T) {
	const opBrTable, opBr, opI32LtS, opI32Ne = 0x0e, 0x0c, 0x48, 0x47
	// params: n; locals: state 1, a 2, b 3, i 4, t 5
	// t = a; a = b; b = t + b, then case 0 again while i < n, as Go does:
	// a constant state on each branch
	case0 := cat(get(2), put(5), get(3), put(2), get(5), get(3), []byte{opI32Add}, put(3),
		get(4), i32c(1), []byte{opI32Add, opLocalTee, 4}, get(0), []byte{opI32LtS, opIf, blockEmpty}, i32c(0), put(1), []byte{opBr, 3, 0x0b},
		i32c(1), put(1), []byte{opBr, 2})
	case1 := cat(get(2), get(3), []byte{opI32Ne}, []byte{opIf, blockEmpty}, get(3), put(2), []byte{0x0b}, i32c(2), put(1), []byte{opBr, 1})
	code := cat(i32c(0), put(2), i32c(1), put(3),
		[]byte{opLoop, blockEmpty, 0x02, blockEmpty, 0x02, blockEmpty, 0x02, blockEmpty},
		get(1), []byte{opBrTable, 2, 0, 1, 2, 0x0b}, case0, []byte{0x0b}, case1, []byte{0x0b},
		get(2), []byte{opReturn, 0x0b}, []byte{opUnreachable})
	m := &testModule{funcs: []testFunc{{params: i32, results: i32, locals: []ValType{I32, I32, I32, I32, I32}, export: "fib", code: code}}}
	i := load(t, m, nil)
	fib, _ := i.export("fib")

	for _, n := range []uint32{1, 2, 10, 30} {
		a, b := uint32(0), uint32(1)

		for k := uint32(0); k < n; k++ {
			a, b = b, a+b
		}

		if got := fib.Interface().(func(uintptr, uint32) uint32)(i.ctx(), n); got != b {
			t.Fatalf("fib(%d) = %d, want %d", n, got, b)
		}
	}

	runtime.KeepAlive(i)
}
