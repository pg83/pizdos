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

func appendUleb(out []byte, x uint64) []byte {
	return appendLeb(out, x)
}

// ehFrame writes a CIE and an FDE per region, and the offsets of the FDEs'
// starting addresses, which a relocation to the text fills.
func ehFrame(regions []Region) ([]byte, []uint32) {
	var out []byte
	var starts []uint32

	entry := func(body []byte) {
		for (4+len(body))%8 != 0 {
			body = append(body, 0) // DW_CFA_nop
		}

		out = binary.LittleEndian.AppendUint32(out, uint32(len(body)))
		out = append(out, body...)
	}

	// version 1, "zR": FDE addresses are pc-relative sdata4; code
	// alignment 1, data alignment -8, return address in register 16. At
	// entry the CFA is rsp+8 and the return address is at CFA-8.
	cie := []byte{0, 0, 0, 0, 1, 'z', 'R', 0, 1, 0x78, 16, 1, 0x1b, 0x0c, 7, 8, 0x90, 1}
	entry(cie)

	for _, r := range regions {
		at := len(out)
		body := binary.LittleEndian.AppendUint32(nil, uint32(at+4))
		starts = append(starts, uint32(at+8))
		body = binary.LittleEndian.AppendUint32(body, 0)
		body = binary.LittleEndian.AppendUint32(body, r.end-r.start)
		body = append(body, 0)
		loc := int32(0)

		for k, e := range r.sp {
			if k == 0 && e.delta == 0 {
				continue
			}

			switch d := e.pc - loc; {
			case d == 0:
			case d < 64:
				body = append(body, 0x40|byte(d))
			case d < 256:
				body = append(body, 2, byte(d))
			case d < 65536:
				body = binary.LittleEndian.AppendUint16(append(body, 3), uint16(d))
			default:
				body = binary.LittleEndian.AppendUint32(append(body, 4), uint32(d))
			}

			loc = e.pc
			body = appendUleb(append(body, 0x0e), uint64(e.delta+8))
		}

		entry(body)
	}

	return out, starts
}

func writeELF(oc *objCode, note []byte) []byte {
	const (
		secText = 1 + iota
		secRelaText
		secRodata
		secEhFrame
		secRelaEhFrame
		secNoteModule
		secSymtab
		secStrtab
		secShstrtab
		secNoteStack
		secCount
	)

	var strtab []byte
	str := func(s string) uint32 {
		off := uint32(len(strtab))
		strtab = append(strtab, s...)
		strtab = append(strtab, 0)

		return off
	}

	str("")
	var symtab []byte
	sym := func(name uint32, info byte, shndx uint16, value, size uint64) {
		symtab = binary.LittleEndian.AppendUint32(symtab, name)
		symtab = append(symtab, info, 0)
		symtab = binary.LittleEndian.AppendUint16(symtab, shndx)
		symtab = binary.LittleEndian.AppendUint64(symtab, value)
		symtab = binary.LittleEndian.AppendUint64(symtab, size)
	}

	const (
		stbLocal  = 0
		stbGlobal = 1
		sttNotype = 0
		sttFunc   = 2
		sttSect   = 3
		symText   = 1
		symRodata = 2
	)

	sym(0, 0, 0, 0, 0)
	sym(0, stbLocal<<4|sttSect, secText, 0, 0)
	sym(0, stbLocal<<4|sttSect, secRodata, 0, 0)
	count := 3

	for _, r := range oc.regions {
		if !oc.globals[r.name] {
			sym(str(r.name), stbLocal<<4|sttFunc, secText, uint64(r.start), uint64(r.end-r.start))
			count++
		}
	}

	firstGlobal := count

	for _, r := range oc.regions {
		if oc.globals[r.name] {
			sym(str(r.name), stbGlobal<<4|sttFunc, secText, uint64(r.start), uint64(r.end-r.start))
			count++
		}
	}

	externBase := count

	for _, name := range oc.externs {
		sym(str(name), stbGlobal<<4|sttNotype, 0, 0, 0)
	}

	rela := func(out []byte, at uint64, symbol int, typ uint32, addend int64) []byte {
		out = binary.LittleEndian.AppendUint64(out, at)
		out = binary.LittleEndian.AppendUint64(out, uint64(symbol)<<32|uint64(typ))

		return binary.LittleEndian.AppendUint64(out, uint64(addend))
	}

	var relaText []byte

	for _, r := range oc.relocs {
		if r.sym < 0 {
			relaText = rela(relaText, uint64(r.at), symRodata, rX86_64_PC32, r.addend)
		} else {
			relaText = rela(relaText, uint64(r.at), externBase+int(r.sym), rX86_64_GOTPCREL, r.addend)
		}
	}

	eh, starts := ehFrame(oc.regions)
	var relaEh []byte

	for k, at := range starts {
		relaEh = rela(relaEh, uint64(at), symText, rX86_64_PC32, int64(oc.regions[k].start))
	}

	noteBody := binary.LittleEndian.AppendUint32(nil, uint32(len(noteOwner)+1))
	noteBody = binary.LittleEndian.AppendUint32(noteBody, uint32(len(note)))
	noteBody = binary.LittleEndian.AppendUint32(noteBody, 1)
	noteBody = append(append(noteBody, noteOwner...), 0)

	for len(noteBody)%4 != 0 {
		noteBody = append(noteBody, 0)
	}

	noteBody = append(noteBody, note...)

	for len(noteBody)%4 != 0 {
		noteBody = append(noteBody, 0)
	}

	var shstrtab []byte
	sh := func(s string) uint32 {
		off := uint32(len(shstrtab))
		shstrtab = append(shstrtab, s...)
		shstrtab = append(shstrtab, 0)

		return off
	}

	sh("")
	names := [secCount]uint32{}

	for _, sec := range []struct {
		index int
		name  string
	}{
		{secText, ".text"}, {secRelaText, ".rela.text"}, {secRodata, ".rodata"}, {secEhFrame, ".eh_frame"},
		{secRelaEhFrame, ".rela.eh_frame"}, {secNoteModule, ".note.wasm2o"}, {secSymtab, ".symtab"},
		{secStrtab, ".strtab"}, {secShstrtab, ".shstrtab"}, {secNoteStack, ".note.GNU-stack"},
	} {
		names[sec.index] = sh(sec.name)
	}

	out := make([]byte, 64)
	var offsets, sizes [secCount]uint64
	place := func(sec int, data []byte, align int) {
		for len(out)%align != 0 {
			out = append(out, 0)
		}

		offsets[sec] = uint64(len(out))
		sizes[sec] = uint64(len(data))
		out = append(out, data...)
	}

	place(secText, oc.a.buf, 16)
	place(secRelaText, relaText, 8)
	place(secRodata, oc.rodata, 16)
	place(secEhFrame, eh, 8)
	place(secRelaEhFrame, relaEh, 8)
	place(secNoteModule, noteBody, 4)
	place(secSymtab, symtab, 8)
	place(secStrtab, strtab, 1)
	place(secShstrtab, shstrtab, 1)
	place(secNoteStack, nil, 1)

	for len(out)%8 != 0 {
		out = append(out, 0)
	}

	shoff := uint64(len(out))
	header := func(sec int, typ uint32, flags uint64, link, info uint32, align, entsize uint64) {
		out = binary.LittleEndian.AppendUint32(out, names[sec])
		out = binary.LittleEndian.AppendUint32(out, typ)
		out = binary.LittleEndian.AppendUint64(out, flags)
		out = binary.LittleEndian.AppendUint64(out, 0)
		out = binary.LittleEndian.AppendUint64(out, offsets[sec])
		out = binary.LittleEndian.AppendUint64(out, sizes[sec])
		out = binary.LittleEndian.AppendUint32(out, link)
		out = binary.LittleEndian.AppendUint32(out, info)
		out = binary.LittleEndian.AppendUint64(out, align)
		out = binary.LittleEndian.AppendUint64(out, entsize)
	}

	out = append(out, make([]byte, 64)...)
	header(secText, shtProgbits, shfAlloc|shfExec, 0, 0, 16, 0)
	header(secRelaText, shtRela, shfInfoLink, secSymtab, secText, 8, 24)
	header(secRodata, shtProgbits, shfAlloc, 0, 0, 16, 0)
	header(secEhFrame, shtProgbits, shfAlloc, 0, 0, 8, 0)
	header(secRelaEhFrame, shtRela, shfInfoLink, secSymtab, secEhFrame, 8, 24)
	header(secNoteModule, shtNote, 0, 0, 0, 4, 0)
	header(secSymtab, shtSymtab, 0, secStrtab, uint32(firstGlobal), 8, 24)
	header(secStrtab, shtStrtab, 0, 0, 0, 1, 0)
	header(secShstrtab, shtStrtab, 0, 0, 0, 1, 0)
	header(secNoteStack, shtProgbits, 0, 0, 0, 1, 0)

	copy(out, []byte{0x7f, 'E', 'L', 'F', 2, 1, 1, 0})
	binary.LittleEndian.PutUint16(out[16:], 1)
	binary.LittleEndian.PutUint16(out[18:], 62)
	binary.LittleEndian.PutUint32(out[20:], 1)
	binary.LittleEndian.PutUint64(out[40:], shoff)
	binary.LittleEndian.PutUint16(out[52:], 64)
	binary.LittleEndian.PutUint16(out[58:], 64)
	binary.LittleEndian.PutUint16(out[60:], secCount)
	binary.LittleEndian.PutUint16(out[62:], secShstrtab)

	return out
}

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
