package main

// The instance is one block of memory the host allocates. Its fixed header
// comes first; the module's globals, tables, function descriptors and
// segment flags follow at offsets the generated header names.
const (
	offMem        = 0  // uint8_t* base of linear memory
	offMemSize    = 8  // uint64_t size of linear memory in bytes
	offMemBound   = 16 // uint64_t size - boundSlack, at least 0: see mCheck
	offStackLimit = 24 // the lowest stack address generated code may use
	offHost       = 32 // void* for the host
	headerSize    = 48
)

// boundSlack is how far past a checked address an access may reach without
// a check of its own: an access at address a, offset o and width n is in
// bounds when a <= size - boundSlack and o + n <= boundSlack.
const boundSlack = 1 << 16

// Each table is {entries, size, max}: entries is an array of descriptor
// pointers, null for a null reference.
const (
	tableEntries = 0
	tableSize    = 8
	tableMax     = 12
	tableBytes   = 16
)

// A function descriptor is {code, type id}; a funcref is its address.
const descBytes = 16

type Layout struct {
	globals []int32
	tables  []int32
	descs   int32
	dropped int32
	size    int32
}

func newLayout(m *Module) *Layout {
	l := &Layout{}
	off := int32(headerSize)

	for _, g := range m.globals {
		if g.typ == V128 {
			off = (off + 15) &^ 15
		}

		l.globals = append(l.globals, off)
		off += 8

		if g.typ == V128 && !g.imported {
			off += 8
		}
	}

	off = (off + 15) &^ 15

	for range m.tables {
		l.tables = append(l.tables, off)
		off += tableBytes
	}

	l.descs = off
	off += int32(len(m.funcs)) * descBytes
	l.dropped = off
	off += int32(len(m.datas) + len(m.elems))
	l.size = (off + 15) &^ 15

	return l
}

func (l *Layout) desc(f int) int32 {
	return l.descs + int32(f)*descBytes
}

func (l *Layout) dataDropped(seg int) int32 {
	return l.dropped + int32(seg)
}

func (l *Layout) elemDropped(m *Module, seg int) int32 {
	return l.dropped + int32(len(m.datas)+seg)
}
