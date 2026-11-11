//go:build linux && amd64

package main

import "encoding/binary"

// setjmp and longjmp (jump.go). The process keeps its jumps by the address
// of their jmp_buf, which stays unused: the guest cannot forge a frame. A
// jump also restores the module's stack pointer, which the program exports
// as __stack_pointer, and sigsetjmp's mask when it saved one.
//
// longjmp may leave a signal handler, dropping the kernel's frames down to
// it: what runs a handler holds no lock, defers nothing and leaves nothing
// half done for after it.

type jump struct {
	Jump
	stack uint32
	mask  uint64
	saved bool
}

// stackPointer is where the instance keeps __stack_pointer.
func (p *Process) stackPointer() uint32 {
	if p.stackAt == 0 {
		at, ok := p.inst.globalAt("__stack_pointer")

		if !ok {
			p.fatal("setjmp: no __stack_pointer")
		}

		p.stackAt = at
	}

	return p.stackAt
}

func (p *Process) setjmp(buf uint32, saveMask bool) uint32 {
	j := p.jumps[buf]

	if j == nil {
		j = new(jump)
	}

	if !p.inst.setjmp(&j.Jump) {
		p.fatal("setjmp: called through a pointer")
	}

	j.stack = binary.LittleEndian.Uint32(p.inst.block[p.stackPointer():])
	j.mask, j.saved = p.mask(), saveMask
	p.jumps[buf] = j

	return 0
}

func (p *Process) longjmp(buf, val uint32) {
	j := p.jumps[buf]

	if j == nil {
		p.fatal("longjmp: the jmp_buf was not set")
	}

	if val == 0 {
		val = 1
	}

	// The frames the jump leaves take their jumps with them.
	for b, o := range p.jumps {
		if o.sp() > j.sp() {
			delete(p.jumps, b)
		}
	}

	binary.LittleEndian.PutUint32(p.inst.block[p.stackPointer():], j.stack)

	if j.saved {
		p.setMask(j.mask)
		p.deliver()
	}

	p.inst.longjmp(&j.Jump, val)
	p.fatal("longjmp: the function that set the jmp_buf has returned")
}

func (p *Process) libcSetjmp(buf uint32) uint32 {
	return p.setjmp(buf, false)
}

func (p *Process) libcSigsetjmp(buf, saveMask uint32) uint32 {
	return p.setjmp(buf, saveMask != 0)
}

func (p *Process) libcLongjmp(buf, val uint32) {
	p.longjmp(buf, val)
}
