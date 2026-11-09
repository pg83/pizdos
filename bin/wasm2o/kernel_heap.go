//go:build linux && amd64

package main

import (
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

// block returns the size of the used block of a payload; another pointer
// ends the process.
func (p *Process) block(ptr uint32, what string) uint32 {
	block := ptr - blockHeader

	if ptr%16 != 0 || ptr <= p.heap+heapHeader || ptr >= p.u32(p.heap+heapTop) {
		p.fatal(fmt.Sprintf("%s(): invalid pointer %#x", what, ptr))
	}

	word := p.u32(block)
	size := word &^ blockUsed

	if word&blockUsed == 0 || p.u32(block+4) != blockCheck(block, size) {
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

const mapAnonymous = 0x20

func (k *Kernel) heapFunctions() map[string]any {
	return map[string]any{
		"malloc": func(ctx uintptr, n uint32) uint32 {
			p := proc(ctx)
			at := p.malloc(n)

			if at == 0 {
				p.setErrno(syscall.ENOMEM)
			}

			return at
		},
		"free": func(ctx uintptr, ptr uint32) {
			proc(ctx).free(ptr)
		},
		"calloc": func(ctx uintptr, n, size uint32) uint32 {
			p := proc(ctx)
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
		},
		"realloc": func(ctx uintptr, ptr, n uint32) uint32 {
			p := proc(ctx)
			at := p.realloc(ptr, n)

			if at == 0 && n != 0 {
				p.setErrno(syscall.ENOMEM)
			}

			return at
		},
		"mallopt": func(ctx uintptr, param, value uint32) uint32 {
			return 1
		},
		"getpagesize": func(ctx uintptr) uint32 {
			return 4096
		},
		// mmap maps from the heap: anonymous memory zeroed, a file read in
		// as a private copy.
		"mmap": func(ctx uintptr, addr, n, prot, flags, fd uint32, off uint64) uint32 {
			p := proc(ctx)
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
			}

			return at
		},
	}
}
