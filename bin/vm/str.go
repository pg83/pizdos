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

// A configuration is a name, what it needs of the host and the table: 20
// slots of 8 bytes.
const (
	strConfigTable = 16
	strConfigSize  = strConfigTable + 20*8
)

type strLibrary struct {
	code   []byte
	config string
	slots  map[string]uintptr
	// every configuration's slots and whether the host runs it, for tests
	configs map[string]map[string]uintptr
	runs    map[string]bool
}

var strLib = sync.OnceValue(func() *strLibrary { return linkStr(strBlob) })

// strFunction is the code address of the function a symbol str.<slot>
// names, in the host's configuration.
func strFunction(sym string) uintptr {
	at, ok := strLib().slots[strings.TrimPrefix(sym, symStr)]

	if !ok {
		throwFmt("no function of lib/str for %s", sym)
	}

	return at
}

// linkStr links str's object: the text, executable, then the read-only
// data, both relocated, with nothing outside them. The code is registered
// with Go as assembly is, never preempted asynchronously: it keeps values
// in vector and mask registers Go does not save and below rsp.
func linkStr(data []byte) *strLibrary {
	f := parseELF(data)
	eh := f.find(".eh_frame")

	if f.rodata < 0 || eh < 0 {
		throwFmt("lib/str: no .rodata or .eh_frame")
	}

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
			case inExternal:
				throwFmt("lib/str refers to %s", r.sym)
			}

			at := section.at + int(r.at)

			switch r.typ {
			case rX86_64_PC32:
				put32rel(code[at:], int64(target)+r.addend-int64(uint64(base)+uint64(at)))
			case rX86_64_64:
				binary.LittleEndian.PutUint64(code[at:], target+uint64(r.addend))
			default:
				throwFmt("lib/str: relocation type %d", r.typ)
			}
		}
	}

	throw(syscall.Mprotect(code[:rodataAt], syscall.PROT_READ|syscall.PROT_EXEC))
	throw(syscall.Mprotect(code[rodataAt:], syscall.PROT_READ))
	registerCode("str", base, uintptr(len(text)), regions, true)
	choose, ok := defined["str_choose"]

	if !ok {
		throwFmt("lib/str: no str_choose")
	}

	lib := &strLibrary{code: code, configs: map[string]map[string]uintptr{}, runs: map[string]bool{}}
	word := func(at uintptr) uintptr {
		if at+8 > uintptr(len(code)) {
			throwFmt("lib/str: a configuration out of the library")
		}

		return uintptr(binary.LittleEndian.Uint64(code[at:]))
	}

	// read reads the configuration at offset at: its name and slots
	read := func(at uintptr) (string, uint32, map[string]uintptr) {
		name := cstr(code, uint32(word(at)-base))
		slots := map[string]uintptr{}

		for k, slot := range strSlots {
			fn := word(at + strConfigTable + 8*uintptr(k))

			if fn < base || fn >= base+uintptr(len(text)) {
				throwFmt("lib/str: the %s of configuration %s is outside the text", slot, name)
			}

			slots[slot] = fn
		}

		return name, binary.LittleEndian.Uint32(code[at+8:]), slots
	}

	lib.config, _, lib.slots = read(strCall0(base+uintptr(choose)) - base)
	configs, count := -1, -1

	for _, s := range f.symbols {
		if s.section == f.rodata && s.name == "str_configs" {
			configs = rodataAt + int(s.value)
		}

		if s.section == f.rodata && s.name == "str_nconfigs" {
			count = int(binary.LittleEndian.Uint32(code[rodataAt+int(s.value):]))
		}
	}

	host, ok := defined["str_host"]

	if configs < 0 || count < 0 || !ok {
		throwFmt("lib/str: no str_configs, str_nconfigs or str_host")
	}

	has := uint32(strCall0(base + uintptr(host)))

	for k := range count {
		name, needs, slots := read(uintptr(configs + k*strConfigSize))
		lib.configs[name] = slots
		lib.runs[name] = needs&^has == 0
	}

	return lib
}
