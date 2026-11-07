package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
)

// dumpFunc names a function, by name or index, whose IR is printed at
// each stage; traceFuncs prints every function as it is compiled.
var dumpFunc = os.Getenv("W2X_DUMP")
var traceFuncs = os.Getenv("W2X_TRACE") != ""

// compileModule compiles a module to an ELF relocatable object whose .w2o
// section describes it to a loader (meta.go).

type callFixup struct {
	at int32
	fn int32
}

type blob struct {
	bytes []byte
	align int
}

// objSlot is eight bytes of the text the loader fills with a symbol's value.
type objSlot struct {
	label int32
	sym   string
}

type objCode struct {
	m          *Module
	lay        *Layout
	a          asm
	funcAt     []int32
	stubAt     [stubCount]int32
	calls      []callFixup
	stubCalls  []callFixup
	blobs      []blob
	blobAt     []int32
	floatIx    map[[2]uint64]int32
	slots      []objSlot
	slotIx     map[string]int32
	regions    []Region
	regionName string
	// importSyms names the Go function each imported function calls.
	importSyms map[int]string
	initFn     int
}

func compileModule(data []byte) []byte {
	m := decodeModule(data)
	oc := &objCode{m: m, floatIx: map[[2]uint64]int32{}, slotIx: map[string]int32{}, importSyms: map[int]string{}}
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
	}

	exports := oc.exportThunks()
	oc.initThunk()
	oc.placeBlobs()
	oc.resolve()

	return writeELF(oc, encodeMeta(oc.meta(exports)))
}

// funcName names a function's region, which Go shows in tracebacks.
func (oc *objCode) funcName(i int) string {
	if n := oc.m.funcs[i].name; n != "" {
		return fmt.Sprintf("f%d:%s", i, n)
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

func (oc *objCode) placeBlobs() {
	a := &oc.a
	oc.blobAt = make([]int32, len(oc.blobs))

	for i, b := range oc.blobs {
		for len(a.buf)%b.align != 0 {
			a.b(0)
		}

		oc.blobAt[i] = a.pos()
		a.b(b.bytes...)
	}
}

func (oc *objCode) resolve() {
	a := &oc.a

	for _, f := range a.fixups {
		var target int32

		switch f.kind {
		case labelModule:
			target = oc.blobAt[f.label]
		case labelFunc:
			target = oc.funcAt[f.label]
		default:
			throwFmt("internal error: unresolved local label")
		}

		binary.LittleEndian.PutUint32(a.buf[f.at:], uint32(target-f.end))
	}

	for _, c := range oc.calls {
		a.patch32(c.at, oc.funcAt[c.fn])
	}

	for _, c := range oc.stubCalls {
		a.patch32(c.at, oc.stubAt[c.fn])
	}
}

func (oc *objCode) align16() {
	for len(oc.a.buf)%16 != 0 {
		oc.a.b(0xcc)
	}
}

// importAdapters give imported functions an entry in the internal
// convention: an adapter calls the Go function value of the import's
// symbol, its module and name, or with a type other than an earlier import
// of the same name, those and its index.
func (oc *objCode) importAdapters() {
	m := oc.m
	types := map[string]uint64{}

	for _, imp := range m.imports {
		if imp.kind != externFunc {
			continue
		}

		t := m.funcType(imp.index)
		sym := imp.module + "." + imp.name

		if id, ok := types[sym]; ok && id != typeID(t) {
			sym = fmt.Sprintf("%s#%d", sym, imp.index)
		}

		types[sym] = typeID(t)
		oc.importSyms[int(imp.index)] = sym
		oc.funcAt[imp.index] = oc.goCall("import:"+imp.module+"."+imp.name, t.params, t.results, sym)
	}
}

// exportThunks gives every exported function a thunk Go calls.
func (oc *objCode) exportThunks() []ExportMeta {
	m := oc.m
	var out []ExportMeta

	for _, e := range m.exports {
		x := ExportMeta{kind: e.kind, name: e.name, index: e.index}

		if e.kind == externFunc {
			t := m.funcType(e.index)
			x.sym = "export:" + e.name
			x.params, x.results = t.params, t.results
			oc.thunk(x.sym, t.params, t.results, int(e.index), nil)
		}

		out = append(out, x)
	}

	return out
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

	oc.thunk("init", nil, nil, -1, func() {
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

// meta describes the module to its loader.
func (oc *objCode) meta(exports []ExportMeta) *Meta {
	m := oc.m
	lay := oc.lay
	meta := &Meta{instanceSize: uint32(lay.size), exports: exports, init: "init", regions: oc.regions}

	if len(m.mems) > 0 {
		l := m.mems[0].limits
		max := uint64(65536)

		if l.hasMax {
			max = l.max
		}

		meta.memory = &MemoryMeta{min: l.min, max: max, imported: m.mems[0].imported}
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
		x := ImportMeta{kind: imp.kind, module: imp.module, name: imp.name, index: imp.index}

		if imp.kind == externFunc {
			t := m.funcType(imp.index)
			x.sym = oc.importSyms[int(imp.index)]
			x.params, x.results = t.params, t.results
		}

		meta.imports = append(meta.imports, x)
	}

	return meta
}

// addInitFunction appends a function that initializes the globals and
// applies the active data segments, then calls the start function.
func (m *Module) addInitFunction() int {
	code := []byte{0}

	for i := m.importedGlobals; i < len(m.globals); i++ {
		g := &m.globals[i]
		code = append(code, g.init[:len(g.init)-1]...)
		code = append(code, 0x24)
		code = appendLeb(code, uint64(i))
	}

	for i, d := range m.datas {
		if d.mode != segmentActive {
			continue
		}

		code = append(code, d.offset[:len(d.offset)-1]...)
		code = append(code, 0x41, 0, 0x41)
		code = appendSleb(code, int64(int32(uint32(len(d.bytes)))))
		code = append(code, 0xfc, 8)
		code = appendLeb(code, uint64(i))
		code = append(code, 0, 0xfc, 9)
		code = appendLeb(code, uint64(i))
	}

	if m.start >= 0 {
		code = append(code, 0x10)
		code = appendLeb(code, uint64(m.start))
	}

	code = append(code, 0x0b)
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

	if m.dataCount < 0 {
		m.dataCount = int64(len(m.datas))
	}

	m.funcs = append(m.funcs, Func{typ: uint32(typ), body: code, name: "init"})

	return len(m.funcs) - 1
}
