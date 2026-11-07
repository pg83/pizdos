package main

import "encoding/binary"

// writeELF lays out an x86-64 relocatable object: the text section, its
// relocations, the symbol table and an empty .note.GNU-stack.
func writeELF(oc *objCode) []byte {
	const (
		secText = 1 + iota
		secRela
		secSymtab
		secStrtab
		secShstrtab
		secNote
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

	sym(0, 0, 0, 0, 0)
	sym(0, 3, secText, 0, 0)
	count := 2

	for _, s := range oc.syms {
		if !s.global {
			sym(str(s.name), 2, secText, uint64(s.value), uint64(s.size))
			count++
		}
	}

	firstGlobal := count

	for _, s := range oc.syms {
		if s.global {
			sym(str(s.name), 1<<4|2, secText, uint64(s.value), uint64(s.size))
			count++
		}
	}

	externBase := count

	for _, name := range oc.externs {
		sym(str(name), 1<<4, 0, 0, 0)
	}

	var rela []byte

	for _, r := range oc.relocs {
		rela = binary.LittleEndian.AppendUint64(rela, uint64(r.at))
		// R_X86_64_PLT32
		rela = binary.LittleEndian.AppendUint64(rela, uint64(externBase+int(r.sym))<<32|4)
		rela = binary.LittleEndian.AppendUint64(rela, uint64(r.addend))
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
	header(names[secText], 1, 6, offsets[secText], sizes[secText], 0, 0, 16, 0)
	header(names[secRela], 4, 0x40, offsets[secRela], sizes[secRela], secSymtab, secText, 8, 24)
	header(names[secSymtab], 2, 0, offsets[secSymtab], sizes[secSymtab], secStrtab, uint32(firstGlobal), 8, 24)
	header(names[secStrtab], 3, 0, offsets[secStrtab], sizes[secStrtab], 0, 0, 1, 0)
	header(names[secShstrtab], 3, 0, offsets[secShstrtab], sizes[secShstrtab], 0, 0, 1, 0)
	header(names[secNote], 1, 0, offsets[secNote], 0, 0, 0, 1, 0)

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
