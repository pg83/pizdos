package main

import (
	"math"
	"unicode/utf8"
)

// ValType is a value type in its binary encoding.
type ValType uint8

const (
	I32       ValType = 0x7f
	I64       ValType = 0x7e
	F32       ValType = 0x7d
	F64       ValType = 0x7c
	V128      ValType = 0x7b
	FuncRef   ValType = 0x70
	ExternRef ValType = 0x6f
)

const (
	externFunc   = 0
	externTable  = 1
	externMemory = 2
	externGlobal = 3
)

const (
	segmentActive      = 0
	segmentPassive     = 1
	segmentDeclarative = 2
)

type FuncType struct {
	params  []ValType
	results []ValType
}

type Limits struct {
	min    uint64
	max    uint64
	hasMax bool
	is64   bool
	shared bool
}

type Import struct {
	module string
	name   string
	kind   byte
	// index is the imported entity's index in its own index space.
	index uint32
}

type Func struct {
	typ      uint32
	imported bool
	// returnsTwice marks an import that returns twice (goabi.go).
	returnsTwice bool
	name         string
	// body holds the local declarations and the code, without the size.
	body []byte
	// offset is the body's position in the module, for diagnostics.
	offset int
}

type Table struct {
	elem     ValType
	limits   Limits
	imported bool
}

type Memory struct {
	limits   Limits
	imported bool
}

type Global struct {
	typ      ValType
	mutable  bool
	imported bool
	// init is a constant expression including its end opcode.
	init []byte
}

type Export struct {
	name  string
	kind  byte
	index uint32
}

type Elem struct {
	mode  byte
	table uint32
	// offset is the constant expression of an active segment.
	offset []byte
	typ    ValType
	// exprs holds one constant expression per element.
	exprs [][]byte
}

type Data struct {
	mode   byte
	memory uint32
	offset []byte
	bytes  []byte
}

type Module struct {
	memoryTail  uint64
	tailRanges  bool
	tailProfile bool
	types       []FuncType
	imports     []Import
	funcs       []Func
	tables      []Table
	mems        []Memory
	globals     []Global
	exports     []Export
	start       int64
	elems       []Elem
	datas       []Data
	dataCount   int64
	// counts of imported entities, which come first in each index space
	importedFuncs   int
	importedTables  int
	importedMems    int
	importedGlobals int
}

type reader struct {
	data []byte
	pos  int
}

func (r *reader) eof() bool {
	return r.pos >= len(r.data)
}

func (r *reader) byte() byte {
	if r.pos >= len(r.data) {
		throwFmt("unexpected end at offset %d", r.pos)
	}

	b := r.data[r.pos]
	r.pos++

	return b
}

func (r *reader) bytes(n int) []byte {
	if n < 0 || n > len(r.data)-r.pos {
		throwFmt("unexpected end at offset %d: need %d bytes", r.pos, n)
	}

	b := r.data[r.pos : r.pos+n]
	r.pos += n

	return b
}

// leb reads an unsigned LEB128 number of at most bits bits.
func (r *reader) leb(bits uint) uint64 {
	var result uint64
	var shift uint

	for {
		b := r.byte()

		if shift+7 >= bits && b&0x7f>>(bits-shift) != 0 {
			throwFmt("integer too large at offset %d", r.pos-1)
		}

		result |= uint64(b&0x7f) << shift

		if b&0x80 == 0 {
			return result
		}

		shift += 7

		if shift >= bits {
			throwFmt("integer representation too long at offset %d", r.pos)
		}
	}
}

// sleb reads a signed LEB128 number of at most bits bits.
func (r *reader) sleb(bits uint) int64 {
	var result int64
	var shift uint

	for {
		b := r.byte()

		if shift+7 >= bits {
			// The unused bits of the last byte must sign-extend the value.
			rest := int8(b<<1) >> (bits - shift)

			if b&0x80 != 0 || (rest != 0 && rest != -1) {
				if b&0x80 != 0 {
					throwFmt("integer representation too long at offset %d", r.pos)
				}

				throwFmt("integer too large at offset %d", r.pos-1)
			}
		}

		result |= int64(b&0x7f) << shift
		shift += 7

		if b&0x80 == 0 {
			if shift < 64 && b&0x40 != 0 {
				result |= -1 << shift
			}

			return result
		}
	}
}

func (r *reader) u32() uint32 {
	if r.pos < len(r.data) && r.data[r.pos] < 0x80 {
		b := r.data[r.pos]
		r.pos++

		return uint32(b)
	}

	return uint32(r.leb(32))
}

func (r *reader) s32() int32 {
	return int32(r.sleb(32))
}

func (r *reader) s64() int64 {
	return r.sleb(64)
}

func (r *reader) f32() uint32 {
	b := r.bytes(4)

	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
}

func (r *reader) f64() uint64 {
	b := r.bytes(8)

	return uint64(r.le32(b[0:4])) | uint64(r.le32(b[4:8]))<<32
}

func (r *reader) le32(b []byte) uint32 {
	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
}

func (r *reader) name() string {
	n := r.u32()
	b := r.bytes(int(n))

	if !utf8.Valid(b) {
		throwFmt("malformed UTF-8 encoding at offset %d", r.pos)
	}

	return string(b)
}

func (r *reader) valType() ValType {
	t := ValType(r.byte())

	switch t {
	case I32, I64, F32, F64, V128, FuncRef, ExternRef:
		return t
	}

	throwFmt("malformed value type 0x%02x at offset %d", byte(t), r.pos-1)

	return 0
}

func (r *reader) refType() ValType {
	t := ValType(r.byte())

	if t != FuncRef && t != ExternRef {
		throwFmt("malformed reference type 0x%02x at offset %d", byte(t), r.pos-1)
	}

	return t
}

func (r *reader) limits(memory bool) Limits {
	flags := r.byte()
	var l Limits

	if flags > 7 || (!memory && flags > 1) {
		throwFmt("integer too large: limits flags 0x%02x", flags)
	}

	l.hasMax = flags&1 != 0
	l.shared = flags&2 != 0
	l.is64 = flags&4 != 0

	// Bounds checks compare 32-bit addresses: a 64-bit one could pass them.
	if l.is64 {
		throwFmt("unsupported: memory64")
	}

	bits := uint(32)

	if l.is64 {
		bits = 64
	}

	l.min = r.leb(bits)

	if l.hasMax {
		l.max = r.leb(bits)
	}

	return l
}

// constExpr returns the bytes of a constant expression up to and including
// its end opcode.
func (r *reader) constExpr() []byte {
	start := r.pos

	for {
		op := r.byte()

		switch op {
		case 0x0b:
			return r.data[start:r.pos]
		case 0x41:
			r.s32()
		case 0x42:
			r.s64()
		case 0x43:
			r.bytes(4)
		case 0x44:
			r.bytes(8)
		case 0x23, 0xd2:
			r.u32()
		case 0xd0:
			r.refType()
		case 0x6a, 0x6b, 0x6c, 0x7c, 0x7d, 0x7e:
		case 0xfd:
			if r.u32() != 12 {
				throwFmt("constant expression required at offset %d", r.pos)
			}

			r.bytes(16)
		default:
			throwFmt("constant expression required: opcode 0x%02x at offset %d", op, r.pos-1)
		}
	}
}

func decodeModule(data []byte) *Module {
	r := &reader{data: data}
	m := &Module{start: -1, dataCount: -1}

	if string(r.bytes(4)) != "\x00asm" {
		throwFmt("magic header not detected")
	}

	if r.f32() != 1 {
		throwFmt("unknown binary version")
	}

	var funcTypes []uint32
	var codes int
	last := byte(0)

	for !r.eof() {
		id := r.byte()
		size := r.u32()
		body := &reader{data: r.bytes(int(size))}
		base := r.pos - int(size)

		if id != 0 {
			// datacount (12) sits between import/.../element and code.
			if order(id) <= order(last) {
				throwFmt("unexpected content after last section")
			}

			last = id
		}

		switch id {
		case 0:
			m.custom(body)
		case 1:
			m.typeSection(body)
		case 2:
			m.importSection(body)
		case 3:
			n := body.u32()

			for i := uint32(0); i < n; i++ {
				funcTypes = append(funcTypes, body.u32())
			}
		case 4:
			n := body.u32()

			for i := uint32(0); i < n; i++ {
				elem := body.refType()
				m.tables = append(m.tables, Table{elem: elem, limits: body.limits(false)})
			}
		case 5:
			n := body.u32()

			for i := uint32(0); i < n; i++ {
				m.mems = append(m.mems, Memory{limits: body.limits(true)})
			}
		case 6:
			n := body.u32()

			for i := uint32(0); i < n; i++ {
				g := Global{typ: body.valType()}
				g.mutable = globalMutability(body)
				g.init = body.constExpr()
				m.globals = append(m.globals, g)
			}
		case 7:
			m.exportSection(body)
		case 8:
			m.start = int64(body.u32())
		case 9:
			m.elemSection(body)
		case 10:
			n := int(body.u32())

			if n != len(funcTypes) {
				throwFmt("function and code section have inconsistent lengths")
			}

			for i := 0; i < n; i++ {
				size := int(body.u32())
				offset := base + body.pos
				m.funcs = append(m.funcs, Func{typ: funcTypes[i], body: body.bytes(size), offset: offset})
			}

			codes = n
		case 11:
			m.dataSection(body)
		case 12:
			m.dataCount = int64(body.u32())
		default:
			throwFmt("malformed section id %d", id)
		}

		if !body.eof() {
			throwFmt("section size mismatch in section %d", id)
		}
	}

	if codes != len(funcTypes) {
		throwFmt("function and code section have inconsistent lengths")
	}

	if m.dataCount >= 0 && int(m.dataCount) != len(m.datas) {
		throwFmt("data count and data section have inconsistent lengths")
	}

	for i := range m.funcs {
		if int(m.funcs[i].typ) >= len(m.types) {
			throwFmt("unknown type %d", m.funcs[i].typ)
		}
	}

	// The instance has one memory: its base is the memory register.
	if len(m.mems) > 1 {
		throwFmt("unsupported: %d memories", len(m.mems))
	}

	for i, d := range m.datas {
		if d.memory != 0 {
			throwFmt("unsupported: data segment %d of memory %d", i, d.memory)
		}
	}

	return m
}

// order ranks section ids in their required order; datacount goes before
// code.
func order(id byte) int {
	switch id {
	case 12:
		return 10
	case 10, 11:
		return int(id) + 1
	}

	return int(id)
}

func globalMutability(r *reader) bool {
	b := r.byte()

	if b > 1 {
		throwFmt("malformed mutability 0x%02x", b)
	}

	return b == 1
}

func (m *Module) custom(r *reader) {
	name := r.name()

	if name == "name" {
		// A broken name section must not make the module fail to compile.
		try(func() {
			m.nameSection(r)
		})
	}

	r.pos = len(r.data)
}

func (m *Module) nameSection(r *reader) {
	for !r.eof() {
		id := r.byte()
		size := r.u32()
		sub := &reader{data: r.bytes(int(size))}

		if id != 1 {
			continue
		}

		n := sub.u32()

		for i := uint32(0); i < n; i++ {
			index := sub.u32()
			name := sub.name()

			if int(index) < len(m.funcs) {
				m.funcs[index].name = name
			}
		}
	}
}

func (m *Module) typeSection(r *reader) {
	n := r.u32()

	for i := uint32(0); i < n; i++ {
		if form := r.byte(); form != 0x60 {
			throwFmt("malformed function type form 0x%02x", form)
		}

		var t FuncType
		np := r.u32()

		for j := uint32(0); j < np; j++ {
			t.params = append(t.params, r.valType())
		}

		nr := r.u32()

		for j := uint32(0); j < nr; j++ {
			t.results = append(t.results, r.valType())
		}

		m.types = append(m.types, t)
	}
}

func (m *Module) importSection(r *reader) {
	n := r.u32()

	for i := uint32(0); i < n; i++ {
		imp := Import{module: r.name(), name: r.name(), kind: r.byte()}

		switch imp.kind {
		case externFunc:
			imp.index = uint32(len(m.funcs))
			m.funcs = append(m.funcs, Func{typ: r.u32(), imported: true, returnsTwice: returnsTwice(imp.name)})
			m.importedFuncs++
		case externTable:
			imp.index = uint32(len(m.tables))
			elem := r.refType()
			m.tables = append(m.tables, Table{elem: elem, limits: r.limits(false), imported: true})
			m.importedTables++
		case externMemory:
			imp.index = uint32(len(m.mems))
			m.mems = append(m.mems, Memory{limits: r.limits(true), imported: true})
			m.importedMems++
		case externGlobal:
			imp.index = uint32(len(m.globals))
			g := Global{typ: r.valType(), imported: true}
			g.mutable = globalMutability(r)
			m.globals = append(m.globals, g)
			m.importedGlobals++
		default:
			throwFmt("malformed import kind %d", imp.kind)
		}

		m.imports = append(m.imports, imp)
	}
}

func (m *Module) exportSection(r *reader) {
	n := r.u32()
	seen := map[string]bool{}

	for i := uint32(0); i < n; i++ {
		e := Export{name: r.name(), kind: r.byte(), index: r.u32()}

		if e.kind > externGlobal {
			throwFmt("malformed export kind %d", e.kind)
		}

		if seen[e.name] {
			throwFmt("duplicate export name %q", e.name)
		}

		seen[e.name] = true
		m.exports = append(m.exports, e)
	}
}

func (m *Module) elemSection(r *reader) {
	n := r.u32()

	for i := uint32(0); i < n; i++ {
		flags := r.u32()

		if flags > 7 {
			throwFmt("malformed elements segment kind %d", flags)
		}

		e := Elem{typ: FuncRef}

		switch {
		case flags&1 == 0:
			e.mode = segmentActive
		case flags&2 == 0:
			e.mode = segmentPassive
		default:
			e.mode = segmentDeclarative
		}

		if flags&1 == 0 {
			if flags&2 != 0 {
				e.table = r.u32()
			}

			e.offset = r.constExpr()
		}

		exprs := flags&4 != 0

		if flags&3 != 0 {
			if exprs {
				e.typ = r.refType()
			} else if kind := r.byte(); kind != 0 {
				throwFmt("malformed element kind %d", kind)
			}
		}

		count := r.u32()

		for j := uint32(0); j < count; j++ {
			if exprs {
				e.exprs = append(e.exprs, r.constExpr())
			} else {
				e.exprs = append(e.exprs, refFuncExpr(r.u32()))
			}
		}

		m.elems = append(m.elems, e)
	}
}

// refFuncExpr is the constant expression ref.func index.
func refFuncExpr(index uint32) []byte {
	expr := []byte{0xd2}
	expr = appendLeb(expr, uint64(index))

	return append(expr, 0x0b)
}

func (m *Module) dataSection(r *reader) {
	n := r.u32()

	for i := uint32(0); i < n; i++ {
		flags := r.u32()
		d := Data{}

		switch flags {
		case 0:
			d.offset = r.constExpr()
		case 1:
			d.mode = segmentPassive
		case 2:
			d.memory = r.u32()
			d.offset = r.constExpr()
		default:
			throwFmt("malformed data segment kind %d", flags)
		}

		d.bytes = r.bytes(int(r.u32()))
		m.datas = append(m.datas, d)
	}
}

func appendLeb(b []byte, v uint64) []byte {
	for {
		c := byte(v & 0x7f)
		v >>= 7

		if v == 0 {
			return append(b, c)
		}

		b = append(b, c|0x80)
	}
}

func appendSleb(b []byte, v int64) []byte {
	for {
		c := byte(v & 0x7f)
		v >>= 7

		if (v == 0 && c&0x40 == 0) || (v == -1 && c&0x40 != 0) {
			return append(b, c)
		}

		b = append(b, c|0x80)
	}
}

func (m *Module) funcType(index uint32) *FuncType {
	return &m.types[m.funcs[index].typ]
}

// typeID is a structural identity of a function type, equal for equal
// signatures across modules: call_indirect compares it at run time.
func typeID(t *FuncType) uint64 {
	// three bits per value type, a separator between params and results
	id := uint64(1)
	bits := 1

	push := func(code uint64) {
		id = id<<3 | code
		bits += 3
	}

	for _, p := range t.params {
		push(valTypeCode(p))
	}

	push(0)

	for _, r := range t.results {
		push(valTypeCode(r))
	}

	if bits <= 63 {
		return id
	}

	// Too long to spell out: hash the encoding, with the top bit set so it
	// never equals a spelled-out identity.
	h := uint64(14695981039346656037)

	for _, p := range t.params {
		h = (h ^ valTypeCode(p)) * 1099511628211
	}

	h = (h ^ 7) * 1099511628211

	for _, r := range t.results {
		h = (h ^ valTypeCode(r)) * 1099511628211
	}

	return h | 1<<63
}

func valTypeCode(t ValType) uint64 {
	switch t {
	case I32:
		return 1
	case I64:
		return 2
	case F32:
		return 3
	case F64:
		return 4
	case V128:
		return 5
	case FuncRef:
		return 6
	}

	return 7
}

func f32bits(v float32) uint32 {
	return math.Float32bits(v)
}
