//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"runtime"
	"strings"
	"sync/atomic"
	"unsafe"
)

// Loaded code becomes Go code to the runtime as a module of its own,
// linked after the program's: a moduledata in Go 1.25's layout
// (runtime/symtab.go) with a function, a name and pc tables for every code
// region. Stack maps say no frame holds a pointer: values are wasm values,
// and the instance stays alive in its Go caller. The stack pointer table is
// exact at every instruction and there is no unsafe-point table, so Go may
// preempt the code anywhere by a signal, as it does its own; but for the
// resume stub of fork, which writes below its stack pointer: it is marked
// assembly, which Go never preempts.

type goPCHeader struct {
	magic                                                            uint32
	pad1, pad2, minLC, ptrSize                                       uint8
	nfunc                                                            int
	nfiles                                                           uint
	textStart                                                        uintptr
	funcnameOffset, cuOffset, filetabOffset, pctabOffset, pclnOffset uintptr
}

// funcFlagAsm is abi.FuncFlagAsm.
const funcFlagAsm = 1 << 2

type goFuncTab struct {
	entry, offset uint32
}

type goBitVector struct {
	n    int32
	data *byte
}

// goModule is runtime.moduledata; slices of runtime types are slices of
// bytes here, of the same size.
type goModule struct {
	header                                                     *goPCHeader
	names                                                      []byte
	cutab                                                      []uint32
	files, pctab, pcln                                         []byte
	ftab                                                       []goFuncTab
	findfunctab, minpc, maxpc                                  uintptr
	text, etext, noptrdata, enoptrdata, data, edata, bss, ebss uintptr
	noptrbss, enoptrbss, covctrs, ecovctrs, end, gcdata, gcbss uintptr
	types, etypes, rodata, gofunc                              uintptr
	textsectmap                                                []byte
	typelinks                                                  []int32
	itablinks, ptab                                            []byte
	pluginpath                                                 string
	pkghashes, inittasks                                       []byte
	modulename                                                 string
	modulehashes                                               []byte
	hasmain                                                    uint8
	bad                                                        bool
	gcdatamask, gcbssmask                                      goBitVector
	typemap                                                    map[int32]unsafe.Pointer
	next                                                       *goModule
}

//go:linkname lastModule runtime.lastmoduledatap
var lastModule *goModule

// hostModule is the program's own module, where runtime symbols are.
var hostModule = lastModule

// goImage keeps a registered module and what it points to: Go never drops
// a module, and profiles keep PCs.
type goImage struct {
	module   goModule
	header   goPCHeader
	stackMap []byte
	buckets  []byte
}

type registration struct {
	image *goImage
	done  chan struct{}
}

var registrations = make(chan registration)

// The module list has one writer: this goroutine. The runtime reads it
// without locks, so a module is complete before it is linked in.
func init() {
	go func() {
		var images []*goImage

		for r := range registrations {
			images = append(images, r.image)
			m := &r.image.module
			atomic.StorePointer((*unsafe.Pointer)(unsafe.Pointer(&lastModule.next)), unsafe.Pointer(m))
			lastModule = m
			close(r.done)
		}
	}()
}

func checkGo() {
	if !strings.HasPrefix(runtime.Version(), "go1.25.") {
		throwFmt("the loader is written for Go 1.25, not %s", runtime.Version())
	}
}

// goSymbol returns the address of a function of the program.
func goSymbol(name string) uintptr {
	checkGo()
	m := hostModule

	for _, f := range m.ftab[:len(m.ftab)-1] {
		record := m.pcln[f.offset:]
		n := m.names[binary.LittleEndian.Uint32(record[4:]):]

		if string(n[:bytes.IndexByte(n, 0)]) == name {
			return m.text + uintptr(f.entry)
		}
	}

	throwFmt("no %s in the program", name)

	return 0
}

func append32(dst []byte, values ...uint32) []byte {
	for _, v := range values {
		dst = binary.LittleEndian.AppendUint32(dst, v)
	}

	return dst
}

// pcTable appends a pc-value table: the values from their offsets on, up
// to size; neighbours differ, as the encoding requires.
func (m *goModule) pcTable(values []spEntry, size uint32) uint32 {
	offset := uint32(len(m.pctab))
	previous := int32(-1)

	for k, v := range values {
		end := int32(size)

		if k+1 < len(values) {
			end = values[k+1].pc
		}

		if end <= v.pc || k > 0 && v.delta == previous {
			throwFmt("internal error: bad pc table")
		}

		delta := v.delta - previous
		m.pctab = binary.AppendUvarint(m.pctab, uint64(uint32(delta<<1)^uint32(delta>>31)))
		m.pctab = binary.AppendUvarint(m.pctab, uint64(end-v.pc))
		previous = v.delta
	}

	m.pctab = append(m.pctab, 0)

	return offset
}

// registerCode tells Go about the regions of code at base.
func registerCode(name string, base uintptr, size uintptr, regions []Region) {
	checkGo()

	if len(regions) == 0 {
		return
	}

	// A stack map of one entry with no bits: for locals and arguments.
	img := &goImage{stackMap: append32(nil, 1, 0)}
	m := &img.module
	end := regions[len(regions)-1].end
	img.header = goPCHeader{magic: 0xfffffff1, minLC: 1, ptrSize: 8, nfunc: len(regions), nfiles: 1, textStart: base}
	m.header = &img.header
	m.text, m.minpc, m.maxpc, m.etext = base, base, base+uintptr(end), base+size
	m.modulename = name
	m.gofunc = uintptr(unsafe.Pointer(unsafe.SliceData(img.stackMap)))
	m.names = []byte{0}
	m.pctab = []byte{0}
	m.files, m.cutab = append([]byte(name), 0), []uint32{0}

	for index, r := range regions {
		size := r.end - r.start

		if index > 0 && r.start < regions[index-1].end || len(r.sp) == 0 || r.sp[0].pc != 0 {
			throwFmt("invalid code region %s", r.name)
		}

		line := uint32(index + 1)
		flag := uint32(0)

		if r.name == symResume {
			flag = funcFlagAsm
		}

		pcsp := m.pcTable(r.sp, size)
		pcfile := m.pcTable([]spEntry{{0, 0}}, size)
		pcln := m.pcTable([]spEntry{{0, int32(line)}}, size)
		m.ftab = append(m.ftab, goFuncTab{r.start, uint32(len(m.pcln))})
		// _func: entry, name, args, deferreturn, pcsp, pcfile, pcln,
		// npcdata, cu, start line, funcID 0, flag 0, pad, two funcdata;
		// then the funcdata offsets in gofunc, both the stack map.
		m.pcln = append32(m.pcln, r.start, uint32(len(m.names)), 0, 0, pcsp, pcfile, pcln, 0, 0, line, 2<<24|flag<<8, 0, 0)
		m.names = append(m.names, r.name...)
		m.names = append(m.names, 0)
	}

	m.ftab = append(m.ftab, goFuncTab{end, 0})
	img.buckets = make([]byte, (uintptr(end)+4095)/4096*20)
	function := 0

	for bucket := 0; bucket < len(img.buckets)/20; bucket++ {
		for function+1 < len(m.ftab)-1 && m.ftab[function+1].entry <= uint32(bucket*4096) {
			function++
		}

		binary.LittleEndian.PutUint32(img.buckets[bucket*20:], uint32(function))
	}

	m.findfunctab = uintptr(unsafe.Pointer(unsafe.SliceData(img.buckets)))
	r := registration{img, make(chan struct{})}
	registrations <- r
	<-r.done
}
