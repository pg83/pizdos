//go:build linux && amd64

package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"unsafe"
)

// specCommand runs one test of the WebAssembly spec testsuite as wast2json
// writes it, `vm spec test.json`: every binary module is compiled and
// loaded in this process, linked with the spectest module and registered
// modules. An instantiation or an assertion on an action is a check; what
// cannot be compiled, linked or expressed is skipped, as are validation
// assertions. It prints every failure, then "RESULT passed failed skipped"
// and what was not compiled.

type specValue struct {
	Type  string          `json:"type"`
	Value json.RawMessage `json:"value"`
}

type specAction struct {
	Type   string      `json:"type"`
	Module string      `json:"module"`
	Field  string      `json:"field"`
	Args   []specValue `json:"args"`
}

type specCmd struct {
	Type       string      `json:"type"`
	Line       int         `json:"line"`
	Filename   string      `json:"filename"`
	Name       string      `json:"name"`
	As         string      `json:"as"`
	Action     *specAction `json:"action"`
	Expected   []specValue `json:"expected"`
	Text       string      `json:"text"`
	ModuleType string      `json:"module_type"`
}

// The trap messages of the testsuite, by the code of the trap.
var specTraps = []struct {
	prefix string
	code   uint32
}{
	{"unreachable", trapUnreachable}, {"out of bounds memory access", trapMemoryOOB},
	{"integer overflow", trapIntOverflow}, {"integer divide by zero", trapDivByZero},
	{"invalid conversion to integer", trapConversion}, {"indirect call type mismatch", trapIndirectType},
	{"undefined element", trapUndefinedElem}, {"uninitialized element", trapUninitElem},
	{"call stack exhausted", trapStack}, {"out of bounds table access", trapTableOOB},
}

const specStack = 8 << 20

type specRunner struct {
	dir        string
	current    *Instance
	named      map[string]*Instance
	registered map[string]*Instance
	// instances of modules that compiled and linked
	live                    map[*Instance]bool
	passed, failed, skipped int
	unsupported             map[string]bool
	// the spectest module's memory, table and globals
	testMemory  *LinearMemory
	testTable   []uintptr
	testHeader  *[2]uint64
	testGlobals map[string]*uint64
}

func specCommand(args []string) int {
	if len(args) != 1 {
		usage()
	}

	var file struct {
		Commands []specCmd `json:"commands"`
	}

	throw(json.Unmarshal(throw2(os.ReadFile(args[0])), &file))
	r := &specRunner{dir: filepath.Dir(args[0]), named: map[string]*Instance{}, registered: map[string]*Instance{}, live: map[*Instance]bool{}, unsupported: map[string]bool{}}
	r.spectest()

	for i := range file.Commands {
		r.command(&file.Commands[i])
	}

	fmt.Printf("RESULT %d %d %d\n", r.passed, r.failed, r.skipped)
	var notes []string

	for n := range r.unsupported {
		notes = append(notes, n)
	}

	sort.Strings(notes)

	for _, n := range notes {
		fmt.Printf("UNSUPPORTED %s\n", n)
	}

	runtime.KeepAlive(r)

	return 0
}

func (r *specRunner) fail(line int, format string, args ...any) {
	r.failed++
	fmt.Printf("FAIL line %d: %s\n", line, fmt.Sprintf(format, args...))
}

// spectest is the module the testsuite imports from: print functions that
// do nothing, globals of 666, a memory of one to two pages, a table of 10 to
// 20 elements.
func (r *specRunner) spectest() {
	r.testMemory = newMemory(1, 2, false)
	r.testTable = make([]uintptr, 21)
	r.testHeader = &[2]uint64{uint64(uintptr(unsafe.Pointer(&r.testTable[0]))), 10 | 20<<32}
	cell := func(bits uint64) *uint64 {
		return &bits
	}
	r.testGlobals = map[string]*uint64{
		"global_i32": cell(666), "global_i64": cell(666),
		"global_f32": cell(uint64(math.Float32bits(666.6))), "global_f64": cell(math.Float64bits(666.6)),
	}
}

func (r *specRunner) exporter(module string) *Instance {
	return r.registered[module]
}

func findExport(i *Instance, name string, kind byte) *ExportMeta {
	for k := range i.obj.meta.exports {
		if e := &i.obj.meta.exports[k]; e.name == name && e.kind == kind {
			return e
		}
	}

	return nil
}

func sameTypes(a, b []ValType) bool {
	return reflect.DeepEqual(append([]ValType{}, a...), append([]ValType{}, b...))
}

// spectest's functions print nothing.
var spectestFunctions = map[string]any{
	"print":         func(ctx uintptr) {},
	"print_i32":     func(ctx uintptr, x uint32) {},
	"print_i64":     func(ctx uintptr, x uint64) {},
	"print_f32":     func(ctx uintptr, x float32) {},
	"print_f64":     func(ctx uintptr, x float64) {},
	"print_i32_f32": func(ctx uintptr, x uint32, y float32) {},
	"print_f64_f64": func(ctx uintptr, x, y float64) {},
}

// function binds an import of another module to that module's export
// thunk with its instance, which the thunk takes as an import takes it.
func (r *specRunner) function(imp *ImportMeta) any {
	if imp.module == "spectest" {
		return spectestFunctions[imp.name]
	}

	src := r.exporter(imp.module)

	if src == nil {
		return nil
	}

	e := findExport(src, imp.name, externFunc)

	if e == nil || !sameTypes(e.params, imp.params) || !sameTypes(e.results, imp.results) {
		return nil
	}

	return bindContext(src.symbol(e.sym), src.ctx())
}

// globalAddr is where an instance keeps the value of one of its globals.
func globalAddr(i *Instance, index uint32) uintptr {
	g := i.obj.meta.globals[index]
	at := i.ctx() + uintptr(g.offset)

	if g.imported {
		return uintptr(*(*uint64)(unsafe.Pointer(at)))
	}

	return at
}

func (r *specRunner) global(imp *ImportMeta) uintptr {
	if imp.module == "spectest" {
		if g := r.testGlobals[imp.name]; g != nil {
			return uintptr(unsafe.Pointer(g))
		}

		return 0
	}

	src := r.exporter(imp.module)

	if src == nil {
		return 0
	}

	e := findExport(src, imp.name, externGlobal)

	if e == nil {
		return 0
	}

	return globalAddr(src, e.index)
}

func (r *specRunner) memory(imp *ImportMeta) *LinearMemory {
	if imp.module == "spectest" && imp.name == "memory" {
		return r.testMemory
	}

	src := r.exporter(imp.module)

	if src == nil || findExport(src, imp.name, externMemory) == nil {
		return nil
	}

	return src.memory
}

func (r *specRunner) table(imp *ImportMeta) uintptr {
	if imp.module == "spectest" && imp.name == "table" {
		return uintptr(unsafe.Pointer(r.testHeader))
	}

	src := r.exporter(imp.module)

	if src == nil {
		return 0
	}

	e := findExport(src, imp.name, externTable)

	if e == nil {
		return 0
	}

	return src.ctx() + uintptr(src.obj.meta.tables[e.index].offset)
}

// Spec assertions always select exact Wasm bounds in the embedded compiler.
func specCompile(data []byte) []byte {
	return compileModuleWithOptions(data, CompileOptions{})
}

// load compiles and instantiates a module file, or returns nil, skipped.
func (r *specRunner) load(filename string) *Instance {
	var i *Instance

	if e := try(func() {
		data := throw2(os.ReadFile(filepath.Join(r.dir, filename)))
		// Spec assertions require exact Wasm bounds, independently of
		// the ordinary compiler's padded memory semantics.
		obj := readObject(specCompile(data))
		i = instantiate(filename, obj, r, specStack)
	}); e != nil {
		r.unsupported[e.message()] = true

		return nil
	}

	return i
}

// trapped runs f and returns the code of its trap, or -1.
func trapped(f func()) (code int) {
	defer func() {
		v := recover()

		if t, ok := v.(*Trap); ok {
			code = int(t.code)

			return
		}

		if v != nil {
			panic(v)
		}
	}()

	f()

	return -1
}

func (r *specRunner) command(c *specCmd) {
	switch c.Type {
	case "module":
		r.current = nil

		if c.ModuleType == "text" {
			r.skipped++

			return
		}

		i := r.load(c.Filename)

		if c.Name != "" {
			r.named[c.Name] = i
		}

		if i == nil {
			r.skipped++

			return
		}

		r.current = i

		if code := trapped(i.start); code >= 0 {
			r.fail(c.Line, "instantiation trapped: %d", code)

			return
		}

		r.live[i] = true
		r.passed++
	case "register":
		i := r.current

		if c.Name != "" {
			i = r.named[c.Name]
		}

		if i != nil && r.live[i] {
			r.registered[c.As] = i
		}
	case "assert_return", "action":
		r.assertReturn(c)
	case "assert_trap", "assert_exhaustion":
		r.assertTrap(c)
	case "assert_uninstantiable":
		i := r.load(c.Filename)

		if i == nil {
			r.skipped++

			return
		}

		if trapped(i.start) < 0 {
			r.fail(c.Line, "instantiation did not trap")

			return
		}

		r.passed++
	default:
		r.skipped++
	}
}

func (r *specRunner) moduleOf(a *specAction) *Instance {
	i := r.current

	if a.Module != "" {
		i = r.named[a.Module]
	}

	if i == nil || !r.live[i] {
		return nil
	}

	return i
}

// specArg converts an argument; ok is false for one the loader cannot pass.
func specArg(v specValue) (reflect.Value, bool) {
	var s string

	if json.Unmarshal(v.Value, &s) != nil {
		return reflect.Value{}, false
	}

	if v.Type == "funcref" || v.Type == "externref" {
		if s == "null" {
			return reflect.ValueOf(uintptr(0)), true
		}

		x, err := strconv.ParseUint(s, 10, 64)

		return reflect.ValueOf(uintptr(x + 1)), err == nil
	}

	x, err := strconv.ParseUint(s, 10, 64)

	if err != nil {
		return reflect.Value{}, false
	}

	switch v.Type {
	case "i32":
		return reflect.ValueOf(uint32(x)), true
	case "i64":
		return reflect.ValueOf(x), true
	case "f32":
		return reflect.ValueOf(math.Float32frombits(uint32(x))), true
	case "f64":
		return reflect.ValueOf(math.Float64frombits(x)), true
	}

	return reflect.Value{}, false
}

// invoke prepares an action: a call of an export or a read of a global,
// returning the values it produces; ok is false when it cannot be run.
func (r *specRunner) invoke(a *specAction) (func() []reflect.Value, bool) {
	if a == nil {
		return nil, false
	}

	i := r.moduleOf(a)

	if i == nil {
		return nil, false
	}

	if a.Type == "get" {
		e := findExport(i, a.Field, externGlobal)

		if e == nil {
			return nil, false
		}

		t := i.obj.meta.globals[e.index].typ

		return func() []reflect.Value {
			bits := *(*uint64)(unsafe.Pointer(globalAddr(i, e.index)))

			return []reflect.Value{specBits(t, bits)}
		}, true
	}

	e := findExport(i, a.Field, externFunc)

	if e == nil {
		return nil, false
	}

	for _, t := range append(append([]ValType{}, e.params...), e.results...) {
		if t == V128 {
			return nil, false
		}
	}

	f, _ := i.export(e.name)
	args := []reflect.Value{reflect.ValueOf(i.ctx())}

	if len(a.Args) != len(e.params) {
		return nil, false
	}

	for k, v := range a.Args {
		x, ok := specArg(v)

		if !ok || x.Type() != goType(e.params[k]) {
			return nil, false
		}

		args = append(args, x)
	}

	return func() []reflect.Value {
		out := f.Call(args)
		runtime.KeepAlive(i)

		return out
	}, true
}

func specBits(t ValType, bits uint64) reflect.Value {
	switch t {
	case I32:
		return reflect.ValueOf(uint32(bits))
	case F32:
		return reflect.ValueOf(math.Float32frombits(uint32(bits)))
	case F64:
		return reflect.ValueOf(math.Float64frombits(bits))
	case FuncRef, ExternRef:
		return reflect.ValueOf(uintptr(bits))
	}

	return reflect.ValueOf(bits)
}

// matches compares a result with an expected value, NaN patterns included.
func matches(got reflect.Value, want specValue) (bool, string) {
	var bits uint64

	switch x := got.Interface().(type) {
	case uint32:
		bits = uint64(x)
	case uint64:
		bits = x
	case float32:
		bits = uint64(math.Float32bits(x))
	case float64:
		bits = math.Float64bits(x)
	case uintptr:
		bits = uint64(x)
	}

	shown := fmt.Sprintf("%s %#x", want.Type, bits)
	var s string

	if len(want.Value) == 0 {
		// a non-null reference of the type
		return bits != 0, shown
	}

	if json.Unmarshal(want.Value, &s) != nil {
		return false, shown
	}

	switch {
	case s == "nan:canonical" && want.Type == "f32":
		return bits&0x7fffffff == 0x7fc00000, shown
	case s == "nan:canonical":
		return bits&0x7fffffffffffffff == 0x7ff8000000000000, shown
	case s == "nan:arithmetic" && want.Type == "f32":
		return bits&0x7fc00000 == 0x7fc00000, shown
	case s == "nan:arithmetic":
		return bits&0x7ff8000000000000 == 0x7ff8000000000000, shown
	case s == "null":
		return bits == 0, shown
	}

	x, err := strconv.ParseUint(s, 10, 64)

	if err != nil {
		return false, shown
	}

	if want.Type == "funcref" || want.Type == "externref" {
		x++
	}

	return bits == x, shown
}

func (r *specRunner) assertReturn(c *specCmd) {
	run, ok := r.invoke(c.Action)

	if !ok {
		r.skipped++

		return
	}

	for _, v := range c.Expected {
		if v.Type == "v128" {
			r.skipped++

			return
		}
	}

	var out []reflect.Value

	if code := trapped(func() { out = run() }); code >= 0 {
		r.fail(c.Line, "trap %d", code)

		return
	}

	if c.Type == "assert_return" {
		if len(out) != len(c.Expected) {
			r.fail(c.Line, "%d results, expected %d", len(out), len(c.Expected))

			return
		}

		for k := range out {
			if ok, shown := matches(out[k], c.Expected[k]); !ok {
				r.fail(c.Line, "result %d is %s, expected %s", k, shown, strings.Trim(string(c.Expected[k].Value), `"`))

				return
			}
		}
	}

	r.passed++
}

func (r *specRunner) assertTrap(c *specCmd) {
	if c.Action == nil {
		r.skipped++

		return
	}

	run, ok := r.invoke(c.Action)

	if !ok {
		r.skipped++

		return
	}

	want := -1

	if c.Type == "assert_exhaustion" {
		want = trapStack
	} else {
		for _, t := range specTraps {
			if strings.HasPrefix(c.Text, t.prefix) {
				want = int(t.code)
			}
		}
	}

	code := trapped(func() { run() })

	switch {
	case code < 0:
		r.fail(c.Line, "no trap")
	case want >= 0 && code != want:
		r.fail(c.Line, "trap %d, expected %d", code, want)
	default:
		r.passed++
	}
}
