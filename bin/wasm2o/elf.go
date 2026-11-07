package main

import "encoding/binary"

// writeELF lays out an x86-64 relocatable object: the text, every code
// region a function symbol, the export thunks and the init thunk global;
// each slot an absolute relocation to the undefined symbol it holds; the
// module's description in .w2o; an empty .note.GNU-stack.
func writeELF(oc *objCode, meta []byte) []byte {
	const (
		secText = 1 + iota
		secRela
		secSymtab
		secStrtab
		secMeta
		secShstrtab
		secNote
		secCount
	)

	global := map[string]bool{"init": true}

	for _, r := range oc.regions {
		if len(r.name) > 7 && r.name[:7] == "export:" {
			global[r.name] = true
		}
	}

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
	)

	sym(0, 0, 0, 0, 0)
	sym(0, stbLocal<<4|sttSect, secText, 0, 0)
	count := 2

	for _, r := range oc.regions {
		if !global[r.name] {
			sym(str(r.name), stbLocal<<4|sttFunc, secText, uint64(r.start), uint64(r.end-r.start))
			count++
		}
	}

	firstGlobal := count

	for _, r := range oc.regions {
		if global[r.name] {
			sym(str(r.name), stbGlobal<<4|sttFunc, secText, uint64(r.start), uint64(r.end-r.start))
			count++
		}
	}

	var rela []byte

	for _, s := range oc.slots {
		sym(str(s.sym), stbGlobal<<4|sttNotype, 0, 0, 0)
		rela = binary.LittleEndian.AppendUint64(rela, uint64(oc.blobAt[s.label]))
		// R_X86_64_64
		rela = binary.LittleEndian.AppendUint64(rela, uint64(count)<<32|1)
		rela = binary.LittleEndian.AppendUint64(rela, 0)
		count++
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
	names[secText] = sh(".text")
	names[secRela] = sh(".rela.text")
	names[secSymtab] = sh(".symtab")
	names[secStrtab] = sh(".strtab")
	names[secMeta] = sh(".w2o")
	names[secShstrtab] = sh(".shstrtab")
	names[secNote] = sh(".note.GNU-stack")

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
	place(secRela, rela, 8)
	place(secSymtab, symtab, 8)
	place(secStrtab, strtab, 1)
	place(secMeta, meta, 1)
	place(secShstrtab, shstrtab, 1)
	place(secNote, nil, 1)

	for len(out)%8 != 0 {
		out = append(out, 0)
	}

	shoff := uint64(len(out))
	header := func(name, typ uint32, flags, offset, size uint64, link, info uint32, align, entsize uint64) {
		out = binary.LittleEndian.AppendUint32(out, name)
		out = binary.LittleEndian.AppendUint32(out, typ)
		out = binary.LittleEndian.AppendUint64(out, flags)
		out = binary.LittleEndian.AppendUint64(out, 0)
		out = binary.LittleEndian.AppendUint64(out, offset)
		out = binary.LittleEndian.AppendUint64(out, size)
		out = binary.LittleEndian.AppendUint32(out, link)
		out = binary.LittleEndian.AppendUint32(out, info)
		out = binary.LittleEndian.AppendUint64(out, align)
		out = binary.LittleEndian.AppendUint64(out, entsize)
	}

	header(0, 0, 0, 0, 0, 0, 0, 0, 0)
	header(names[secText], elfProgbits, 6, offsets[secText], sizes[secText], 0, 0, 16, 0)
	header(names[secRela], elfRela, 0x40, offsets[secRela], sizes[secRela], secSymtab, secText, 8, 24)
	header(names[secSymtab], elfSymtab, 0, offsets[secSymtab], sizes[secSymtab], secStrtab, uint32(firstGlobal), 8, 24)
	header(names[secStrtab], elfStrtab, 0, offsets[secStrtab], sizes[secStrtab], 0, 0, 1, 0)
	header(names[secMeta], elfProgbits, 0, offsets[secMeta], sizes[secMeta], 0, 0, 1, 0)
	header(names[secShstrtab], elfStrtab, 0, offsets[secShstrtab], sizes[secShstrtab], 0, 0, 1, 0)
	header(names[secNote], elfProgbits, 0, offsets[secNote], 0, 0, 0, 1, 0)

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

// section types
const (
	elfProgbits = 1
	elfSymtab   = 2
	elfStrtab   = 3
	elfRela     = 4
)

// Object is what a loader takes from an object wasm2o wrote.
type Object struct {
	text []byte
	// defined maps a symbol to its offset in the text; relocs are the
	// slots and the symbols whose values they take.
	defined map[string]uint32
	relocs  []objReloc
	meta    *Meta
}

type objReloc struct {
	at  uint32
	sym string
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

	type section struct {
		name, typ   uint32
		data        []byte
		link, info  uint32
		offset, end uint64
	}

	sections := make([]section, count)

	for i := range sections {
		h := data[shoff+uint64(64*i):]
		offset, size := le.Uint64(h[24:]), le.Uint64(h[32:])
		typ := le.Uint32(h[4:])

		if typ != 8 && (offset > uint64(len(data)) || size > uint64(len(data))-offset) {
			throwFmt("section %d out of the file", i)
		}

		s := section{name: le.Uint32(h), typ: typ, link: le.Uint32(h[40:]), info: le.Uint32(h[44:])}

		if typ != 8 {
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

	byName := map[string]*section{}

	for i := range sections {
		byName[cstr(sections[names].data, sections[i].name)] = &sections[i]
	}

	need := func(name string) *section {
		s := byName[name]

		if s == nil {
			throwFmt("no %s section", name)
		}

		return s
	}

	obj := &Object{text: need(".text").data, defined: map[string]uint32{}, meta: decodeMeta(need(".w2o").data)}
	symtab := need(".symtab")

	if symtab.link >= uint32(count) || len(symtab.data)%24 != 0 {
		throwFmt("malformed symbol table")
	}

	strtab := sections[symtab.link].data
	var symbols []string

	for at := 0; at < len(symtab.data); at += 24 {
		e := symtab.data[at:]
		name := cstr(strtab, le.Uint32(e))
		symbols = append(symbols, name)

		if shndx := le.Uint16(e[6:]); shndx != 0 && e[4]&0xf == 2 {
			value := le.Uint64(e[8:])

			if value > uint64(len(obj.text)) {
				throwFmt("symbol %s out of the text", name)
			}

			obj.defined[name] = uint32(value)
		}
	}

	rela := need(".rela.text")

	if len(rela.data)%24 != 0 {
		throwFmt("malformed relocations")
	}

	for at := 0; at < len(rela.data); at += 24 {
		e := rela.data[at:]
		offset, info, addend := le.Uint64(e), le.Uint64(e[8:]), int64(le.Uint64(e[16:]))

		if info&0xffffffff != 1 || addend != 0 || offset+8 > uint64(len(obj.text)) || info>>32 >= uint64(len(symbols)) {
			throwFmt("unsupported relocation at %#x", offset)
		}

		obj.relocs = append(obj.relocs, objReloc{uint32(offset), symbols[info>>32]})
	}

	return obj
}
