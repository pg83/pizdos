package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"runtime"
)

// dumpFunc names a function, by name or index, whose IR is printed at
// each stage; traceFuncs prints every function as it is compiled;
// optRange, lo-hi, limits the SSA optimizer to those functions.
var dumpFunc = os.Getenv("W2X_DUMP")
var traceFuncs = os.Getenv("W2X_TRACE") != ""
var optRange = os.Getenv("W2X_OPT_FUNCS")

func optEnabled(i int) bool {
	if optRange == "" {
		return true
	}

	var lo, hi int
	fmt.Sscanf(optRange, "%d-%d", &lo, &hi)

	return i >= lo && i <= hi
}

// compileModule compiles a module to an ordinary ELF relocatable object
// (elf.go) of the ABI of goabi.go.

type callFixup struct {
	at int32
	fn int32
}

type blob struct {
	bytes []byte
	align int
}

// textReloc is a relocation of the text: R_X86_64_PC32 to .rodata when sym
// is negative, else R_X86_64_GOTPCREL to the external symbol sym.
type textReloc struct {
	at     int32
	sym    int32
	addend int64
}

// Absolute text addresses in jump tables, relocated by an ordinary linker.
type TextAddress struct {
	at, target int32
}

type objCode struct {
	m         *Module
	lay       *Layout
	a         asm
	funcAt    []int32
	stubAt    [stubCount]int32
	calls     []callFixup
	stubCalls []callFixup
	// jumpAt holds the adapters of direct calls of the imports that return
	// twice, by function.
	jumpAt     map[int32]int32
	jumpCalls  []callFixup
	blobs      []blob
	blobAt     []int32
	rodata     []byte
	floatIx    map[[2]uint64]int32
	externs    []string
	externIx   map[string]int32
	relocs     []textReloc
	addresses  []TextAddress
	regions    []Region
	regionName string
	// globals are the regions whose symbols are global: thunks Go calls.
	globals map[string]bool
	initFn  int
}

type CompileOptions struct {
	memoryTail  bool
	tailRanges  bool
	tailProfile bool
}

func compileModule(data []byte) []byte {
	options := CompileOptions{memoryTail: true, tailRanges: true}

	switch mode := os.Getenv("W2X_MEMORY_TAIL"); mode {
	case "", "1", "optimized":
	case "plain":
		options.tailRanges = false
	case "strict":
		options.memoryTail, options.tailRanges = false, false
	default:
		throwFmt("unknown memory tail mode %q", mode)
	}

	options.tailProfile = options.tailRanges && os.Getenv("W2X_TAIL_PROFILE") != ""

	return compileModuleWithOptions(data, options)
}

func compileModuleWithOptions(data []byte, options CompileOptions) []byte {
	m := decodeModule(data)

	if options.memoryTail && len(m.mems) > 0 {
		m.memoryTail = boundSlack
		m.tailRanges = options.tailRanges
		m.tailProfile = options.tailProfile && options.tailRanges
	}

	oc := &objCode{m: m, floatIx: map[[2]uint64]int32{}, externIx: map[string]int32{}, globals: map[string]bool{}, jumpAt: map[int32]int32{}}
	oc.initFn = m.addInitFunction()
	oc.lay = newLayout(m)
	oc.funcAt = make([]int32, len(m.funcs))
	oc.addFixedBlobs()
	oc.stubs()
	oc.importAdapters()

	for i := m.importedFuncs; i < len(m.funcs); i++ {
		var mf *MFunc
		var al *allocator

		dump := dumpFunc != "" && (m.funcs[i].name == dumpFunc || fmt.Sprint(i) == dumpFunc)

		if traceFuncs {
			fmt.Fprintf(os.Stderr, "compile %d %s\n", i, m.funcs[i].name)
		}

		if e := try(func() {
			f := buildFunction(m, i)

			if dump {
				fmt.Fprintf(os.Stderr, "== ssa %d %s\n%s", i, m.funcs[i].name, f.dump())
			}

			if optEnabled(i) {
				optimize(f)

				if checkAlloc {
					checkSSA(f)
				}
			}

			if dump {
				fmt.Fprintf(os.Stderr, "== optimized\n%s", f.dump())
			}

			mf = lowerFunction(m, oc.lay, f)

			if dump {
				fmt.Fprintf(os.Stderr, "== mir\n%s", mf.dump())
			}

			dumping = dump
			al = allocate(mf)
			dumping = false

			if dump {
				fmt.Fprintf(os.Stderr, "== allocated\n%s", mf.dump())
			}
		}); e != nil {
			throwFmt("function %d (%s): %s", i, m.funcs[i].name, e.message())
		}

		oc.emitFunction(i, mf, al)

		if runtime.GOARCH == "wasm" {
			// Go on wasm schedules cooperatively. Let its collector finish
			// between functions instead of retaining successive IR graphs
			// as allocations made during one long marking cycle.
			runtime.Gosched()
		}
	}

	oc.exportThunks()
	oc.initThunk()
	oc.placeBlobs()
	oc.resolve()

	return writeELF(oc, encodeNote(oc.description()))
}

// funcName names a function's region, which Go shows in tracebacks.
func (oc *objCode) funcName(i int) string {
	if n := oc.m.funcs[i].name; n != "" {
		return fmt.Sprintf("f%d:%s", i, displayName(n))
	}

	return fmt.Sprintf("f%d", i)
}

func (oc *objCode) callFunc(fn int) {
	oc.calls = append(oc.calls, callFixup{oc.a.callRel(), int32(fn)})
}

func (oc *objCode) addBlob(b []byte, align int) int32 {
	oc.blobs = append(oc.blobs, blob{b, align})

	return int32(len(oc.blobs) - 1)
}

func (oc *objCode) addFixedBlobs() {
	mask := func(lo, hi uint64) []byte {
		b := make([]byte, 16)
		binary.LittleEndian.PutUint64(b, lo)
		binary.LittleEndian.PutUint64(b[8:], hi)

		return b
	}

	// in the order of constF32Sign..constF64Abs
	oc.addBlob(mask(0x80000000, 0), 16)
	oc.addBlob(mask(0x7fffffff, 0), 16)
	oc.addBlob(mask(1<<63, 0), 16)
	oc.addBlob(mask(1<<63-1, 0), 16)

	for _, d := range oc.m.datas {
		oc.addBlob(d.bytes, 1)
	}
}

// floatConst returns a RIP-relative operand for a float constant.
func (oc *objCode) floatConst(bits uint64, size uint8) amem {
	key := [2]uint64{bits, uint64(size)}
	label, ok := oc.floatIx[key]

	if !ok {
		b := make([]byte, size)

		if size == 4 {
			binary.LittleEndian.PutUint32(b, uint32(bits))
		} else {
			binary.LittleEndian.PutUint64(b, bits)
		}

		label = oc.addBlob(b, int(size))
		oc.floatIx[key] = label
	}

	return amem{base: -1, index: -1, label: label, labelKind: labelModule}
}

// placeBlobs lays the constants and data segments out in .rodata.
func (oc *objCode) placeBlobs() {
	oc.blobAt = make([]int32, len(oc.blobs))

	for i, b := range oc.blobs {
		for len(oc.rodata)%b.align != 0 {
			oc.rodata = append(oc.rodata, 0)
		}

		oc.blobAt[i] = int32(len(oc.rodata))
		oc.rodata = append(oc.rodata, b.bytes...)
	}
}

// resolve fills the displacements within the text and turns the others
// into relocations: a displacement is relative to the end of its
// instruction, so the addend takes that distance off.
func (oc *objCode) resolve() {
	a := &oc.a

	for _, f := range a.fixups {
		switch f.kind {
		case labelModule:
			oc.relocs = append(oc.relocs, textReloc{f.at, -1, int64(oc.blobAt[f.label]) - int64(f.end-f.at)})
		case labelGot:
			oc.relocs = append(oc.relocs, textReloc{f.at, f.label, -int64(f.end - f.at)})
		case labelFunc:
			binary.LittleEndian.PutUint32(a.buf[f.at:], uint32(oc.funcAt[f.label]-f.end))
		default:
			throwFmt("internal error: unresolved local label")
		}
	}

	for _, c := range oc.calls {
		a.patch32(c.at, oc.funcAt[c.fn])
	}

	for _, c := range oc.stubCalls {
		a.patch32(c.at, oc.stubAt[c.fn])
	}

	for _, c := range oc.jumpCalls {
		a.patch32(c.at, oc.jumpAt[c.fn])
	}
}

func (oc *objCode) align16() {
	for len(oc.a.buf)%16 != 0 {
		oc.a.b(0xcc)
	}
}

// importAdapters give imported functions an entry in the internal
// convention: an adapter calls the Go function value of the import's
// symbol, which names it with its type. One that returns twice has a second
// adapter, for its direct calls, which writes the jump frame; the module
// then has the land stub (goabi.go). Tables and exports take the first,
// as their callers keep no token.
func (oc *objCode) importAdapters() {
	m := oc.m

	for _, imp := range m.imports {
		if imp.kind != externFunc {
			continue
		}

		t := m.funcType(imp.index)
		sym := importSymbol(imp.module, imp.name, t)
		name := "w2o.adapter." + symbolPart(imp.module) + "." + symbolPart(imp.name)
		oc.funcAt[imp.index] = oc.goCall(name, t.params, t.results, sym, false)

		if m.funcs[imp.index].returnsTwice {
			oc.jumpAt[int32(imp.index)] = oc.goCall(name+".jump", t.params, t.results, sym, true)
		}
	}

	if len(oc.jumpAt) > 0 {
		oc.landStub()
		oc.framesStub()
		oc.resumeStub()
	}
}

// exportThunks gives every exported function a thunk Go calls, global.
func (oc *objCode) exportThunks() {
	m := oc.m

	for _, e := range m.exports {
		if e.kind == externFunc {
			t := m.funcType(e.index)
			sym := exportSymbol(e.name, t)
			oc.globals[sym] = true
			oc.thunk(sym, t.params, t.results, int(e.index), nil)
		}
	}
}

// constOffset evaluates a constant offset expression: i32.const or
// i64.const.
func constOffset(expr []byte) (int64, bool) {
	r := &reader{data: expr}
	op := r.byte()

	var v int64

	switch op {
	case 0x41:
		v = int64(uint32(r.s32()))
	case 0x42:
		v = r.s64()
	default:
		return 0, false
	}

	return v, r.byte() == 0x0b && r.eof()
}

// initThunk emits the thunk Go calls on a new instance, func(instance
// uintptr): it fills the function descriptors, applies the active element
// segments and calls the init function, which sets the globals, applies the
// data segments and calls the start function.
func (oc *objCode) initThunk() {
	m := oc.m
	lay := oc.lay
	a := &oc.a

	oc.globals[symInit] = true
	oc.thunk(symInit, nil, nil, -1, func() {
		for i := range m.funcs {
			d := lay.desc(i)
			a.b(0x48, 0x8d, 0x05)
			a.fixups = append(a.fixups, ripFixup{at: a.pos(), end: a.pos() + 4, label: int32(i), kind: labelFunc})
			a.u32(0)
			a.store(8, RAX, baseAt(ctxReg, d))
			a.movImm(8, RAX, int64(typeID(m.funcType(uint32(i)))))
			a.store(8, RAX, baseAt(ctxReg, d+8))
		}

		for i, e := range m.elems {
			if e.mode == segmentActive {
				off, ok := constOffset(e.offset)

				if !ok {
					throwFmt("unsupported: element segment %d with a computed offset", i)
				}

				tb := lay.tables[e.table]
				n := int64(len(e.exprs))
				arr := make([]byte, 0, 4*n)

				for _, x := range e.exprs {
					var d int32

					if x[0] == 0xd2 {
						r := &reader{data: x[1:]}
						d = lay.desc(int(r.u32()))
					}

					arr = binary.LittleEndian.AppendUint32(arr, uint32(d))
				}

				// off + n must fit the table
				a.load(4, 4, false, RAX, baseAt(ctxReg, tb+tableSize))
				a.movImm(8, RCX, off+n)
				a.aluRR(aluCmp, 8, RAX, RCX)
				fits := a.jcc8(ccAE)
				oc.trapCall(trapTableOOB)
				a.here8(fits)

				if n > 0 {
					label := oc.addBlob(arr, 4)
					a.lea(8, RSI, amem{base: -1, index: -1, label: label, labelKind: labelModule})
					a.load(8, 8, false, RDI, baseAt(ctxReg, tb+tableEntries))
					a.movImm(8, RAX, 8*off)
					a.aluRR(aluAdd, 8, RDI, RAX)
					a.movImm(4, RCX, n)
					loop := a.pos()
					a.opRM(true, []byte{0x63}, int(RAX), baseAt(RSI, 0), 0, false)
					a.test(8, RAX, RAX)
					null := a.jcc8(ccE)
					a.aluRR(aluAdd, 8, RAX, ctxReg)
					a.here8(null)
					a.store(8, RAX, baseAt(RDI, 0))
					a.aluRI(aluAdd, 8, RSI, 4)
					a.aluRI(aluAdd, 8, RDI, 8)
					a.aluRI(aluSub, 4, RCX, 1)
					a.patch32(a.jccRel(ccNE), loop)
				}
			}

			if e.mode != segmentPassive {
				a.storeImm(1, baseAt(ctxReg, lay.elemDropped(m, i)), 1)
			}
		}

		oc.callFunc(oc.initFn)
	})
}

// description is what the symbols do not tell a loader: the instance, its
// memory, tables and globals, the imports and exports of those.
func (oc *objCode) description() *Meta {
	m := oc.m
	lay := oc.lay
	meta := &Meta{instanceSize: uint32(lay.size)}

	if len(m.mems) > 0 {
		l := m.mems[0].limits
		max := uint64(65536)

		if l.hasMax {
			max = l.max
		}

		meta.memory = &MemoryMeta{min: l.min, max: max, imported: m.mems[0].imported, tail: m.memoryTail}

		if m.memoryTail != 0 {
			meta.memory.tailEnd = uint32(lay.memoryEnd)
		}

		if m.tailProfile {
			meta.memory.profile = uint32(lay.tailProfile)
			meta.memory.profileFuncs = uint32(len(m.funcs))
		}
	}

	for i, t := range m.tables {
		max := uint64(math.MaxUint32)

		if t.limits.hasMax {
			max = t.limits.max
		}

		meta.tables = append(meta.tables, TableMeta{offset: uint32(lay.tables[i]), min: t.limits.min, max: max, imported: t.imported})
	}

	for i, g := range m.globals {
		meta.globals = append(meta.globals, GlobalMeta{offset: uint32(lay.globals[i]), typ: g.typ, mutable: g.mutable, imported: g.imported})
	}

	for _, imp := range m.imports {
		if imp.kind != externFunc {
			meta.imports = append(meta.imports, ImportMeta{kind: imp.kind, module: imp.module, name: imp.name, index: imp.index})
		}
	}

	for _, e := range m.exports {
		if e.kind != externFunc {
			meta.exports = append(meta.exports, ExportMeta{kind: e.kind, name: e.name, index: e.index})
		}
	}

	return meta
}

// addInitFunction appends a function that initializes the globals and
// applies the active data segments, then calls the start function.
func (m *Module) addInitFunction() int {
	code := []byte{0}
	var parts [][]byte
	steps := 0
	next := func() {
		// Go modules can contain tens of thousands of small data segments.
		// Keep their generated initializer from becoming one enormous SSA
		// and register-allocation problem.
		if steps == 128 {
			parts = append(parts, append(code, 0x0b))
			code = []byte{0}
			steps = 0
		}

		steps++
	}

	for i := m.importedGlobals; i < len(m.globals); i++ {
		next()
		g := &m.globals[i]
		code = append(code, g.init[:len(g.init)-1]...)
		code = append(code, 0x24)
		code = appendLeb(code, uint64(i))
	}

	for i, d := range m.datas {
		if d.mode != segmentActive {
			continue
		}

		next()
		code = append(code, d.offset[:len(d.offset)-1]...)
		code = append(code, 0x41, 0, 0x41)
		code = appendSleb(code, int64(int32(uint32(len(d.bytes)))))
		code = append(code, 0xfc, 8)
		code = appendLeb(code, uint64(i))
		code = append(code, 0, 0xfc, 9)
		code = appendLeb(code, uint64(i))
	}

	typ := -1

	for i, t := range m.types {
		if len(t.params) == 0 && len(t.results) == 0 {
			typ = i

			break
		}
	}

	if typ < 0 {
		m.types = append(m.types, FuncType{})
		typ = len(m.types) - 1
	}

	if len(parts) > 0 {
		parts = append(parts, append(code, 0x0b))
		code = []byte{0}

		for k, body := range parts {
			fn := len(m.funcs)
			m.funcs = append(m.funcs, Func{typ: uint32(typ), body: body, name: fmt.Sprintf("init.part%d", k)})
			code = appendLeb(append(code, 0x10), uint64(fn))
		}
	}

	if m.start >= 0 {
		code = appendLeb(append(code, 0x10), uint64(m.start))
	}

	code = append(code, 0x0b)

	if m.dataCount < 0 {
		m.dataCount = int64(len(m.datas))
	}

	m.funcs = append(m.funcs, Func{typ: uint32(typ), body: code, name: "init"})

	return len(m.funcs) - 1
}
