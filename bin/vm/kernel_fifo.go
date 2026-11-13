//go:build linux && amd64

package main

import "syscall"

// A FIFO of a server is a node and nothing more (lib/nim/fs.nim): its pipe
// is the kernel's, kept by the FIFO's device and inode while an end of it
// is open, and an open of the FIFO joins it as a reader, a writer or, with
// O_RDWR, both (kernel_pipe.go join). An open to read waits for a writer
// and one to write for a reader, as POSIX has it, unless O_NONBLOCK: then
// a reader goes on alone and a writer with no reader is ENXIO. A FIFO of a
// host root is the host's (kernel_inbox.go).

type fifoKey struct {
	dev, ino uint64
}

// fifoName is what a FIFO's pipe knows of the FIFO: where the kernel keeps
// it, its mode.
type fifoName struct {
	k    *Kernel
	key  fifoKey
	mode uint32
}

// forget takes a FIFO's pipe off the kernel's table when it has no end.
func (n *fifoName) forget(pp *pipe) {
	n.k.mu.Lock()
	defer n.k.mu.Unlock()

	pp.mu.Lock()
	empty := pp.readers == 0 && pp.writers == 0
	pp.mu.Unlock()

	if empty && n.k.fifos[n.key] == pp {
		delete(n.k.fifos, n.key)
	}
}

// openFifo opens the FIFO of a server whose stat st is, with the flags of
// open.
func (p *Process) openFifo(st *syscall.Stat_t, flags int) (File, error) {
	access := flags & syscall.O_ACCMODE
	reads, writes := access != syscall.O_WRONLY, access != syscall.O_RDONLY
	nonblock := flags&syscall.O_NONBLOCK != 0
	key := fifoKey{st.Dev, st.Ino}
	k := p.k
	k.mu.Lock()
	pp := k.fifos[key]

	if pp == nil {
		pp = newPipe()
		pp.name = &fifoName{k: k, key: key, mode: st.Mode}
		k.fifos[key] = pp
	}

	if writes && !reads && nonblock {
		pp.mu.Lock()
		alone, empty := pp.readers == 0, pp.readers == 0 && pp.writers == 0
		pp.mu.Unlock()

		if alone {
			if empty {
				delete(k.fifos, key)
			}

			k.mu.Unlock()

			return nil, syscall.ENXIO
		}
	}

	readerOpens, writerOpens := pp.join(reads, writes)
	k.mu.Unlock()
	e := &pipeEnd{pipe: pp, reads: reads, writes: writes}
	e.refs.Store(1)
	e.nonblock.Store(nonblock)

	if nonblock || reads && writes {
		return e, nil
	}

	err := p.waitFor(func() bool { return pp.met(reads, readerOpens, writerOpens) }, pp.opened.wait, true)

	if err != nil {
		e.unref()

		return nil, err
	}

	return e, nil
}
