//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"math/bits"
	"os"
	"slices"
	"syscall"
)

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

// The heap is the memory from __heap_base up, and it holds the program's
// data alone: what the allocator knows of it, which block is in use and how
// large, is the kernel's, in Go, where the program can neither read nor
// change it, and every pointer it frees is checked against it. The memory
// is pages of 64 KiB, the wasm page, in segments of 32 the heap grows by,
// as trustme's allocator has them. A block of up to 16 KiB is a slot of a
// page of its size class: the class hands out its current page's slots,
// those freed last first, then the ones the page never handed out, then
// takes a page with free slots or a free page, and a page goes back to its
// segment when its last slot is freed. A larger block is a run of pages.
// A segment's mask has a bit for each free page. All of the heap is in
// memory, so a fork copies it with the process, and the child gets a copy
// of what the kernel knows of it.

const (
	heapPageShift = 16
	heapPageSize  = 1 << heapPageShift
	segmentPages  = 32
	segmentSize   = segmentPages * heapPageSize
	smallLimit    = 16384
	smallClasses  = 40
	// heapMax is the largest allocation.
	heapMax = 1 << 31
	// A page's kind is its class, or one of these.
	pageFree = 0xff
	pageRun  = 0xfe
	pageTail = 0xfd
	noPage   = -1
	// segmentHops is how many segments with free pages a run looks
	// through before the heap grows.
	segmentHops = 8
)

// Heap is what the kernel knows of a process's heap.
type Heap struct {
	// base is the address of the first page, top the end of the pages
	// ever handed out, the heap's end for fork.
	base     uint32
	top      uint32
	pages    []heapPage
	segments []heapSegment
	// free is the first segment with free pages.
	free    int32
	classes [smallClasses]heapClass
}

// heapPage is a page: of a class, the slots in use and their number, the
// first slot it never handed out and those freed, the last freed last; of
// a run, its first page has the run's length.
type heapPage struct {
	kind uint8
	// listed is whether the page is in its class's list of pages with
	// free slots, by prev and next.
	listed bool
	live   uint16
	bump   uint16
	run    uint32
	prev   int32
	next   int32
	freed  []uint16
	used   []uint64
}

// heapSegment is 32 pages: a bit of mask for each free one, and its place
// in the list of segments with free pages.
type heapSegment struct {
	mask   uint32
	linked bool
	prev   int32
	next   int32
}

// heapClass is a size class: the page it hands out slots of and the first
// of its other pages with free slots.
type heapClass struct {
	current int32
	partial int32
}

// classSizes and classSlots are each class's block size and the blocks a
// page has of it.
var classSizes, classSlots = func() (sizes [smallClasses]uint32, slots [smallClasses]uint16) {
	for i := range sizes {
		if i < 16 {
			sizes[i] = uint32(i+1) * 16
		} else {
			base := uint32(1) << (8 + (i-16)/4)
			sizes[i] = base + base/4*uint32((i-16)%4+1)
		}

		slots[i] = uint16(heapPageSize / sizes[i])
	}

	return
}()

// classOf is the class of a block of n bytes, at most smallLimit: steps of
// 16 up to 256, then four per power of two.
func classOf(n uint32) int {
	if n <= 256 {
		return max(int(n+15)/16-1, 0)
	}

	e := bits.Len32(n-1) - 1

	return 16 + (e-8)*4 + int((n-1)>>(e-2)&3)
}

// pagesFor is how many pages n bytes take.
func pagesFor(n uint64) uint32 {
	return uint32((n + heapPageSize - 1) >> heapPageShift)
}

// heapInit makes the heap from __heap_base.
func (p *Process) heapInit() *Heap {
	base, ok := p.inst.globalExport("__heap_base")

	if !ok {
		p.fatal("no __heap_base")
	}

	p.heap = newHeap(base)

	return p.heap
}

// newHeap is an empty heap whose first page is the first at base or after
// it.
func newHeap(base uint32) *Heap {
	first := (uint64(base) + heapPageSize - 1) &^ (heapPageSize - 1)
	h := &Heap{base: uint32(first), top: uint32(first), free: noPage}

	for i := range h.classes {
		h.classes[i] = heapClass{noPage, noPage}
	}

	return h
}

// address is the address of a page.
func (h *Heap) address(page int32) uint32 {
	return h.base + uint32(page)<<heapPageShift
}

func (h *Heap) link(s int32) {
	seg := &h.segments[s]
	seg.linked, seg.prev, seg.next = true, noPage, h.free

	if h.free != noPage {
		h.segments[h.free].prev = s
	}

	h.free = s
}

func (h *Heap) unlink(s int32) {
	seg := &h.segments[s]
	seg.linked = false

	if seg.prev != noPage {
		h.segments[seg.prev].next = seg.next
	} else {
		h.free = seg.next
	}

	if seg.next != noPage {
		h.segments[seg.next].prev = seg.prev
	}
}

// grow adds a segment of free pages at the end of the heap.
func (p *Process) heapGrow(h *Heap) bool {
	end := uint64(h.base) + uint64(len(h.segments)+1)*segmentSize

	if end > math.MaxUint32 || !p.ensure(end) {
		return false
	}

	h.segments = append(h.segments, heapSegment{mask: math.MaxUint32})

	for range segmentPages {
		h.pages = append(h.pages, heapPage{kind: pageFree})
	}

	h.link(int32(len(h.segments) - 1))

	return true
}

// takePages takes count free pages in a row, from a segment with free
// pages or one the heap grows by.
func (p *Process) takePages(h *Heap, count uint32) (int32, bool) {
	if count > segmentPages {
		return p.takeSegments(h, count)
	}

	need := uint32(math.MaxUint32)

	if count < segmentPages {
		need = 1<<count - 1
	}

	for {
		hops := 0

		for s := h.free; s != noPage && hops < segmentHops; s, hops = h.segments[s].next, hops+1 {
			seg := &h.segments[s]
			runs := seg.mask

			for i := uint32(1); i < count && runs != 0; i++ {
				runs &= runs >> 1
			}

			if runs == 0 {
				continue
			}

			start := bits.TrailingZeros32(runs)
			seg.mask &^= need << start

			if seg.mask == 0 {
				h.unlink(s)
			}

			first := s*segmentPages + int32(start)
			h.top = max(h.top, h.address(first+int32(count)))

			return first, true
		}

		if !p.heapGrow(h) {
			return 0, false
		}
	}
}

// takeSegments takes the segments a run of more than a segment's pages
// needs, free in a row, those at the end of the heap or new ones; the
// last keeps the pages the run leaves.
func (p *Process) takeSegments(h *Heap, count uint32) (int32, bool) {
	k := int((count + segmentPages - 1) / segmentPages)
	at, row := -1, 0

	for i := range h.segments {
		if h.segments[i].mask != math.MaxUint32 {
			row = 0

			continue
		}

		if row++; row == k {
			at = i - k + 1

			break
		}
	}

	if at < 0 {
		for range k - row {
			if !p.heapGrow(h) {
				return 0, false
			}
		}

		at = len(h.segments) - k
	}

	left := count

	for s := at; s < at+k; s++ {
		n := min(left, segmentPages)
		seg := &h.segments[s]

		if n == segmentPages {
			seg.mask = 0
		} else {
			seg.mask &^= 1<<n - 1
		}

		if seg.mask == 0 && seg.linked {
			h.unlink(int32(s))
		}

		left -= n
	}

	first := int32(at * segmentPages)
	h.top = max(h.top, h.address(first+int32(count)))

	return first, true
}

// releasePages gives count pages from first back to their segments.
func (h *Heap) releasePages(first int32, count uint32) {
	for q := first; q < first+int32(count); q++ {
		h.pages[q].kind = pageFree
		s := q / segmentPages
		h.segments[s].mask |= 1 << (q % segmentPages)

		if !h.segments[s].linked {
			h.link(s)
		}
	}
}

// markRun makes count pages from first a run.
func (h *Heap) markRun(first int32, count uint32) {
	h.pages[first].kind, h.pages[first].run = pageRun, count

	for q := first + 1; q < first+int32(count); q++ {
		h.pages[q].kind = pageTail
	}
}

func (h *Heap) list(cls int, page int32) {
	pg, c := &h.pages[page], &h.classes[cls]
	pg.listed, pg.prev, pg.next = true, noPage, c.partial

	if c.partial != noPage {
		h.pages[c.partial].prev = page
	}

	c.partial = page
}

func (h *Heap) unlist(cls int, page int32) {
	pg, c := &h.pages[page], &h.classes[cls]
	pg.listed = false

	if pg.prev != noPage {
		h.pages[pg.prev].next = pg.next
	} else {
		c.partial = pg.next
	}

	if pg.next != noPage {
		h.pages[pg.next].prev = pg.prev
	}
}

// refill gives a class a current page: one of its pages with free slots,
// or a free page.
func (p *Process) refill(h *Heap, cls int) bool {
	c := &h.classes[cls]

	if page := c.partial; page != noPage {
		h.unlist(cls, page)
		c.current = page

		return true
	}

	page, ok := p.takePages(h, 1)

	if !ok {
		return false
	}

	pg := &h.pages[page]
	words := (int(classSlots[cls]) + 63) / 64
	pg.kind, pg.live, pg.bump, pg.freed = uint8(cls), 0, 0, pg.freed[:0]

	if cap(pg.used) >= words {
		pg.used = pg.used[:words]
		clear(pg.used)
	} else {
		pg.used = make([]uint64, words)
	}

	c.current = page

	return true
}

// mallocSmall allocates a block of a class.
func (p *Process) mallocSmall(h *Heap, cls int) uint32 {
	c := &h.classes[cls]

	for {
		if page := c.current; page != noPage {
			pg := &h.pages[page]
			slot := uint16(0)

			if n := len(pg.freed); n > 0 {
				slot = pg.freed[n-1]
				pg.freed = pg.freed[:n-1]
			} else if pg.bump < classSlots[cls] {
				slot = pg.bump
				pg.bump++
			} else {
				slot = math.MaxUint16
			}

			if slot != math.MaxUint16 {
				pg.used[slot/64] |= 1 << (slot % 64)
				pg.live++

				return h.address(page) + uint32(slot)*classSizes[cls]
			}
		}

		if !p.refill(h, cls) {
			return 0
		}
	}
}

// mallocRun allocates a run of pages.
func (p *Process) mallocRun(h *Heap, n uint64) uint32 {
	count := pagesFor(n)
	first, ok := p.takePages(h, count)

	if !ok {
		return 0
	}

	h.markRun(first, count)

	return h.address(first)
}

// malloc allocates n bytes of the heap, or returns 0.
func (p *Process) malloc(n uint32) uint32 {
	h := p.heap

	if h == nil {
		h = p.heapInit()
	}

	if n <= smallLimit {
		return p.mallocSmall(h, classOf(n))
	}

	if n > heapMax {
		return 0
	}

	return p.mallocRun(h, uint64(n))
}

// usedBlock finds the block ptr is the start of: its page, slot and size,
// or false for another pointer.
func (p *Process) usedBlock(ptr uint32) (page int32, slot, size uint32, ok bool) {
	h := p.heap

	if h == nil || ptr < h.base || uint64(ptr-h.base)>>heapPageShift >= uint64(len(h.pages)) {
		return 0, 0, 0, false
	}

	page = int32((ptr - h.base) >> heapPageShift)
	off := (ptr - h.base) & (heapPageSize - 1)
	pg := &h.pages[page]

	switch {
	case pg.kind < smallClasses:
		size = classSizes[pg.kind]
		slot = off / size

		if off%size != 0 || slot >= uint32(pg.bump) || pg.used[slot/64]&(1<<(slot%64)) == 0 {
			return 0, 0, 0, false
		}
	case pg.kind == pageRun && off == 0:
		size = pg.run << heapPageShift
	default:
		return 0, 0, 0, false
	}

	return page, slot, size, true
}

// block is usedBlock of a pointer the program gave what; another pointer
// ends the process.
func (p *Process) block(ptr uint32, what string) (int32, uint32, uint32) {
	page, slot, size, ok := p.usedBlock(ptr)

	if !ok {
		p.fatal(fmt.Sprintf("%s(): invalid pointer %#x", what, ptr))
	}

	return page, slot, size
}

func (p *Process) free(ptr uint32) {
	if ptr == 0 {
		return
	}

	page, slot, _ := p.block(ptr, "free")
	h := p.heap
	pg := &h.pages[page]

	if pg.kind == pageRun {
		h.releasePages(page, pg.run)

		return
	}

	cls := int(pg.kind)
	pg.used[slot/64] &^= 1 << (slot % 64)
	pg.live--
	pg.freed = append(pg.freed, uint16(slot))

	if page == h.classes[cls].current {
		return
	}

	if pg.live == 0 {
		if pg.listed {
			h.unlist(cls, page)
		}

		h.releasePages(page, 1)
	} else if !pg.listed {
		h.list(cls, page)
	}
}

// resizeRun makes a run hold n bytes in place: the pages past them go
// back, or the free pages after it, those the heap grows by at its end
// included, join it. It returns false when they are not free.
func (p *Process) resizeRun(page int32, n uint64) bool {
	h := p.heap
	run := h.pages[page].run
	count := pagesFor(n)

	if count <= run {
		h.releasePages(page+int32(count), run-count)
		h.pages[page].run = count

		return true
	}

	end := page + int32(count)

	for q := page + int32(run); q < end; q++ {
		if int(q) >= len(h.pages) {
			break
		}

		if h.pages[q].kind != pageFree {
			return false
		}
	}

	for int(end) > len(h.pages) {
		if !p.heapGrow(h) {
			return false
		}
	}

	for q := page + int32(run); q < end; q++ {
		s := q / segmentPages
		h.segments[s].mask &^= 1 << (q % segmentPages)

		if h.segments[s].mask == 0 && h.segments[s].linked {
			h.unlink(s)
		}
	}

	h.markRun(page, count)
	h.top = max(h.top, h.address(end))

	return true
}

func (p *Process) realloc(ptr, n uint32) uint32 {
	if ptr == 0 {
		return p.malloc(n)
	}

	if n == 0 {
		p.free(ptr)

		return 0
	}

	page, _, size := p.block(ptr, "realloc")
	run := p.heap.pages[page].kind == pageRun

	if n <= size && !run {
		return ptr
	}

	if run && n > smallLimit && n <= heapMax && p.resizeRun(page, uint64(n)) {
		return ptr
	}

	moved := p.malloc(n)

	if moved != 0 {
		keep := min(size, n)
		copy(p.span(moved, keep), p.span(ptr, keep))
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

// memalign allocates n bytes at a multiple of align, a power of two above
// 16: a slot of the class of the power of two that holds them, whose slots
// are at multiples of its size, a run, at multiples of a page, or a run
// with room to align its start, the pages before and after it given back.
func (p *Process) memalign(align, n uint32) uint32 {
	if align <= 16 {
		return p.malloc(n)
	}

	h := p.heap

	if h == nil {
		h = p.heapInit()
	}

	if n > heapMax {
		return 0
	}

	if size := max(n, align); size <= smallLimit {
		return p.mallocSmall(h, classOf(1<<bits.Len32(size-1)))
	}

	if align <= heapPageSize {
		return p.mallocRun(h, uint64(n))
	}

	count := pagesFor(uint64(n))
	slack := align/heapPageSize - 1
	first, ok := p.takePages(h, count+slack)

	if !ok {
		return 0
	}

	at := uint64(h.address(first))
	aligned := (at + uint64(align) - 1) &^ (uint64(align) - 1)
	lead := uint32((aligned - at) >> heapPageShift)
	h.releasePages(first, lead)
	h.releasePages(first+int32(lead+count), slack-lead)
	h.markRun(first+int32(lead), count)

	return uint32(aligned)
}

// clone is a copy of what the kernel knows of the heap, for a child of
// fork.
func (h *Heap) clone() *Heap {
	if h == nil {
		return nil
	}

	c := *h
	c.pages = slices.Clone(h.pages)

	for i := range c.pages {
		c.pages[i].freed = slices.Clone(c.pages[i].freed)
		c.pages[i].used = slices.Clone(c.pages[i].used)
	}

	c.segments = slices.Clone(h.segments)

	return &c
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

	_, _, size := p.block(ptr, "malloc_usable_size")

	return size
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
