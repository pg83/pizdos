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

// The loader links an object's code once, executable, with its GOT filled,
// and tells Go about it; instances of the object share that code. An
// instance is the instance block, linear memory and tables, filled by the
// init thunk. An export is a Go function value whose code is its thunk:
// calling it runs the module on the caller's goroutine stack.

// runStack is how much goroutine stack a command may take; deeper, it
// traps, before Go's own limit of 1 GiB would end the program.
const runStack = 64 << 20

const pageSize = 65536

// Imports resolves what an instance imports.
type Imports interface {
	// function returns a Go function of the import's type,
	// func(uintptr, params...) (results...), no closure, the first argument
	// the instance; or the address of code of that convention, a stub of
	// bindContext, say.
	function(imp *ImportMeta) any
	// global returns the address of an imported global's value.
	global(imp *ImportMeta) uintptr
	memory(imp *ImportMeta) *LinearMemory
	// table returns the address of a table's {entries, size, max}.
	table(imp *ImportMeta) uintptr
}

// Code is an object linked in memory: the text, then .rodata and the GOT.
// The code reaches its instance through the context, and the Go functions
// of the GOT take the instance first, so every instance of the object runs
// the same code.
type Code struct {
	name string
	obj  *Object
	text []byte
	base uintptr
	// keep holds the Go function values the code calls.
	keep []any
}

// bindContext returns code of Go's convention that calls code with ctx as
// its first argument, the others as they are: an export thunk of another
// instance with that instance, say. The stub lives as long as the process.
//
//	movabs $ctx, %rax
//	jmp *target(%rip)
//	target: .quad code
func bindContext(code, ctx uintptr) uintptr {
	stub := mmap(4096, syscall.PROT_READ|syscall.PROT_WRITE)
	stub = append(stub[:0], 0x48, 0xb8)
	stub = binary.LittleEndian.AppendUint64(stub, uint64(ctx))
	stub = append(stub, 0xff, 0x25, 0, 0, 0, 0)
	stub = binary.LittleEndian.AppendUint64(stub, uint64(code))
	throw(syscall.Mprotect(stub[:4096], syscall.PROT_READ|syscall.PROT_EXEC))

	return uintptr(unsafe.Pointer(unsafe.SliceData(stub)))
}

type Instance struct {
	name   string
	obj    *Object
	code   *Code
	block  []byte
	memory *LinearMemory
	tables [][]uintptr
	// host is what the embedder attaches: the kernel's process, the WASI.
	host any
}

// LinearMemory is reserved at its maximum: it grows in place.
type LinearMemory struct {
	data     []byte
	pages    uint64
	max      uint64
	tailUsed bool
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

	m := &LinearMemory{data: mmap(int(most*pageSize+boundSlack), syscall.PROT_READ|syscall.PROT_WRITE), max: most}
	m.grow(uint32(least))

	return m
}

// grow adds pages and returns the old size, or MaxUint32 past the maximum.
func (m *LinearMemory) grow(delta uint32) uint32 {
	old := m.pages

	if old+uint64(delta) > m.max {
		return math.MaxUint32
	}

	if delta != 0 && m.tailUsed {
		// The old accessible tail becomes part of newly grown memory.
		// Writes to it must not survive the zero initialization of growth.
		clear(m.data[old*pageSize : old*pageSize+boundSlack])
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

func (i *Instance) memoryBytes() []byte {
	return i.memory.data[:i.memory.pages*pageSize+i.obj.meta.memory.tail]
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
	i.put64(offMemBound, uint64(int64(size+i.obj.meta.memory.tail)-boundSlack))

	if i.obj.meta.memory.tail != 0 {
		i.put64(i.obj.meta.memory.tailEnd, size+i.obj.meta.memory.tail)
	}
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

// The bulk memory instructions, whose ranges the code checked: memory.init
// copies from the segment's bytes in the object.
func helperMemoryCopy(ctx uintptr, d, s, n uint32) {
	m := instanceAt(ctx).memoryBytes()
	copy(m[d:][:n], m[s:][:n])
}

func helperMemoryFill(ctx uintptr, d, v, n uint32) {
	fill(instanceAt(ctx).memoryBytes()[d:][:n], byte(v))
}

func helperMemoryInit(ctx uintptr, d uint32, src uint64, n uint32) {
	copy(instanceAt(ctx).memoryBytes()[d:][:n], unsafe.Slice((*byte)(unsafe.Pointer(uintptr(src))), n))
}

// fill sets every byte of b to c.
func fill(b []byte, c byte) {
	if c == 0 {
		clear(b)

		return
	}

	if len(b) == 0 {
		return
	}

	b[0] = c

	for k := 1; k < len(b); k *= 2 {
		copy(b[k:], b[:k])
	}
}

// interruptible is a host whose code Go interrupts (lib/goruntime).
type interruptible interface {
	interrupt()
}

// helperInterrupt is the call Go injects into interrupted code: the host
// does what it asked the interruption for.
func helperInterrupt(ctx uintptr) {
	if h, ok := instanceAt(ctx).host.(interruptible); ok {
		h.interrupt()
	}
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

// keepFunc returns the code address of a Go function, the GOT's entry
// for it: the code calls it as Go does, with the instance in AX. A
// closure would miss its context; none of the VM's functions is one.
func (c *Code) keepFunc(f any) uintptr {
	c.keep = append(c.keep, f)

	return *(*uintptr)(unsafe.Pointer(funcval(f)))
}

func (c *Code) resolve(sym string, imports Imports) uintptr {
	switch sym {
	case symMorestack:
		return goSymbol("runtime.morestack_noctxt")
	case symTrap:
		return c.keepFunc(helperTrap)
	case symMemoryGrow:
		return c.keepFunc(helperMemoryGrow)
	case symTableGrow:
		return c.keepFunc(helperTableGrow)
	case symInterrupt:
		return c.keepFunc(helperInterrupt)
	case symMemoryCopy:
		return c.keepFunc(helperMemoryCopy)
	case symMemoryFill:
		return c.keepFunc(helperMemoryFill)
	case symMemoryInit:
		return c.keepFunc(helperMemoryInit)
	}

	for k := range c.obj.meta.imports {
		imp := &c.obj.meta.imports[k]

		if imp.kind != externFunc || imp.sym != sym {
			continue
		}

		f := imports.function(imp)

		if f == nil {
			throwFmt("unresolved import %s.%s", imp.module, imp.name)
		}

		if code, ok := f.(uintptr); ok {
			return code
		}

		if want := goFuncType(imp.params, imp.results); reflect.TypeOf(f) != want {
			throwFmt("import %s.%s is %T, not %s", imp.module, imp.name, f, want)
		}

		return c.keepFunc(f)
	}

	throwFmt("unresolved symbol %s", sym)

	return 0
}

// link links an object in place: the text, executable, then on pages of
// their own .rodata and a GOT entry per external symbol, read-only. It
// registers the code with Go; code lives as long as the process.
func link(name string, obj *Object, imports Imports) *Code {
	c := &Code{name: name, obj: obj}
	page := func(n int) int {
		return (n + 4095) &^ 4095
	}

	got := map[string]int{}

	for _, r := range obj.relocs {
		if r.in == inExternal {
			if _, ok := got[r.sym]; !ok {
				got[r.sym] = len(got)
			}
		}
	}

	textSize := page(max(len(obj.text), 1))
	gotAt := textSize + int(alignTo(int32(len(obj.rodata)), 16))
	c.text = mmap(textSize+page(gotAt-textSize+8*len(got)), syscall.PROT_READ|syscall.PROT_WRITE)
	copy(c.text, obj.text)
	copy(c.text[textSize:], obj.rodata)
	c.base = uintptr(unsafe.Pointer(unsafe.SliceData(c.text)))

	for sym, k := range got {
		binary.LittleEndian.PutUint64(c.text[gotAt+8*k:], uint64(c.resolve(sym, imports)))
	}

	for _, r := range obj.relocs {
		var target uint64

		switch r.in {
		case inText:
			target = uint64(c.base) + r.value
		case inRodata:
			target = uint64(c.base) + uint64(textSize) + r.value
		}

		place := uint64(c.base) + uint64(r.at)

		switch r.typ {
		case rX86_64_PC32:
			if r.in == inExternal {
				throwFmt("pc-relative relocation to external %s", r.sym)
			}

			put32rel(c.text[r.at:], int64(target)+r.addend-int64(place))
		case rX86_64_GOTPCREL, rX86_64_GOTPCRELX, rX86_64_REX_GOTPCRELX:
			if r.in != inExternal {
				throwFmt("GOT relocation to a symbol of the object at %#x", r.at)
			}

			put32rel(c.text[r.at:], int64(c.base)+int64(gotAt+8*got[r.sym])+r.addend-int64(place))
		case rX86_64_64:
			if r.in == inExternal {
				target = uint64(c.resolve(r.sym, imports))
			}

			binary.LittleEndian.PutUint64(c.text[r.at:], target+uint64(r.addend))
		default:
			throwFmt("unsupported relocation type %d at %#x", r.typ, r.at)
		}
	}

	throw(syscall.Mprotect(c.text[:textSize], syscall.PROT_READ|syscall.PROT_EXEC))
	throw(syscall.Mprotect(c.text[textSize:], syscall.PROT_READ))
	registerCode(name, c.base, uintptr(len(obj.text)), obj.meta.regions)

	return c
}

func put32rel(at []byte, v int64) {
	if v != int64(int32(v)) {
		throwFmt("relocation out of range")
	}

	binary.LittleEndian.PutUint32(at, uint32(v))
}

// instantiate links an object and makes an instance of it that may take
// stack bytes of goroutine stack; start runs its init.
func instantiate(name string, obj *Object, imports Imports, stack uint64) *Instance {
	return newInstance(link(name, obj, imports), imports, stack)
}

// newInstance makes an instance of linked code: its imports other than
// functions come from imports.
func newInstance(c *Code, imports Imports, stack uint64) *Instance {
	meta := c.obj.meta
	i := &Instance{name: c.name, obj: c.obj, code: c, block: mmap(int(max(meta.instanceSize, headerSize)), syscall.PROT_READ|syscall.PROT_WRITE)}

	if uint64(meta.instanceSize) > uint64(len(i.block)) {
		throwFmt("instance of %d bytes", meta.instanceSize)
	}

	i.put64(offHost, uint64(uintptr(unsafe.Pointer(i))))
	i.put64(offStackLimit, stack)

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
		if meta.memory.tail != 0 {
			if uint64(len(i.memory.data)) < i.memory.max*pageSize+meta.memory.tail {
				throwFmt("imported memory has no accessible tail")
			}

			i.memory.tailUsed = true
		}

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

	return i
}

// clone makes an instance of i's code that is a copy of i: its block, the
// tables' entries moved to its own, its globals, and of linear memory the
// bytes [from, to), the rest zero.
func (i *Instance) clone(imports Imports, stack uint64, from, to uint64) *Instance {
	c := newInstance(i.code, imports, stack)

	if i.memory != nil {
		c.memory.grow(uint32(i.memory.pages - c.memory.pages))
		copy(c.memory.data[from:to], i.memory.data[from:to])

		if i.memory.tailUsed {
			c.memory.tailUsed = true
			start := i.memory.pages * pageSize
			copy(c.memory.data[start:start+boundSlack], i.memory.data[start:start+boundSlack])
		}
	}

	copy(c.block[headerSize:], i.block[headerSize:])

	if meta := i.obj.meta.memory; meta != nil && meta.profile != 0 {
		clear(c.block[meta.profile : uint64(meta.profile)+uint64(meta.profileFuncs)*tailCounterBytes])
	}

	c.put64(offTokens, binary.LittleEndian.Uint64(i.block[offTokens:]))
	moved := c.ctx() - i.ctx()
	k := 0

	for _, t := range i.obj.meta.tables {
		if t.imported {
			continue
		}

		entries := c.tables[k]

		for e, v := range i.tables[k] {
			if v != 0 {
				entries[e] = v + moved
			}
		}

		c.put64(t.offset+tableEntries, uint64(uintptr(unsafe.Pointer(unsafe.SliceData(entries)))))
		k++
	}

	return c
}

// release unmaps the instance block and the memory the instance made; the
// instance is not used again.
func (i *Instance) release() {
	i.writeTailProfile()

	if i.memory != nil && !i.obj.meta.memory.imported && len(i.memory.users) == 1 {
		throw(syscall.Munmap(i.memory.data))
	}

	throw(syscall.Munmap(i.block))
	i.block, i.memory, i.tables = nil, nil, nil
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

	return i.code.base + uintptr(at)
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
