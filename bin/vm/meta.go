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
	min, max uint64
	imported bool
	tail     uint64 // accessible bytes beyond memory.size; zero for strict objects
	tailEnd  uint32 // instance slot holding the accessible end
	// profile is the instance slot of the tail profile's first counter and
	// sites what each one counts: a profiled object's only.
	profile uint32
	sites   []ProfileSite
	// shadow is the instance slot holding the shadow map's base: an asan
	// object (kernel_asan.go), which has no tail.
	shadow uint32
}

// ProfileSite is a counter of the tail profile: of function's code, of kind
// (siteKinds), at the wasm instruction at offset in the module, 0 for none.
type ProfileSite struct {
	function, kind, offset uint32
}

// siteKinds names the kinds of sites by the compiler's numbers (aot.cpp
// siteCheck...): what each counts (tail_profile.go).
var siteKinds = [...]string{"check", "guard", "fallback", "omitted", "loop", "entry", "fast_loop", "fast_entry"}

const (
	siteCheck = iota
	siteGuard
	siteFallback
	siteOmitted
)

// asan tells an object compiled with --asan.
func (m *Meta) asan() bool {
	return m.memory != nil && m.memory.shadow != 0
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

// lineEntry says that from pc of the text on the code is of line of the
// file, an index of Meta's files: the module's DWARF line table, carried
// over (aot.cpp objCodeLines) into Go's tables (registerCode), so a trace
// names the source line of a frame of the module.
type lineEntry struct {
	pc, file, line uint32
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
	// files and lines are the source lines of the text, in order of pc;
	// empty for a module without a DWARF line table.
	files []string
	lines []lineEntry
	// functions is the first descriptor in the instance; numeric spec
	// funcrefs name entries of this array, rather than host addresses.
	functions, functionCount uint32
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

	// What follows is optional, by its flags: the accessible tail and its
	// slot, the tail's profile counters, the shadow map's slot, the source
	// lines, function descriptors.
	flags := r.u()

	if flags&7 != 0 && m.memory == nil {
		throwFmt("memory metadata without a memory")
	}

	if flags&1 != 0 {
		m.memory.tail = r.u()
		m.memory.tailEnd = r.u32()

		if m.memory.tail != boundSlack || m.memory.tailEnd&7 != 0 || uint64(m.memory.tailEnd)+8 > uint64(m.instanceSize) {
			throwFmt("unsupported memory tail %d", m.memory.tail)
		}
	}

	if flags&2 != 0 {
		m.memory.profile = r.u32()
		m.memory.sites = make([]ProfileSite, r.count())

		for k := range m.memory.sites {
			m.memory.sites[k] = ProfileSite{function: r.u32(), kind: r.u32(), offset: r.u32()}

			if m.memory.sites[k].kind >= uint32(len(siteKinds)) {
				throwFmt("invalid memory tail profile")
			}
		}

		if m.memory.tail == 0 || m.memory.profile < headerSize || m.memory.profile&7 != 0 || uint64(m.memory.profile)+uint64(len(m.memory.sites))*profileCounterBytes > uint64(m.instanceSize) {
			throwFmt("invalid memory tail profile")
		}
	}

	if flags&4 != 0 {
		m.memory.shadow = r.u32()

		if m.memory.tail != 0 || m.memory.shadow < headerSize || m.memory.shadow&7 != 0 || uint64(m.memory.shadow)+8 > uint64(m.instanceSize) {
			throwFmt("invalid shadow slot %d", m.memory.shadow)
		}
	}

	if flags&8 != 0 {
		m.files = make([]string, r.count())

		for i := range m.files {
			m.files[i] = r.str()
		}

		m.lines = make([]lineEntry, r.count())
		pc := uint32(0)

		for i := range m.lines {
			pc += r.u32()
			m.lines[i] = lineEntry{pc: pc, file: r.u32(), line: r.u32()}

			if uint64(m.lines[i].file) >= uint64(len(m.files)) || i > 0 && pc == m.lines[i-1].pc {
				throwFmt("invalid line table")
			}
		}
	}

	if flags&16 != 0 {
		m.functions, m.functionCount = r.u32(), r.u32()

		if m.functions < headerSize || m.functions&15 != 0 || m.functionCount == 0 || uint64(m.functions)+uint64(m.functionCount)*descBytes > uint64(m.instanceSize) {
			throwFmt("invalid function descriptors")
		}
	}

	if len(r.data) != 0 {
		throwFmt("trailing object metadata")
	}

	return m
}
