package main

import "encoding/binary"

// The .w2o section of an object describes the module to its loader, which
// never needs the source wasm: the instance's size and layout, the memory,
// tables and globals, the imports and exports with their symbols and types,
// and the code regions with their stack pointer tables. Integers are
// unsigned LEB128, stack pointer deltas signed; a string or a list is its
// length, then its bytes or elements, in the order of the fields below.

const metaMagic = "W2O1"

type MemoryMeta struct {
	// min and max are in pages of 64 KiB.
	min, max uint64
	imported bool
}

type TableMeta struct {
	offset   uint32
	min, max uint64
	imported bool
}

type GlobalMeta struct {
	offset uint32
	typ    ValType
	// An imported global's slot holds the address of its value.
	mutable, imported bool
}

type ImportMeta struct {
	kind   byte
	module string
	name   string
	// index is the entity in its index space; a function's adapter calls
	// the Go function value of sym, with the type that follows.
	index   uint32
	sym     string
	params  []ValType
	results []ValType
}

type ExportMeta struct {
	kind  byte
	name  string
	index uint32
	// sym is a function's thunk, Go calls it as func(instance uintptr,
	// params...) (results...).
	sym     string
	params  []ValType
	results []ValType
}

// Region is code with one entry: a function, an adapter, a thunk, a stub.
type Region struct {
	name       string
	start, end uint32
	sp         []spEntry
}

type Meta struct {
	instanceSize uint32
	memory       *MemoryMeta
	tables       []TableMeta
	globals      []GlobalMeta
	imports      []ImportMeta
	exports      []ExportMeta
	// init is the thunk that fills the instance, applies the segments and
	// calls the start function.
	init    string
	regions []Region
}

type metaWriter struct {
	out []byte
}

func (w *metaWriter) u(x uint64) {
	w.out = binary.AppendUvarint(w.out, x)
}

func (w *metaWriter) s(x int64) {
	w.out = binary.AppendVarint(w.out, x)
}

func (w *metaWriter) flag(b bool) {
	if b {
		w.u(1)
	} else {
		w.u(0)
	}
}

func (w *metaWriter) str(s string) {
	w.u(uint64(len(s)))
	w.out = append(w.out, s...)
}

func (w *metaWriter) types(ts []ValType) {
	w.u(uint64(len(ts)))

	for _, t := range ts {
		w.u(uint64(t))
	}
}

func encodeMeta(m *Meta) []byte {
	w := &metaWriter{}
	w.out = append(w.out, metaMagic...)
	w.u(uint64(m.instanceSize))
	w.flag(m.memory != nil)

	if m.memory != nil {
		w.u(m.memory.min)
		w.u(m.memory.max)
		w.flag(m.memory.imported)
	}

	w.u(uint64(len(m.tables)))

	for _, t := range m.tables {
		w.u(uint64(t.offset))
		w.u(t.min)
		w.u(t.max)
		w.flag(t.imported)
	}

	w.u(uint64(len(m.globals)))

	for _, g := range m.globals {
		w.u(uint64(g.offset))
		w.u(uint64(g.typ))
		w.flag(g.mutable)
		w.flag(g.imported)
	}

	w.u(uint64(len(m.imports)))

	for _, i := range m.imports {
		w.u(uint64(i.kind))
		w.str(i.module)
		w.str(i.name)
		w.u(uint64(i.index))
		w.str(i.sym)
		w.types(i.params)
		w.types(i.results)
	}

	w.u(uint64(len(m.exports)))

	for _, e := range m.exports {
		w.u(uint64(e.kind))
		w.str(e.name)
		w.u(uint64(e.index))
		w.str(e.sym)
		w.types(e.params)
		w.types(e.results)
	}

	w.str(m.init)
	w.u(uint64(len(m.regions)))

	for _, r := range m.regions {
		w.str(r.name)
		w.u(uint64(r.start))
		w.u(uint64(r.end))
		w.u(uint64(len(r.sp)))

		for _, e := range r.sp {
			w.u(uint64(e.pc))
			w.s(int64(e.delta))
		}
	}

	return w.out
}

type metaReader struct {
	data []byte
}

func (r *metaReader) u() uint64 {
	x, n := binary.Uvarint(r.data)

	if n <= 0 {
		throwFmt("malformed object metadata")
	}

	r.data = r.data[n:]

	return x
}

func (r *metaReader) s() int64 {
	x, n := binary.Varint(r.data)

	if n <= 0 {
		throwFmt("malformed object metadata")
	}

	r.data = r.data[n:]

	return x
}

func (r *metaReader) u32() uint32 {
	x := r.u()

	if x > 1<<32-1 {
		throwFmt("malformed object metadata")
	}

	return uint32(x)
}

func (r *metaReader) flag() bool {
	return r.u() != 0
}

// count reads a list's length, at most one element per remaining byte.
func (r *metaReader) count() int {
	n := r.u()

	if n > uint64(len(r.data)) {
		throwFmt("malformed object metadata")
	}

	return int(n)
}

func (r *metaReader) str() string {
	n := r.count()
	s := string(r.data[:n])
	r.data = r.data[n:]

	return s
}

func (r *metaReader) types() []ValType {
	ts := make([]ValType, r.count())

	for i := range ts {
		ts[i] = ValType(r.u())
	}

	return ts
}

func decodeMeta(data []byte) *Meta {
	if len(data) < len(metaMagic) || string(data[:len(metaMagic)]) != metaMagic {
		throwFmt("object metadata is not %s", metaMagic)
	}

	r := &metaReader{data: data[len(metaMagic):]}
	m := &Meta{instanceSize: r.u32()}

	if r.flag() {
		m.memory = &MemoryMeta{min: r.u(), max: r.u(), imported: r.flag()}
	}

	m.tables = make([]TableMeta, r.count())

	for i := range m.tables {
		m.tables[i] = TableMeta{offset: r.u32(), min: r.u(), max: r.u(), imported: r.flag()}
	}

	m.globals = make([]GlobalMeta, r.count())

	for i := range m.globals {
		m.globals[i] = GlobalMeta{offset: r.u32(), typ: ValType(r.u()), mutable: r.flag(), imported: r.flag()}
	}

	m.imports = make([]ImportMeta, r.count())

	for i := range m.imports {
		m.imports[i] = ImportMeta{kind: byte(r.u()), module: r.str(), name: r.str(), index: r.u32(), sym: r.str(), params: r.types(), results: r.types()}
	}

	m.exports = make([]ExportMeta, r.count())

	for i := range m.exports {
		m.exports[i] = ExportMeta{kind: byte(r.u()), name: r.str(), index: r.u32(), sym: r.str(), params: r.types(), results: r.types()}
	}

	m.init = r.str()
	m.regions = make([]Region, r.count())

	for i := range m.regions {
		reg := Region{name: r.str(), start: r.u32(), end: r.u32()}
		reg.sp = make([]spEntry, r.count())

		for k := range reg.sp {
			reg.sp[k] = spEntry{pc: int32(r.u32()), delta: int32(r.s())}
		}

		m.regions[i] = reg
	}

	if len(r.data) != 0 {
		throwFmt("trailing object metadata")
	}

	return m
}
