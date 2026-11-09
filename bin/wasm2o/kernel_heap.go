//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"math/bits"
	"os"
	"syscall"
)

// The heap is the memory from __heap_base up, the allocator's own: a header
// at its start, then blocks. A block is an eight-byte header, its size with
// a used bit and a check word, then the payload, 16-byte aligned; its size
// is its class's. A freed block goes to the list of its class, linked
// through its payload; a new block is cut from the top, and memory grows
// under it. All of it is in memory, so a fork copies the heap with the
// process.

const (
	heapMagic   = 0x6b68_6561
	heapTop     = 4
	heapBins    = 16
	heapClasses = 152
	heapHeader  = heapBins + 4*heapClasses
	blockHeader = 8
	blockUsed   = 1
	// heapMax is the largest allocation.
	heapMax = 1 << 31
)

// classOf returns the class of a block size, a multiple of 16, and the
// class's size: steps of 16 up to 1 KiB, then four per power of two.
func classOf(size uint32) (int, uint32) {
	if size <= 1024 {
		return int(size/16 - 1), size
	}

	k := bits.Len32(size-1) - 1
	step := uint32(1) << (k - 2)
	n := (size - 1<<k + step - 1) / step

	return 64 + (k-10)*4 + int(n) - 1, 1<<k + n*step
}

func align16(x uint64) uint64 {
	return (x + 15) &^ 15
}

// globalAt is where an exported global of the module keeps its value in
// the instance block.
func (i *Instance) globalAt(name string) (uint32, bool) {
	for _, e := range i.obj.meta.exports {
		if e.kind == externGlobal && e.name == name && !i.obj.meta.globals[e.index].imported {
			return i.obj.meta.globals[e.index].offset, true
		}
	}

	return 0, false
}

// globalExport is the value of an exported i32 global.
func (i *Instance) globalExport(name string) (uint32, bool) {
	at, ok := i.globalAt(name)

	if !ok {
		return 0, false
	}

	return binary.LittleEndian.Uint32(i.block[at:]), true
}

// ensure grows memory to hold end bytes.
func (p *Process) ensure(end uint64) bool {
	m := p.inst.memory
	size := m.pages * pageSize

	if end <= size {
		return true
	}

	need := (end - size + pageSize - 1) / pageSize
	more := max(need, min(m.pages/8, 256))

	if m.grow(uint32(more)) != math.MaxUint32 {
		return true
	}

	return m.grow(uint32(need)) != math.MaxUint32
}

func (p *Process) heapInit() {
	base, ok := p.inst.globalExport("__heap_base")

	if !ok {
		p.fatal("no __heap_base")
	}

	h := align16(uint64(base))
	top := align16(h+heapHeader) + blockHeader

	if !p.ensure(top) {
		p.fatal("no memory for the heap")
	}

	clear(p.span(uint32(h), heapHeader))
	p.put32(uint32(h), heapMagic)
	p.put32(uint32(h)+heapTop, uint32(top))
	p.heap = uint32(h)
}

func blockCheck(block, size uint32) uint32 {
	return size ^ block ^ heapMagic
}

func (p *Process) setBlock(block, size uint32, used bool) {
	word := size

	if used {
		word |= blockUsed
	}

	p.put32(block, word)
	p.put32(block+4, blockCheck(block, size))
}

// usedBlock returns the size of the used block of a payload, or false for
// another pointer.
func (p *Process) usedBlock(ptr uint32) (uint32, bool) {
	if p.heap == 0 || ptr%16 != 0 || ptr <= p.heap+heapHeader || ptr >= p.u32(p.heap+heapTop) {
		return 0, false
	}

	block := ptr - blockHeader
	word := p.u32(block)
	size := word &^ blockUsed

	if word&blockUsed == 0 || p.u32(block+4) != blockCheck(block, size) {
		return 0, false
	}

	return size, true
}

// block returns the size of the used block of a payload; another pointer
// ends the process.
func (p *Process) block(ptr uint32, what string) uint32 {
	size, ok := p.usedBlock(ptr)

	if !ok {
		p.fatal(fmt.Sprintf("%s(): invalid pointer %#x", what, ptr))
	}

	return size
}

// malloc allocates n bytes of the heap, or returns 0.
func (p *Process) malloc(n uint32) uint32 {
	if p.heap == 0 {
		p.heapInit()
	}

	if n > heapMax {
		return 0
	}

	cls, size := classOf(uint32(align16(uint64(n) + blockHeader)))
	bin := p.heap + heapBins + 4*uint32(cls)

	if ptr := p.u32(bin); ptr != 0 {
		p.put32(bin, p.u32(ptr))
		p.setBlock(ptr-blockHeader, size, true)

		return ptr
	}

	top := p.u32(p.heap + heapTop)

	if !p.ensure(uint64(top) + uint64(size)) {
		return 0
	}

	p.setBlock(top, size, true)
	p.put32(p.heap+heapTop, top+size)

	return top + blockHeader
}

func (p *Process) free(ptr uint32) {
	if ptr == 0 {
		return
	}

	size := p.block(ptr, "free")
	cls, _ := classOf(size)
	bin := p.heap + heapBins + 4*uint32(cls)
	p.setBlock(ptr-blockHeader, size, false)
	p.put32(ptr, p.u32(bin))
	p.put32(bin, ptr)
}

func (p *Process) realloc(ptr, n uint32) uint32 {
	if ptr == 0 {
		return p.malloc(n)
	}

	if n == 0 {
		p.free(ptr)

		return 0
	}

	size := p.block(ptr, "realloc")
	need := align16(uint64(n) + blockHeader)

	if need <= uint64(size) {
		return ptr
	}

	block := ptr - blockHeader

	if top := p.u32(p.heap + heapTop); block+size == top && n <= heapMax {
		_, grown := classOf(uint32(need))

		if p.ensure(uint64(block) + uint64(grown)) {
			p.setBlock(block, grown, true)
			p.put32(p.heap+heapTop, block+grown)

			return ptr
		}
	}

	moved := p.malloc(n)

	if moved != 0 {
		copy(p.span(moved, size-blockHeader), p.span(ptr, size-blockHeader))
		p.free(ptr)
	}

	return moved
}

// fatal ends the process as abort does, with a message on the host's
// standard error.
func (p *Process) fatal(msg string) {
	name := "?"

	if p.image != nil && len(p.image.argv) > 0 {
		name = p.image.argv[0]
	}

	fmt.Fprintf(os.Stderr, "%s[%d]: %s\n", name, p.pid, msg)
	p.exitWith(int(syscall.SIGABRT))
}

const (
	mapShared    = 0x01
	mapAnonymous = 0x20
	protWrite    = 0x2
	madvDontneed = 4
)

func pageRound(n uint32) uint64 {
	return (uint64(n) + 4095) &^ 4095
}

// classSize is the size of a class's blocks.
func classSize(cls int) uint32 {
	if cls < 64 {
		return uint32(cls+1) * 16
	}

	k := 10 + (cls-64)/4

	return 1<<k + uint32((cls-64)%4+1)<<(k-2)
}

// memalign allocates n bytes at a multiple of align, a power of two above
// 16: a block cut from the top of the heap where its payload is aligned,
// the gap below it made free blocks of the largest classes that fit.
func (p *Process) memalign(align, n uint32) uint32 {
	if align <= 16 {
		return p.malloc(n)
	}

	if p.heap == 0 {
		p.heapInit()
	}

	if n > heapMax {
		return 0
	}

	_, size := classOf(uint32(align16(uint64(n) + blockHeader)))
	top := p.u32(p.heap + heapTop)
	payload := (uint64(top) + blockHeader + uint64(align) - 1) &^ (uint64(align) - 1)
	block := payload - blockHeader

	if block+uint64(size) > math.MaxUint32 || !p.ensure(block+uint64(size)) {
		return 0
	}

	for gap := uint32(block) - top; gap > 0; {
		cls, csize := classOf(gap)

		if csize > gap {
			cls--
			csize = classSize(cls)
		}

		bin := p.heap + heapBins + 4*uint32(cls)
		p.setBlock(top, csize, false)
		p.put32(top+blockHeader, p.u32(bin))
		p.put32(bin, top+blockHeader)
		top += csize
		gap -= csize
	}

	p.setBlock(uint32(block), size, true)
	p.put32(p.heap+heapTop, uint32(block)+size)

	return uint32(payload)
}

// alignment checks an alignment: a power of two, for posix_memalign also
// a multiple of a pointer's size.
func alignment(align uint32, pointer bool) bool {
	return align != 0 && align&(align-1) == 0 && (!pointer || align%4 == 0)
}

// mapping is a block mmap made: its length and, of a file, the file and
// its offset. A shared mapping that may be written keeps the contents it
// read, which msync, munmap and the end of the image compare with to write
// back what changed.
type mapping struct {
	length uint32
	file   *File
	offset int64
	read   []byte
}

// writeBack writes what changed of a shared mapping's range to its file,
// up to the file's end.
func (p *Process) writeBack(at uint32, m mapping, from, n uint32) error {
	if m.read == nil {
		return nil
	}

	var st syscall.Stat_t

	if err := syscall.Fstat(m.file.fd, &st); err != nil {
		return err
	}

	n = uint32(max(min(int64(n), st.Size-m.offset-int64(from)), 0))
	now := p.span(at+from, n)
	was := m.read[from : from+n]

	for i := 0; i < len(now); {
		if now[i] == was[i] {
			i++

			continue
		}

		j := i

		for j < len(now) && now[j] != was[j] {
			j++
		}

		if _, err := syscall.Pwrite(m.file.fd, now[i:j], m.offset+int64(from)+int64(i)); err != nil {
			return err
		}

		copy(was[i:j], now[i:j])
		i = j
	}

	return nil
}

// unmap writes a mapping back and frees it.
func (p *Process) unmap(at uint32) {
	m := p.mappings[at]
	p.writeBack(at, m, 0, m.length)
	delete(p.mappings, at)

	if m.file != nil {
		m.file.unref()
	}

	p.free(at)
}

// unmapAll ends the mappings of an image that ends: shared ones go back to
// their files. The memory goes with the image.
func (p *Process) unmapAll() {
	if p.inst == nil {
		return
	}

	for at, m := range p.mappings {
		p.writeBack(at, m, 0, m.length)

		if m.file != nil {
			m.file.unref()
		}
	}

	clear(p.mappings)
}

// mappingOf finds the mapping an address is in.
func (p *Process) mappingOf(addr uint32) (uint32, mapping, bool) {
	for at, m := range p.mappings {
		if addr-at < m.length {
			return at, m, true
		}
	}

	return 0, mapping{}, false
}

func (p *Process) libcMalloc(n uint32) uint32 {
	at := p.malloc(n)

	if at == 0 {
		p.setErrno(syscall.ENOMEM)
	}

	return at
}

func (p *Process) libcCalloc(n, size uint32) uint32 {
	total := uint64(n) * uint64(size)

	if total > math.MaxUint32 {
		p.setErrno(syscall.ENOMEM)

		return 0
	}

	at := p.malloc(uint32(total))

	if at == 0 {
		p.setErrno(syscall.ENOMEM)

		return 0
	}

	clear(p.span(at, uint32(total)))

	return at
}

func (p *Process) libcRealloc(ptr, n uint32) uint32 {
	at := p.realloc(ptr, n)

	if at == 0 && n != 0 {
		p.setErrno(syscall.ENOMEM)
	}

	return at
}

func (p *Process) libcMallopt(param, value uint32) uint32 {
	return 1
}

func (p *Process) libcGetpagesize() uint32 {
	return 4096
}

// mmap maps from the heap: anonymous memory zeroed, a file read in
// as a private copy. The process keeps the length of each mapping
// for munmap.
func (p *Process) libcMmap(addr, n, prot, flags, fd uint32, off uint64) uint32 {
	const failed = math.MaxUint32

	if n == 0 {
		p.setErrno(syscall.EINVAL)

		return failed
	}

	at := p.malloc(n)

	if at == 0 {
		p.setErrno(syscall.ENOMEM)

		return failed
	}

	b := p.span(at, n)
	clear(b)
	m := mapping{length: n}

	if flags&mapAnonymous == 0 {
		f, err := p.fd(fd)

		if err == nil {
			_, err = syscall.Pread(f.fd, b, int64(off))
		}

		if err != nil {
			p.free(at)
			p.setErrno(errnoOf(err))

			return failed
		}

		m.file, m.offset = f.ref(), int64(off)

		if flags&mapShared != 0 && prot&protWrite != 0 {
			m.read = bytes.Clone(b)
		}
	}

	p.mappings[at] = m

	return at
}

// munmap frees a mapping it covers whole; of part of one it frees
// nothing, and the rest of the mapping stays.
func (p *Process) libcMunmap(addr, n uint32) uint32 {
	if n == 0 {
		return p.result(0, syscall.EINVAL)
	}

	if m, ok := p.mappings[addr]; ok && pageRound(n) >= pageRound(m.length) {
		p.unmap(addr)
	}

	return p.result(0, nil)
}

func (p *Process) libcMsync(addr, n, flags uint32) uint32 {
	at, m, ok := p.mappingOf(addr)

	if !ok {
		return p.result(0, syscall.ENOMEM)
	}

	n = min(n, m.length-(addr-at))

	return p.result(0, p.writeBack(at, m, addr-at, n))
}

// madvise's MADV_DONTNEED reads a private mapping of a file again,
// leaves a shared one and zeroes other memory, as Linux's; other
// advice changes nothing.
func (p *Process) libcMadvise(addr, n, advice uint32) uint32 {
	b := p.span(addr, n)

	if advice != madvDontneed {
		return p.result(0, nil)
	}

	at, m, ok := p.mappingOf(addr)

	switch {
	case !ok || m.file == nil:
		clear(b)
	case m.read == nil:
		in := b[:min(n, m.length-(addr-at))]
		clear(in)
		syscall.Pread(m.file.fd, in, m.offset+int64(addr-at))
	}

	return p.result(0, nil)
}

// Memory has no protection nor pages to lock.
func (p *Process) libcMprotect(addr, n, prot uint32) uint32 {
	p.span(addr, n)

	return p.result(0, nil)
}

func (p *Process) libcMemalign(align, n uint32) uint32 {
	if !alignment(align, false) {
		p.setErrno(syscall.EINVAL)

		return 0
	}

	at := p.memalign(align, n)

	if at == 0 {
		p.setErrno(syscall.ENOMEM)
	}

	return at
}

func (p *Process) libcAligned_alloc(align, n uint32) uint32 {
	if !alignment(align, false) {
		p.setErrno(syscall.EINVAL)

		return 0
	}

	at := p.memalign(align, n)

	if at == 0 {
		p.setErrno(syscall.ENOMEM)
	}

	return at
}

func (p *Process) libcValloc(n uint32) uint32 {
	at := p.memalign(4096, n)

	if at == 0 {
		p.setErrno(syscall.ENOMEM)
	}

	return at
}

// posix_memalign returns an error number.
func (p *Process) libcPosix_memalign(out, align, n uint32) uint32 {
	if !alignment(align, true) {
		return uint32(syscall.EINVAL)
	}

	at := p.memalign(align, n)

	if at == 0 {
		return uint32(syscall.ENOMEM)
	}

	p.put32(out, at)

	return 0
}

func (p *Process) libcMalloc_usable_size(ptr uint32) uint32 {
	if ptr == 0 {
		return 0
	}

	return p.block(ptr, "malloc_usable_size") - blockHeader
}

// The heap gives no memory back.
func (p *Process) libcMalloc_trim(pad uint32) uint32 {
	return 0
}

func (p *Process) libcReallocarray(ptr, n, size uint32) uint32 {
	total := uint64(n) * uint64(size)

	if total > math.MaxUint32 {
		p.setErrno(syscall.ENOMEM)

		return 0
	}

	at := p.realloc(ptr, uint32(total))

	if at == 0 && total != 0 {
		p.setErrno(syscall.ENOMEM)
	}

	return at
}

func (p *Process) libcFree(ptr uint32) {
	p.free(ptr)
}

func (p *Process) libcMlock(addr, n uint32) uint32 {
	return p.result(0, nil)
}

func (p *Process) libcMunlock(addr, n uint32) uint32 {
	return p.result(0, nil)
}

func (p *Process) libcMlockall(flags uint32) uint32 {
	return p.result(0, nil)
}

func (p *Process) libcMunlockall() uint32 {
	return p.result(0, nil)
}
