//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"math"
)

// An asan object, one vm compiled with --asan, checks every access of its
// memory against the shadow map instead of the memory's bounds (aot.cpp
// lowererAsanAddress): a byte of the map for each byte of memory, shadowOK
// where the program may read and write and another value, which tells why
// not, where it may not. The map is the kernel's view of the memory. The
// stack and the data, from the first KiB to __heap_base, are accessible
// from the start; a block of the heap is accessible from when the allocator
// hands it out, for the bytes the program asked for, with the rest of the
// block a red zone, and poisoned from when it is freed until a block takes
// its place; the rest, the first KiB, the heap's free pages and the memory
// past the heap, was never allocated. The kernel checks its own accesses for
// the program's calls against the map too (span), so a read into a freed
// buffer is caught as the code's store would be. A program runs with the
// map only under `vm run --asan`, which the kernel's processes inherit:
// exec of an object without the map runs it as ever.

const (
	shadowFree    = 0x00 // never allocated: the first KiB, free pages, past the memory
	shadowRedzone = 0xfa // past what the program asked for, up to the block's size
	shadowFreed   = 0xfd // a freed block
	shadowOK      = 0xff
)

// shadow is the process's shadow map, nil but for an asan object.
func (p *Process) shadow() []byte {
	if p.inst == nil || p.inst.memory == nil {
		return nil
	}

	return p.inst.memory.shadow
}

// fill sets every byte of s to v.
func fill(s []byte, v byte) {
	if len(s) == 0 {
		return
	}

	s[0] = v

	for n := 1; n < len(s); n *= 2 {
		copy(s[n:], s[:n])
	}
}

// asanStart marks the memory before the image's init runs, which places
// the data segments: all of the initial memory but the first KiB, as the
// kernel does not know __heap_base yet.
func (p *Process) asanStart() {
	if sh := p.shadow(); sh != nil {
		fill(sh[boundSlack:p.inst.memory.pages*pageSize], shadowOK)
	}
}

// asanInit marks the memory as the program starts: the stack and the data
// up to __heap_base accessible, the heap's pages never allocated.
func (p *Process) asanInit() {
	sh := p.shadow()

	if sh == nil {
		return
	}

	size := p.inst.memory.pages * pageSize
	base, ok := p.inst.globalExport("__heap_base")

	if ok && uint64(base) < size {
		clear(sh[base:size])
	}
}

// shadowBlock marks a block the allocator handed out: n bytes accessible
// at at, the rest of its size bytes a red zone.
func (p *Process) shadowBlock(at, n, size uint32) {
	if sh := p.shadow(); sh != nil {
		fill(sh[uint64(at):uint64(at)+uint64(n)], shadowOK)
		fill(sh[uint64(at)+uint64(n):uint64(at)+uint64(size)], shadowRedzone)
	}
}

// shadowFree marks a block of size bytes at at freed.
func (p *Process) shadowFree(at, size uint32) {
	if sh := p.shadow(); sh != nil {
		fill(sh[uint64(at):uint64(at)+uint64(size)], shadowFreed)
	}
}

// blockAsked is how many bytes the program asked for of a block of size
// bytes at at: the accessible ones from its start, or all of it without
// the shadow map.
func (p *Process) blockAsked(at, size uint32) uint32 {
	sh := p.shadow()

	if sh == nil {
		return size
	}

	return uint32(okBytes(sh[uint64(at) : uint64(at)+uint64(size)]))
}

// okBytes counts the accessible bytes at the start of a range of the map.
func okBytes(s []byte) int {
	n := len(s)

	for len(s) >= 8 && binary.LittleEndian.Uint64(s) == math.MaxUint64 {
		s = s[8:]
	}

	for len(s) > 0 && s[0] == shadowOK {
		s = s[1:]
	}

	return n - len(s)
}

// asanCheck ends the process as a trap would when the n bytes at at are
// not all accessible: the kernel is about to read or write them for a
// call of the program. The range is within the memory already.
func (p *Process) asanCheck(at, n uint32) {
	sh := p.shadow()

	if sh == nil || n == 0 {
		return
	}

	ok := okBytes(sh[uint64(at) : uint64(at)+uint64(n)])

	if ok != int(n) {
		p.fatal(p.asanReport(&Trap{code: trapAsan, addr: uint64(at) + uint64(ok), kind: asanRange}))
	}
}

// asanReport describes a trap of an asan check: the access, and what the
// shadow map says of its first inaccessible byte.
func (p *Process) asanReport(t *Trap) string {
	bad := t.addr

	if sh := p.shadow(); sh != nil && t.width > 1 && bad < uint64(len(sh)) {
		bad += uint64(okBytes(sh[bad:min(bad+uint64(t.width), uint64(len(sh)))]))
	}

	where := ": "

	if bad != t.addr {
		where = fmt.Sprintf(", whose byte %#x is ", bad)
	}

	return "asan: " + t.access() + where + p.asanDescribe(bad)
}

// asanDescribe tells what the shadow map says of an address: the first KiB,
// past the memory, a freed block, the red zone past what a block's program
// asked for, or memory never allocated, of the heap or elsewhere.
func (p *Process) asanDescribe(addr uint64) string {
	sh := p.shadow()
	size := p.inst.memory.pages * pageSize

	switch {
	case sh == nil:
		return "no shadow map"
	case addr >= size:
		return fmt.Sprintf("past the memory's end %#x", size)
	case addr < boundSlack:
		return "in the first KiB, where nothing is"
	}

	start, asked, blockSize, ok := p.blockAround(uint32(addr))

	switch sh[addr] {
	case shadowOK:
		return "accessible"
	case shadowRedzone:
		// Only shadowBlock creates red zones for live allocations; freeing
		// changes the whole block to shadowFreed, including its red zone.
		return fmt.Sprintf("past the end of the %d-byte block at %#x", asked, start)
	case shadowFreed:
		if ok {
			return fmt.Sprintf("in the freed %d-byte block at %#x", blockSize, start)
		}

		return "in freed memory"
	}

	if h := p.heap; h != nil && addr >= uint64(h.base) {
		return "in free memory of the heap"
	}

	return "in memory never allocated"
}

// blockAround finds the block of the heap addr is in, used or freed: its
// start, the bytes the program asked for, as the map has them, and its
// size.
func (p *Process) blockAround(addr uint32) (start, asked, size uint32, ok bool) {
	h := p.heap

	if h == nil || addr < h.base || uint64(addr-h.base)>>heapPageShift >= uint64(len(h.pages)) {
		return 0, 0, 0, false
	}

	page := int32((addr - h.base) >> heapPageShift)

	for h.pages[page].kind == pageTail && page > 0 {
		page--
	}

	pg := &h.pages[page]

	switch {
	case pg.kind < smallClasses:
		size = classSizes[pg.kind]
		start = h.address(page) + (addr-h.address(page))/size*size
	case pg.kind == pageRun:
		size = pg.run << heapPageShift
		start = h.address(page)
	default:
		return 0, 0, 0, false
	}

	return start, p.blockAsked(start, size), size, true
}
