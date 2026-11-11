//go:build linux && amd64

package main

import (
	_ "embed"
	"encoding/binary"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

// lib/str's string and memory functions (pg83/str compiled for vm,
// lib/str/make.py): one object, its code in the convention between the
// module's functions, taking host addresses and checking nothing. It is
// linked once; str_choose gives the configuration of the host, and the
// public slots of its table are what the code calls through the GOT as
// str.memcpy and the like, the ranges checked before the call.
//
//go:embed libstr.o
var strBlob []byte

// strSlots are the public slots of str's table (struct str_table in str's
// common.h) in its order; a configuration (struct str_config) has its name
// and needs before the table.
var strSlots = []string{"memcpy", "memmove", "memset", "memcmp", "strlen", "strcpy", "strcat", "strcmp", "strspn", "strcspn", "memchr"}

// A configuration's public slots follow its name and feature requirements.
const strConfigTable = 16

type strLibrary struct {
	code  []byte
	slots map[string]uintptr
}

var strLib = sync.OnceValue(linkStr)

// strFunction is the code address of the function a symbol str.<slot>
// names, in the host's configuration.
func strFunction(sym string) uintptr {
	at, ok := strLib().slots[strings.TrimPrefix(sym, symStr)]

	if !ok {
		throwFmt("no function of lib/str for %s", sym)
	}

	return at
}

// linkStr links the embedded str object, validated by lib/str/make.py:
// the text, executable, then the read-only data, both relocated, with
// nothing outside them. The code is registered
// with Go as assembly is, never preempted asynchronously: it keeps values
// in vector and mask registers Go does not save and below rsp.
func linkStr() *strLibrary {
	f := parseELF(strBlob)
	eh := f.find(".eh_frame")

	text, rodata := f.sections[f.text].data, f.sections[f.rodata].data
	defined := f.functions()
	regions := readEhFrame(f.sections[eh].data, f.relocations(eh), defined)
	page := func(n int) int {
		return (n + 4095) &^ 4095
	}

	rodataAt := page(max(len(text), 1))
	code := mmap(rodataAt+page(max(len(rodata), 1)), syscall.PROT_READ|syscall.PROT_WRITE)
	copy(code, text)
	copy(code[rodataAt:], rodata)
	base := uintptr(unsafe.Pointer(unsafe.SliceData(code)))

	for _, section := range []struct{ index, at int }{{f.text, 0}, {f.rodata, rodataAt}} {
		for _, r := range f.relocations(section.index) {
			target := uint64(base) + r.value

			switch r.in {
			case inRodata:
				target += uint64(rodataAt)
			}

			at := section.at + int(r.at)

			switch r.typ {
			case rX86_64_PC32:
				put32rel(code[at:], int64(target)+r.addend-int64(uint64(base)+uint64(at)))
			case rX86_64_64:
				binary.LittleEndian.PutUint64(code[at:], target+uint64(r.addend))
			}
		}
	}

	throw(syscall.Mprotect(code[:rodataAt], syscall.PROT_READ|syscall.PROT_EXEC))
	throw(syscall.Mprotect(code[rodataAt:], syscall.PROT_READ))
	registerCode("str", base, uintptr(len(text)), regions, true, nil, nil)
	choose := defined["str_choose"]
	at := strCall0(base+uintptr(choose)) - base
	lib := &strLibrary{code: code, slots: map[string]uintptr{}}

	for k, slot := range strSlots {
		lib.slots[slot] = uintptr(binary.LittleEndian.Uint64(code[at+strConfigTable+8*uintptr(k):]))
	}

	return lib
}
