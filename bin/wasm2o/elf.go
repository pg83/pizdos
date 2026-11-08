package main

import (
	"encoding/binary"
	"sort"
	"strings"
)

// An object is ordinary x86-64 ELF, relocatable:
//
//	.text          the code: every region a function symbol, the thunks Go
//	               calls global (symbols.go)
//	.rela.text     R_X86_64_PC32 to .rodata, R_X86_64_GOTPCREL to the
//	               external symbols the code calls
//	.rodata        constants, data and element segments
//	.eh_frame      a CFA rule at every instruction of every region: the
//	               stack pointer tables Go's unwinder needs
//	.note.wasm2o   the module's description (meta.go)
//
// A linker may combine and relocate it; a loader needs nothing else.

// section types and flags
const (
	shtProgbits = 1
	shtSymtab   = 2
	shtStrtab   = 3
	shtRela     = 4
	shtNote     = 7
	shtNobits   = 8
	shfAlloc    = 2
	shfExec     = 4
	shfInfoLink = 0x40
)

// relocation types
const (
	rX86_64_64            = 1
	rX86_64_PC32          = 2
	rX86_64_GOTPCREL      = 9
	rX86_64_GOTPCRELX     = 41
	rX86_64_REX_GOTPCRELX = 42
)

const noteOwner = "wasm2o"

// Object is what a loader takes from an object, as wasm2o or a linker
// that combined such objects wrote it.
type Object struct {
	text   []byte
	rodata []byte
	// defined maps a symbol of the text to its offset; relocs are those
	// of the text.
	defined map[string]uint32
	relocs  []objReloc
	meta    *Meta
}

// Where relocations point: the text, .rodata, or an external symbol.
const (
	inExternal = iota
	inText
	inRodata
)

type objReloc struct {
	at     uint32
	typ    uint32
	in     int
	value  uint64
	sym    string
	addend int64
}

type elfSection struct {
	name, typ  uint32
	flags      uint64
	data       []byte
	link, info uint32
}

type elfSymbol struct {
	name    string
	value   uint64
	section int
	info    byte
}

func readObject(data []byte) *Object {
	le := binary.LittleEndian

	if len(data) < 64 || string(data[:4]) != "\x7fELF" || data[4] != 2 || le.Uint16(data[16:]) != 1 || le.Uint16(data[18:]) != 62 {
		throwFmt("not an x86-64 relocatable object")
	}

	shoff, count, names := le.Uint64(data[40:]), int(le.Uint16(data[60:])), int(le.Uint16(data[62:]))

	if shoff > uint64(len(data)) || uint64(count)*64 > uint64(len(data))-shoff || names >= count {
		throwFmt("malformed section headers")
	}

	sections := make([]elfSection, count)

	for i := range sections {
		h := data[shoff+uint64(64*i):]
		offset, size := le.Uint64(h[24:]), le.Uint64(h[32:])
		s := elfSection{name: le.Uint32(h), typ: le.Uint32(h[4:]), flags: le.Uint64(h[8:]), link: le.Uint32(h[40:]), info: le.Uint32(h[44:])}

		if s.typ != shtNobits {
			if offset > uint64(len(data)) || size > uint64(len(data))-offset {
				throwFmt("section %d out of the file", i)
			}

			s.data = data[offset : offset+size]
		}

		sections[i] = s
	}

	cstr := func(table []byte, at uint32) string {
		if uint64(at) >= uint64(len(table)) {
			throwFmt("string out of its table")
		}

		end := at

		for end < uint32(len(table)) && table[end] != 0 {
			end++
		}

		return string(table[at:end])
	}

	sectionName := func(i int) string {
		return cstr(sections[names].data, sections[i].name)
	}

	find := func(name string) int {
		for i := range sections {
			if sectionName(i) == name {
				return i
			}
		}

		return -1
	}

	text, symtab := find(".text"), find(".symtab")

	if text < 0 || symtab < 0 || sections[symtab].link >= uint32(count) || len(sections[symtab].data)%24 != 0 {
		throwFmt("no text or symbol table")
	}

	rodata := find(".rodata")
	obj := &Object{text: sections[text].data, defined: map[string]uint32{}}

	if rodata >= 0 {
		obj.rodata = sections[rodata].data
	}

	strtab := sections[sections[symtab].link].data
	var symbols []elfSymbol

	for at := 0; at < len(sections[symtab].data); at += 24 {
		e := sections[symtab].data[at:]
		s := elfSymbol{name: cstr(strtab, le.Uint32(e)), info: e[4], section: int(le.Uint16(e[6:])), value: le.Uint64(e[8:])}
		symbols = append(symbols, s)

		if s.section == text && s.info&0xf == 2 {
			if s.value > uint64(len(obj.text)) {
				throwFmt("symbol %s out of the text", s.name)
			}

			obj.defined[s.name] = uint32(s.value)
		}
	}

	// relocations of a section: what each points at
	relocations := func(of int) map[uint32]objReloc {
		out := map[uint32]objReloc{}

		for i, s := range sections {
			if s.typ != shtRela || int(s.info) != of {
				continue
			}

			if int(s.link) != symtab || len(s.data)%24 != 0 {
				throwFmt("malformed relocations in section %d", i)
			}

			for at := 0; at < len(s.data); at += 24 {
				e := s.data[at:]
				info := le.Uint64(e[8:])

				if info>>32 >= uint64(len(symbols)) {
					throwFmt("relocation to symbol %d", info>>32)
				}

				target := symbols[info>>32]
				r := objReloc{at: uint32(le.Uint64(e)), typ: uint32(info), addend: int64(le.Uint64(e[16:])), value: target.value}

				switch target.section {
				case 0:
					r.in, r.sym = inExternal, target.name
				case text:
					r.in = inText
				case rodata:
					r.in = inRodata
				default:
					throwFmt("relocation into section %s", sectionName(target.section))
				}

				out[r.at] = r
			}
		}

		return out
	}

	for _, r := range relocations(text) {
		obj.relocs = append(obj.relocs, r)
	}

	sort.Slice(obj.relocs, func(i, j int) bool { return obj.relocs[i].at < obj.relocs[j].at })
	note := find(".note.wasm2o")

	if note < 0 {
		throwFmt("no .note.wasm2o: not an object of wasm2o")
	}

	obj.meta = decodeNote(readNote(sections[note].data))
	eh := find(".eh_frame")

	if eh < 0 {
		throwFmt("no .eh_frame")
	}

	obj.meta.regions = readEhFrame(sections[eh].data, relocations(eh), obj)
	obj.describeSymbols(symbols, text)

	return obj
}

func readNote(data []byte) []byte {
	le := binary.LittleEndian

	for len(data) >= 12 {
		namesz, descsz, typ := le.Uint32(data), le.Uint32(data[4:]), le.Uint32(data[8:])
		name := alignTo(int32(namesz), 4)
		desc := alignTo(int32(descsz), 4)

		if uint64(12+name)+uint64(desc) > uint64(len(data)) {
			break
		}

		if typ == 1 && string(data[12:12+namesz]) == noteOwner+"\x00" {
			return data[12+name : 12+uint32(name)+descsz]
		}

		data = data[12+name+desc:]
	}

	throwFmt("no wasm2o note")

	return nil
}

// readEhFrame takes the regions from the FDEs: their ranges, and the stack
// pointer, CFA less 8, along each.
func readEhFrame(data []byte, relocs map[uint32]objReloc, obj *Object) []Region {
	le := binary.LittleEndian
	type cie struct {
		code uint64
		cfa  int64
	}

	cies := map[int]cie{}
	var regions []Region
	uleb := func(b []byte, at *int) uint64 {
		var x uint64
		var shift uint

		for {
			if *at >= len(b) {
				throwFmt("malformed .eh_frame")
			}

			c := b[*at]
			*at++
			x |= uint64(c&0x7f) << shift
			shift += 7

			if c&0x80 == 0 {
				return x
			}
		}
	}

	for off := 0; off+4 <= len(data); {
		length := int(le.Uint32(data[off:]))

		if length == 0 {
			break
		}

		start := off + 4
		end := start + length

		if length == 0xffffffff || end > len(data) {
			throwFmt("malformed .eh_frame")
		}

		e := data[start:end]
		off = end

		if le.Uint32(e) == 0 {
			// a CIE: version, augmentation "zR" or "", alignments, return
			// register; an FDE points back at its CIE's length
			at := 5
			aug := ""

			for at < len(e) && e[at] != 0 {
				aug += string(e[at])
				at++
			}

			at++
			c := cie{code: uleb(e, &at)}
			uleb(e, &at)
			uleb(e, &at)

			if strings.HasPrefix(aug, "z") {
				n := int(uleb(e, &at))

				if aug != "zR" || n != 1 || e[at] != 0x1b {
					throwFmt("unsupported .eh_frame augmentation %q", aug)
				}

				at += n
			}

			c.cfa = cfaProgram(e[at:], c.code, 8, nil, 0)
			cies[start-4] = c

			continue
		}

		c, ok := cies[start-int(le.Uint32(e))]
		r, rok := relocs[uint32(start+4)]

		if !ok || !rok || r.in != inText || r.typ != rX86_64_PC32 || len(e) < 13 {
			throwFmt("unsupported FDE at %#x", start)
		}

		pc := uint32(int64(r.value) + r.addend)
		size := le.Uint32(e[8:])
		at := 12
		at += int(uleb(e, &at))
		var sp []spEntry
		cfaProgram(e[at:], c.code, c.cfa, &sp, size)
		regions = append(regions, Region{start: pc, end: pc + size, sp: sp})
	}

	sort.Slice(regions, func(i, j int) bool { return regions[i].start < regions[j].start })
	names := map[uint32]string{}

	for name, at := range obj.defined {
		if old, ok := names[at]; !ok || name < old {
			names[at] = name
		}
	}

	for k := range regions {
		regions[k].name = names[regions[k].start]

		if regions[k].name == "" {
			regions[k].name = "w2o.code"
		}
	}

	return regions
}

// cfaProgram runs the CFA instructions wasm2o writes, starting from a CFA of
// rsp+cfa, and returns the CFA offset at the end; with sp it records the
// stack pointer, CFA less 8, at every change, from offset 0 of a range of
// size bytes.
func cfaProgram(b []byte, code uint64, cfa int64, sp *[]spEntry, size uint32) int64 {
	le := binary.LittleEndian
	loc := uint64(0)
	record := func() {
		if sp == nil {
			return
		}

		d := int32(cfa - 8)

		if n := len(*sp); n > 0 && (*sp)[n-1].pc == int32(loc) {
			(*sp)[n-1].delta = d
		} else if n == 0 || (*sp)[n-1].delta != d {
			*sp = append(*sp, spEntry{int32(loc), d})
		}

		if n := len(*sp); n > 1 && (*sp)[n-2].delta == (*sp)[n-1].delta {
			*sp = (*sp)[:n-1]
		}
	}

	record()

	for at := 0; at < len(b); {
		op := b[at]
		at++
		next := func() uint64 {
			var x uint64
			var shift uint

			for {
				c := b[at]
				at++
				x |= uint64(c&0x7f) << shift
				shift += 7

				if c&0x80 == 0 {
					return x
				}
			}
		}

		switch {
		case op&0xc0 == 0x40:
			loc += uint64(op&0x3f) * code
		case op&0xc0 == 0x80:
			next()

			continue
		case op&0xc0 == 0xc0, op == 0:
			continue
		case op == 2:
			loc += uint64(b[at]) * code
			at++
		case op == 3:
			loc += uint64(le.Uint16(b[at:])) * code
			at += 2
		case op == 4:
			loc += uint64(le.Uint32(b[at:])) * code
			at += 4
		case op == 0x0c:
			if next() != 7 {
				throwFmt("a CFA not on rsp")
			}

			cfa = int64(next())
			record()

			continue
		case op == 0x0e:
			cfa = int64(next())
			record()

			continue
		default:
			throwFmt("unsupported CFA instruction %#x", op)
		}

		if sp != nil && loc >= uint64(size) {
			throwFmt("CFA instructions past their range")
		}
	}

	return cfa
}

// describeSymbols adds what the symbols tell to the description: the
// exports and imports of functions, and the init thunk.
func (obj *Object) describeSymbols(symbols []elfSymbol, text int) {
	m := obj.meta
	m.init = symInit

	if _, ok := obj.defined[symInit]; !ok {
		throwFmt("no %s", symInit)
	}

	seen := map[string]bool{}

	for _, s := range symbols {
		if seen[s.name] {
			continue
		}

		seen[s.name] = true

		if names, t, ok := parseFuncSymbol(s.name, symExport, 1); ok && s.section == text {
			m.exports = append(m.exports, ExportMeta{kind: externFunc, name: names[0], sym: s.name, params: t.params, results: t.results})
		}

		if names, t, ok := parseFuncSymbol(s.name, symImport, 2); ok && s.section == 0 {
			m.imports = append(m.imports, ImportMeta{kind: externFunc, module: names[0], name: names[1], sym: s.name, params: t.params, results: t.results})
		}
	}
}
