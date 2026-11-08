package main

import "encoding/binary"

// Meta is what a loader knows of a module. An object tells it in ordinary
// ELF (elf.go): functions are symbols, imports and exports of functions
// symbols with their types (symbols.go), stack pointer tables .eh_frame.
// The rest has no word in ELF and is the descriptor of a note, owner
// "wasm2o", type 1: the instance's size, the memory, tables and globals,
// the imports and exports of those. Its integers are unsigned LEB128; a
// string or a list is its length, then its bytes or elements, in the order
// of the fields below.

type MemoryMeta struct {
	// min and max are in pages of 64 KiB.
	min, max     uint64
	imported     bool
	tail         uint64 // accessible bytes beyond memory.size; zero for strict objects
	tailEnd      uint32 // instance slot holding the accessible end
	profile      uint32 // optional diagnostic counters in the instance
	profileFuncs uint32
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

// encodeNote writes the descriptor: imports and exports of functions, the
// init symbol and the regions are not in it.
func encodeNote(m *Meta) []byte {
	w := &metaWriter{}
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
	}

	w.u(uint64(len(m.exports)))

	for _, e := range m.exports {
		w.u(uint64(e.kind))
		w.str(e.name)
		w.u(uint64(e.index))
	}

	// Strict objects keep their original descriptor. An older loader
	// rejects the extension instead of running padded accesses unsafely.
	if m.memory != nil && m.memory.tail != 0 {
		w.u(m.memory.tail)
		w.u(uint64(m.memory.tailEnd))

		if m.memory.profile != 0 {
			w.u(uint64(m.memory.profile))
			w.u(uint64(m.memory.profileFuncs))
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

func decodeNote(data []byte) *Meta {
	r := &metaReader{data: data}
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
		m.imports[i] = ImportMeta{kind: byte(r.u()), module: r.str(), name: r.str(), index: r.u32()}
	}

	m.exports = make([]ExportMeta, r.count())

	for i := range m.exports {
		m.exports[i] = ExportMeta{kind: byte(r.u()), name: r.str(), index: r.u32()}
	}

	if len(r.data) != 0 && m.memory != nil {
		m.memory.tail = r.u()
		m.memory.tailEnd = r.u32()

		if m.memory.tail != boundSlack || m.memory.tailEnd&7 != 0 || uint64(m.memory.tailEnd)+8 > uint64(m.instanceSize) {
			throwFmt("unsupported memory tail %d", m.memory.tail)
		}

		if len(r.data) != 0 {
			m.memory.profile, m.memory.profileFuncs = r.u32(), r.u32()

			if m.memory.profile < headerSize || m.memory.profile&7 != 0 || uint64(m.memory.profile)+uint64(m.memory.profileFuncs)*tailCounterBytes > uint64(m.instanceSize) {
				throwFmt("invalid memory tail profile")
			}
		}
	}

	if len(r.data) != 0 {
		throwFmt("trailing object metadata")
	}

	return m
}
