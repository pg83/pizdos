package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"strings"
)

// dumpFunc names a function, by name or index, whose IR is printed at
// each stage; traceFuncs prints every function as it is compiled.
var dumpFunc = os.Getenv("W2X_DUMP")
var traceFuncs = os.Getenv("W2X_TRACE") != ""

// compileModule compiles a module to an ELF relocatable object and the C
// header that describes how to embed it.

type callFixup struct {
	at int32
	fn int32
}

type reloc struct {
	at     int32
	sym    int32
	addend int64
}

type elfSym struct {
	name   string
	value  int32
	size   int32
	global bool
	fn     bool
}

type blob struct {
	bytes []byte
	align int
}

type objCode struct {
	m      *Module
	lay    *Layout
	prefix string
	// imports prefixes the symbols of imported functions.
	imports  string
	a        asm
	funcAt   []int32
	calls    []callFixup
	relocs   []reloc
	externs  []string
	externIx map[string]int32
	blobs    []blob
	blobAt   []int32
	floatIx  map[[2]uint64]int32
	syms     []elfSym
	initFn   int
}

func compileModule(data []byte, prefix, imports string) ([]byte, []byte) {
	m := decodeModule(data)
	oc := &objCode{m: m, prefix: prefix, imports: imports, externIx: map[string]int32{}, floatIx: map[[2]uint64]int32{}}
	oc.initFn = m.addInitFunction()
	oc.lay = newLayout(m)
	oc.funcAt = make([]int32, len(m.funcs))
	oc.addFixedBlobs()

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
		oc.syms = append(oc.syms, elfSym{name: oc.funcSymbol(i), value: oc.funcAt[i], size: oc.a.pos() - oc.funcAt[i], fn: true})
	}

	oc.importAdapters()
	oc.exportThunks()
	oc.initThunk()
	oc.placeBlobs()
	oc.resolve()

	return writeELF(oc), oc.header()
}

func (oc *objCode) funcSymbol(i int) string {
	name := fmt.Sprintf("%s_f%d", oc.prefix, i)

	if n := oc.m.funcs[i].name; n != "" {
		name += "_" + sanitize(n)
	}

	return name
}

func (oc *objCode) extern(name string) int32 {
	if i, ok := oc.externIx[name]; ok {
		return i
	}

	i := int32(len(oc.externs))
	oc.externs = append(oc.externs, name)
	oc.externIx[name] = i

	return i
}

func (oc *objCode) importSymbol(fn int) string {
	for _, imp := range oc.m.imports {
		if imp.kind == externFunc && int(imp.index) == fn {
			return oc.imports + sanitize(imp.module) + "__" + sanitize(imp.name)
		}
	}

	throwFmt("internal error: function %d is not imported", fn)

	return ""
}

func (oc *objCode) callFunc(fn int) {
	oc.calls = append(oc.calls, callFixup{oc.a.callRel(), int32(fn)})
}

func (oc *objCode) callImport(fn int) {
	oc.relocs = append(oc.relocs, reloc{oc.a.callRel(), oc.extern(oc.importSymbol(fn)), -4})
}

func (oc *objCode) callHelper(h int) {
	oc.relocs = append(oc.relocs, reloc{oc.a.callRel(), oc.extern(helperNames[h]), -4})
}

func (oc *objCode) jmpExtern(name string) {
	oc.a.b(0xe9)
	oc.relocs = append(oc.relocs, reloc{oc.a.pos(), oc.extern(name), -4})
	oc.a.u32(0)
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
}

func (oc *objCode) align16() {
	for len(oc.a.buf)%16 != 0 {
		oc.a.b(0xcc)
	}
}

// importAdapters give imported functions an entry in the internal
// convention, for their descriptors: the host takes the instance first.
func (oc *objCode) importAdapters() {
	for i := 0; i < oc.m.importedFuncs; i++ {
		oc.align16()
		oc.funcAt[i] = oc.a.pos()
		oc.a.movRR(8, RDI, ctxReg)
		oc.jmpExtern(oc.importSymbol(i))
	}
}

// thunkEnter saves the registers the host expects preserved that generated
// code changes, and sets up the instance and memory registers; frame is
// the stack space below.
func (oc *objCode) thunkEnter(frame int32) {
	a := &oc.a
	a.push(R14)
	a.push(R15)
	a.aluRI(aluSub, 8, RSP, int64(frame))
	a.movRR(8, ctxReg, RDI)
	a.load(8, 8, false, memReg, baseAt(ctxReg, offMem))
}

func (oc *objCode) thunkLeave(frame int32) {
	a := &oc.a
	a.aluRI(aluAdd, 8, RSP, int64(frame))
	a.pop(R15)
	a.pop(R14)
	a.ret()
}

func (oc *objCode) exportThunks() {
	m := oc.m

	for _, e := range m.exports {
		if e.kind != externFunc {
			continue
		}

		t := m.funcType(e.index)
		oc.align16()
		start := oc.a.pos()
		oc.exportThunk(int(e.index), t)
		oc.syms = append(oc.syms, elfSym{name: oc.prefix + "_" + sanitize(e.name), value: start, size: oc.a.pos() - start, global: true, fn: true})
	}
}

// exportThunk calls function index from System V: the instance comes first,
// then the arguments, then, for several results, a pointer they are stored
// to, 8 bytes each.
func (oc *objCode) exportThunk(index int, t *FuncType) {
	a := &oc.a
	gi, xi := 0, 0
	var stack []ValType

	for _, p := range t.params {
		if isFloat(p) {
			if xi < len(argXMMs) {
				xi++

				continue
			}
		} else if gi < len(argGPRs) {
			gi++

			continue
		}

		stack = append(stack, p)
	}

	multi := len(t.results) > 1
	resultsReg := VReg(noReg)
	resultsStack := -1

	if multi {
		if gi < len(argGPRs) {
			resultsReg = argGPRs[gi]
		} else {
			resultsStack = len(stack)
		}
	}

	// the outgoing stack arguments, then the saved results pointer
	slot := int32(8 * len(stack))
	frame := slot + 8

	if (frame+16+8)%16 != 0 {
		frame += 8
	}

	incoming := frame + 16 + 8
	oc.thunkEnter(frame)
	gi = 0

	for _, p := range t.params {
		if !isFloat(p) && gi < len(argGPRs) {
			if p == I32 {
				a.movRR(4, argGPRs[gi], argGPRs[gi])
			}

			gi++
		}
	}

	for k, p := range stack {
		src := rspAt(incoming + int32(8*k))

		switch {
		case isFloat(p):
			a.fload(8, XMM0+15, src)
			a.fstore(8, XMM0+15, rspAt(int32(8*k)))
		case p == I32:
			a.load(4, 4, false, RAX, src)
			a.store(8, RAX, rspAt(int32(8*k)))
		default:
			a.load(8, 8, false, RAX, src)
			a.store(8, RAX, rspAt(int32(8*k)))
		}
	}

	if multi {
		if resultsReg != noReg {
			a.movRR(8, RDI, resultsReg)
		} else {
			a.load(8, 8, false, RDI, rspAt(incoming+int32(8*resultsStack)))
		}

		a.store(8, RDI, rspAt(slot))
	}

	oc.callFunc(index)

	if multi && !memResults(t.results) {
		a.load(8, 8, false, RCX, rspAt(slot))
		g, x := 0, 0

		for i, r := range t.results {
			if isFloat(r) {
				a.fstore(8, resultXMMs[x], baseAt(RCX, int32(8*i)))
				x++
			} else {
				a.store(8, resultGPRs[g], baseAt(RCX, int32(8*i)))
				g++
			}
		}
	}

	oc.thunkLeave(frame)
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

func (oc *objCode) initThunk() {
	m := oc.m
	lay := oc.lay
	oc.align16()
	start := oc.a.pos()
	a := &oc.a
	oc.thunkEnter(8)

	for i := range m.funcs {
		d := lay.desc(i)
		a.b(0x48, 0x8d, 0x05)
		a.fixups = append(a.fixups, ripFixup{at: a.pos(), end: a.pos() + 4, label: int32(i), kind: labelFunc})
		a.u32(0)
		a.store(8, RAX, baseAt(ctxReg, d))
		a.movImm(8, RAX, int64(typeID(m.funcType(uint32(i)))))
		a.store(8, RAX, baseAt(ctxReg, d+8))
	}

	var traps []int32

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
			traps = append(traps, a.jccRel(ccB))

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
	oc.thunkLeave(8)

	if len(traps) > 0 {
		at := a.pos()

		for _, t := range traps {
			a.patch32(t, at)
		}

		a.aluRI(aluAnd, 8, RSP, -16)
		a.movRR(8, RDI, ctxReg)
		a.movImm(4, RSI, trapTableOOB)
		oc.callHelper(helperTrap)
		a.ud2()
	}

	oc.syms = append(oc.syms, elfSym{name: "w2x_init_" + oc.prefix, value: start, size: a.pos() - start, global: true, fn: true})
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

func cType(t ValType) string {
	switch t {
	case I32:
		return "uint32_t"
	case I64:
		return "uint64_t"
	case F32:
		return "float"
	case F64:
		return "double"
	}

	return "void*"
}

func (oc *objCode) signature(name string, t *FuncType) string {
	ret := "void"

	if len(t.results) == 1 {
		ret = cType(t.results[0])
	}

	params := []string{"void* instance"}

	for i, p := range t.params {
		params = append(params, fmt.Sprintf("%s a%d", cType(p), i))
	}

	if len(t.results) > 1 {
		// every result is stored as 8 bytes, in order
		ret = "void"
		params = append(params, "uint64_t* results")
	}

	return fmt.Sprintf("%s %s(%s)", ret, name, strings.Join(params, ", "))
}

func (oc *objCode) header() []byte {
	m := oc.m
	lay := oc.lay
	P := strings.ToUpper(oc.prefix)
	var sb strings.Builder
	sb.WriteString("// Generated by wasm2o: the embedding interface of one module.\n#pragma once\n\n#include \"w2x.h\"\n\n")
	fmt.Fprintf(&sb, "#define %s_INSTANCE_SIZE %d\n", P, lay.size)

	if len(m.mems) > 0 {
		l := m.mems[0].limits
		max := uint64(65536)

		if l.hasMax {
			max = l.max
		}

		fmt.Fprintf(&sb, "#define %s_MEMORY_PAGES %d\n#define %s_MEMORY_MAX_PAGES %d\n", P, l.min, P, max)
	}

	for i, t := range m.tables {
		max := uint64(math.MaxUint32)

		if t.limits.hasMax {
			max = t.limits.max
		}

		fmt.Fprintf(&sb, "#define %s_TABLE%d_OFFSET %d\n#define %s_TABLE%d_SIZE %d\n#define %s_TABLE%d_MAX %d\n", P, i, lay.tables[i], P, i, t.limits.min, P, i, max)
	}

	for _, e := range m.exports {
		if e.kind == externGlobal {
			fmt.Fprintf(&sb, "#define %s_GLOBAL_%s_OFFSET %d\n", P, sanitize(e.name), lay.globals[e.index])
		}
	}

	for i, g := range m.globals {
		if g.imported {
			fmt.Fprintf(&sb, "#define %s_IMPORTED_GLOBAL%d_OFFSET %d\n", P, i, lay.globals[i])
		}
	}

	sb.WriteString("\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
	fmt.Fprintf(&sb, "void w2x_init_%s(void* instance);\n", oc.prefix)

	for _, e := range m.exports {
		if e.kind == externFunc {
			fmt.Fprintf(&sb, "%s;\n", oc.signature(oc.prefix+"_"+sanitize(e.name), m.funcType(e.index)))
		}
	}

	sb.WriteString("\n// imports\n")

	for _, imp := range m.imports {
		if imp.kind == externFunc {
			fmt.Fprintf(&sb, "%s;\n", oc.signature(oc.imports+sanitize(imp.module)+"__"+sanitize(imp.name), m.funcType(imp.index)))
		}
	}

	sb.WriteString("\n#ifdef __cplusplus\n}\n#endif\n")

	// A description for tools: one line per import and export.
	sb.WriteString("\n")

	for _, imp := range m.imports {
		switch imp.kind {
		case externFunc:
			fmt.Fprintf(&sb, "// import func =%s =%s %s\n", sanitize(imp.module), sanitize(imp.name), oc.imports+sanitize(imp.module)+"__"+sanitize(imp.name))
		case externGlobal:
			g := m.globals[imp.index]
			fmt.Fprintf(&sb, "// import global =%s =%s %d %s %v\n", sanitize(imp.module), sanitize(imp.name), lay.globals[imp.index], typeName(g.typ), g.mutable)
		case externMemory:
			fmt.Fprintf(&sb, "// import memory =%s =%s\n", sanitize(imp.module), sanitize(imp.name))
		case externTable:
			fmt.Fprintf(&sb, "// import table =%s =%s %d\n", sanitize(imp.module), sanitize(imp.name), lay.tables[imp.index])
		}
	}

	for _, e := range m.exports {
		switch e.kind {
		case externFunc:
			t := m.funcType(e.index)
			var ps, rs []string

			for _, p := range t.params {
				ps = append(ps, typeName(p))
			}

			for _, r := range t.results {
				rs = append(rs, typeName(r))
			}

			fmt.Fprintf(&sb, "// export func =%s %s_%s (%s) (%s)\n", sanitize(e.name), oc.prefix, sanitize(e.name), strings.Join(ps, ","), strings.Join(rs, ","))
		case externGlobal:
			g := m.globals[e.index]
			fmt.Fprintf(&sb, "// export global =%s %d %s %v %v\n", sanitize(e.name), lay.globals[e.index], typeName(g.typ), g.imported, g.mutable)
		case externMemory:
			fmt.Fprintf(&sb, "// export memory =%s\n", sanitize(e.name))
		case externTable:
			fmt.Fprintf(&sb, "// export table =%s %d\n", sanitize(e.name), lay.tables[e.index])
		}
	}

	return []byte(sb.String())
}
