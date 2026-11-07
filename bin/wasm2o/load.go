//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"reflect"
	"syscall"
	"unsafe"
)

// The loader maps an object's text executable, fills its slots, tells Go
// about its code and makes an instance of it: the instance block, linear
// memory and tables, then the init thunk. An export is a Go function value
// whose code is its thunk: calling it runs the module on the caller's
// goroutine stack.

// stackLimit is how much goroutine stack an instance may take; deeper, it
// traps, before Go's own limit would end the program.
const stackLimit = 64 << 20

const pageSize = 65536

// Imports resolves what an instance imports.
type Imports interface {
	// function returns a Go function value of the import's type,
	// func(uintptr, params...) (results...), the first argument the
	// instance.
	function(imp *ImportMeta) any
	// global returns the address of an imported global's value.
	global(imp *ImportMeta) uintptr
	memory(imp *ImportMeta) *LinearMemory
	// table returns the address of a table's {entries, size, max}.
	table(imp *ImportMeta) uintptr
}

type Instance struct {
	name   string
	obj    *Object
	text   []byte
	base   uintptr
	block  []byte
	memory *LinearMemory
	tables [][]uintptr
	// keep holds the Go function values the code calls.
	keep []any
}

// LinearMemory is reserved at its maximum: it grows in place.
type LinearMemory struct {
	data  []byte
	pages uint64
	max   uint64
	// users are the instances whose headers describe it.
	users []*Instance
}

// Trap is the panic of a trapping instance.
type Trap struct {
	code uint32
}

var trapNames = map[uint32]string{
	trapUnreachable: "unreachable", trapMemoryOOB: "out of bounds memory access", trapIntOverflow: "integer overflow",
	trapDivByZero: "integer divide by zero", trapConversion: "invalid conversion to integer",
	trapIndirectType: "indirect call type mismatch", trapUndefinedElem: "undefined element",
	trapUninitElem: "uninitialized element", trapStack: "call stack exhausted", trapTableOOB: "out of bounds table access",
	trapNullRef: "null reference",
}

func (t *Trap) Error() string {
	return t.message()
}

func (t *Trap) message() string {
	if name, ok := trapNames[t.code]; ok {
		return "wasm trap: " + name
	}

	return fmt.Sprintf("wasm trap %d", t.code)
}

func mmap(size int, prot int) []byte {
	return throw2(syscall.Mmap(-1, 0, size, prot, syscall.MAP_PRIVATE|syscall.MAP_ANON|syscall.MAP_NORESERVE))
}

func newMemory(least, most uint64) *LinearMemory {
	if most > 65536 || least > most {
		throwFmt("memory of %d..%d pages", least, most)
	}

	m := &LinearMemory{data: mmap(int(max(most, 1)*pageSize), syscall.PROT_READ|syscall.PROT_WRITE), max: most}
	m.grow(uint32(least))

	return m
}

// grow adds pages and returns the old size, or MaxUint32 past the maximum.
func (m *LinearMemory) grow(delta uint32) uint32 {
	old := m.pages

	if old+uint64(delta) > m.max {
		return math.MaxUint32
	}

	m.pages += uint64(delta)

	for _, u := range m.users {
		u.setMemory()
	}

	return uint32(old)
}

func (m *LinearMemory) bytes() []byte {
	return m.data[:m.pages*pageSize]
}

func (i *Instance) ctx() uintptr {
	return uintptr(unsafe.Pointer(unsafe.SliceData(i.block)))
}

func (i *Instance) put64(at uint32, v uint64) {
	binary.LittleEndian.PutUint64(i.block[at:], v)
}

func (i *Instance) setMemory() {
	size := i.memory.pages * pageSize
	i.put64(offMem, uint64(uintptr(unsafe.Pointer(unsafe.SliceData(i.memory.data)))))
	i.put64(offMemSize, size)
	i.put64(offMemBound, uint64(int64(size)-boundSlack))
}

// instanceAt is the instance of a context the code passes: its header holds
// the address of the Go value, which the caller of the code keeps alive.
//
//go:nocheckptr
func instanceAt(ctx uintptr) *Instance {
	return *(**Instance)(unsafe.Pointer(ctx + offHost))
}

func helperTrap(ctx uintptr, code uint32) {
	panic(&Trap{code})
}

func helperMemoryGrow(ctx uintptr, delta uint32) uint32 {
	return instanceAt(ctx).memory.grow(delta)
}

// Tables do not grow.
func helperTableGrow(ctx uintptr, table, delta uint32, init uintptr) uint32 {
	return math.MaxUint32
}

// funcval returns a Go function value's closure: the address of its code
// pointer, followed by what it captured. Function values are pointer
// shaped, held in an interface's data word.
func funcval(f any) uintptr {
	if reflect.TypeOf(f).Kind() != reflect.Func {
		throwFmt("%T is not a function", f)
	}

	return (*[2]uintptr)(unsafe.Pointer(&f))[1]
}

func goType(t ValType) reflect.Type {
	switch t {
	case I32:
		return reflect.TypeFor[uint32]()
	case I64:
		return reflect.TypeFor[uint64]()
	case F32:
		return reflect.TypeFor[float32]()
	case F64:
		return reflect.TypeFor[float64]()
	case FuncRef, ExternRef:
		return reflect.TypeFor[uintptr]()
	}

	throwFmt("unsupported value type %s", typeName(t))

	return nil
}

// goFuncType is the Go type of a function of the module at the boundary:
// func(instance uintptr, params...) (results...).
func goFuncType(params, results []ValType) reflect.Type {
	in := []reflect.Type{reflect.TypeFor[uintptr]()}

	for _, t := range params {
		in = append(in, goType(t))
	}

	var out []reflect.Type

	for _, t := range results {
		out = append(out, goType(t))
	}

	return reflect.FuncOf(in, out, false)
}

func (i *Instance) keepFunc(f any) uintptr {
	i.keep = append(i.keep, f)

	return funcval(f)
}

func (i *Instance) resolve(sym string, imports Imports) uintptr {
	switch sym {
	case symMorestack:
		return goSymbol("runtime.morestack_noctxt")
	case symTrap:
		return i.keepFunc(helperTrap)
	case symMemoryGrow:
		return i.keepFunc(helperMemoryGrow)
	case symTableGrow:
		return i.keepFunc(helperTableGrow)
	}

	for k := range i.obj.meta.imports {
		imp := &i.obj.meta.imports[k]

		if imp.kind != externFunc || imp.sym != sym {
			continue
		}

		f := imports.function(imp)

		if f == nil {
			throwFmt("unresolved import %s.%s", imp.module, imp.name)
		}

		if want := goFuncType(imp.params, imp.results); reflect.TypeOf(f) != want {
			throwFmt("import %s.%s is %T, not %s", imp.module, imp.name, f, want)
		}

		return i.keepFunc(f)
	}

	throwFmt("unresolved symbol %s", sym)

	return 0
}

// load maps the text, fills the slots and registers the code with Go.
func (i *Instance) load(imports Imports) {
	obj := i.obj
	size := (len(obj.text) + 4095) &^ 4095
	i.text = mmap(max(size, 4096), syscall.PROT_READ|syscall.PROT_WRITE)
	copy(i.text, obj.text)
	i.base = uintptr(unsafe.Pointer(unsafe.SliceData(i.text)))

	for _, r := range obj.relocs {
		binary.LittleEndian.PutUint64(i.text[r.at:], uint64(i.resolve(r.sym, imports)))
	}

	throw(syscall.Mprotect(i.text, syscall.PROT_READ|syscall.PROT_EXEC))
	registerCode(i.name, i.base, uintptr(len(obj.text)), obj.meta.regions)
}

// instantiate makes an instance of an object; start runs its init.
func instantiate(name string, obj *Object, imports Imports) *Instance {
	meta := obj.meta
	i := &Instance{name: name, obj: obj, block: mmap(int(max(meta.instanceSize, headerSize)), syscall.PROT_READ|syscall.PROT_WRITE)}

	if uint64(meta.instanceSize) > uint64(len(i.block)) {
		throwFmt("instance of %d bytes", meta.instanceSize)
	}

	i.put64(offHost, uint64(uintptr(unsafe.Pointer(i))))
	i.put64(offStackLimit, stackLimit)

	for k := range meta.imports {
		imp := &meta.imports[k]

		switch imp.kind {
		case externMemory:
			i.memory = imports.memory(imp)

			if i.memory == nil {
				throwFmt("unresolved import %s.%s", imp.module, imp.name)
			}
		case externGlobal:
			at := imports.global(imp)

			if at == 0 {
				throwFmt("unresolved import %s.%s", imp.module, imp.name)
			}

			i.put64(meta.globals[imp.index].offset, uint64(at))
		case externTable:
			at := imports.table(imp)

			if at == 0 {
				throwFmt("unresolved import %s.%s", imp.module, imp.name)
			}

			copy(i.block[meta.tables[imp.index].offset:], unsafe.Slice((*byte)(unsafe.Pointer(at)), tableBytes))
		}
	}

	if meta.memory != nil && i.memory == nil {
		i.memory = newMemory(meta.memory.min, meta.memory.max)
	}

	if i.memory != nil {
		i.memory.users = append(i.memory.users, i)
		i.setMemory()
	}

	for _, t := range meta.tables {
		if t.imported {
			continue
		}

		if t.min > 1<<24 {
			throwFmt("table of %d elements", t.min)
		}

		entries := make([]uintptr, t.min+1)
		i.tables = append(i.tables, entries)
		i.put64(t.offset+tableEntries, uint64(uintptr(unsafe.Pointer(unsafe.SliceData(entries)))))
		binary.LittleEndian.PutUint32(i.block[t.offset+tableSize:], uint32(t.min))
		binary.LittleEndian.PutUint32(i.block[t.offset+tableMax:], uint32(min(t.max, math.MaxUint32)))
	}

	i.load(imports)

	return i
}

// nativeFunc is a Go function value of type typ whose code is at code.
func nativeFunc(typ reflect.Type, code uintptr) reflect.Value {
	closure := new(uintptr)
	*closure = code

	return reflect.NewAt(typ, unsafe.Pointer(&closure)).Elem()
}

func (i *Instance) symbol(name string) uintptr {
	at, ok := i.obj.defined[name]

	if !ok {
		throwFmt("no symbol %s", name)
	}

	return i.base + uintptr(at)
}

// start runs the init thunk: the segments and the start function.
func (i *Instance) start() {
	nativeFunc(reflect.TypeFor[func(uintptr)](), i.symbol(i.obj.meta.init)).Interface().(func(uintptr))(i.ctx())
}

// export returns an exported function as a Go function value,
// func(instance uintptr, params...) (results...).
func (i *Instance) export(name string) (reflect.Value, *ExportMeta) {
	for k := range i.obj.meta.exports {
		e := &i.obj.meta.exports[k]

		if e.name == name && e.kind == externFunc {
			return nativeFunc(goFuncType(e.params, e.results), i.symbol(e.sym)), e
		}
	}

	throwFmt("no exported function %s", name)

	return reflect.Value{}, nil
}
