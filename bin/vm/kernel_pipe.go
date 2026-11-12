//go:build linux && amd64

package main

import (
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// A pipe is the kernel's, not the host's: a queue of chunks, chan []byte,
// between its two ends, which are objects of the kernel a process reads
// and writes without a host descriptor. It holds pipeSize bytes, as
// Linux's does: a write of at most pipeChunk bytes, PIPE_BUF, goes whole
// or not at all, a larger one in parts as room comes. A read takes what is
// there, up to its buffer; an end of the chunk it took is kept for the
// next read. The last writer's close ends the data, the last reader's
// makes a write EPIPE and SIGPIPE. A blocking end waits as a host object
// does (kernel_object.go): a change of the pipe wakes the waiters.
const (
	pipeSize  = 65536
	pipeChunk = 4096
)

type pipe struct {
	mu sync.Mutex
	// front is what a read left of the chunk it took, chunks the whole
	// chunks after it, back the chunk being filled after those; bytes
	// counts them all.
	front  []byte
	chunks chan []byte
	back   []byte
	bytes  int
	// readers and writers count the open ends.
	readers int
	writers int
	waiters waitQueue
	ino     uint64
	made    time.Time
}

// pipeEnd is one end of a pipe, an open file: the writer or the reader.
type pipeEnd struct {
	pipe     *pipe
	writer   bool
	refs     atomic.Int32
	nonblock atomic.Bool
}

var (
	_ Transfer = (*pipeEnd)(nil)
	_ Ioctl    = (*pipeEnd)(nil)
)

var pipeInodes atomic.Uint64

// pipe makes a pipe and installs its ends at the lowest descriptors.
func (p *Process) pipe(at, flags uint32) uint32 {
	if flags&^(syscall.O_CLOEXEC|syscall.O_NONBLOCK) != 0 {
		return p.result(0, syscall.EINVAL)
	}

	pp := &pipe{chunks: make(chan []byte, pipeSize/pipeChunk), readers: 1, writers: 1, ino: pipeInodes.Add(1), made: time.Now()}
	ends := [2]*pipeEnd{{pipe: pp}, {pipe: pp, writer: true}}
	cloexec := flags&syscall.O_CLOEXEC != 0
	var fds [2]int

	for i, e := range ends {
		e.refs.Store(1)
		e.nonblock.Store(flags&syscall.O_NONBLOCK != 0)
		fd, err := p.install(e, 0, cloexec)

		if err != nil {
			if i == 1 {
				p.closeFd(fds[0])
			} else {
				ends[1].unref()
			}

			return p.result(0, err)
		}

		fds[i] = fd
	}

	p.put32(at, uint32(fds[0]))
	p.put32(at+4, uint32(fds[1]))

	return p.result(0, nil)
}

func (e *pipeEnd) ref() {
	e.refs.Add(1)
}

// unref closes the end when its last reference goes: the pipe loses a
// reader or a writer, which the other end's waiters learn.
func (e *pipeEnd) unref() {
	if e.refs.Add(-1) != 0 {
		return
	}

	pp := e.pipe
	pp.mu.Lock()

	if e.writer {
		pp.writers--
	} else {
		pp.readers--
	}

	pp.mu.Unlock()
	pp.waiters.wake()
}

func (e *pipeEnd) host() int {
	return -1
}

func (e *pipeEnd) queue() *waitQueue {
	return &e.pipe.waiters
}

// stat is a FIFO's: the ends share an inode of the kernel's.
func (e *pipeEnd) stat(st *syscall.Stat_t) error {
	if chaosFail("file-stat") {
		return syscall.EIO
	}

	pp := e.pipe
	*st = syscall.Stat_t{Ino: pp.ino, Mode: syscall.S_IFIFO | 0o600, Nlink: 1, Uid: uint32(syscall.Getuid()), Gid: uint32(syscall.Getgid()), Blksize: pipeChunk}
	st.Atim = syscall.NsecToTimespec(pp.made.UnixNano())
	st.Mtim, st.Ctim = st.Atim, st.Atim

	return nil
}

func (e *pipeEnd) poll(events int16) int16 {
	pp := e.pipe
	pp.mu.Lock()
	defer pp.mu.Unlock()

	mask := int16(0)

	if e.writer {
		if pipeSize-pp.bytes >= pipeChunk {
			mask |= pollOut | pollWrnorm
		}

		if pp.readers == 0 {
			mask |= pollErr
		}
	} else {
		if pp.bytes > 0 {
			mask |= pollIn | pollRdnorm
		}

		if pp.writers == 0 {
			mask |= pollHup
		}
	}

	return mask & (events | pollErr | pollHup)
}

func (e *pipeEnd) flags() (int, error) {
	fl := syscall.O_RDONLY

	if e.writer {
		fl = syscall.O_WRONLY
	}

	if e.nonblock.Load() {
		fl |= syscall.O_NONBLOCK
	}

	return fl, nil
}

func (e *pipeEnd) setFlags(fl int) error {
	e.nonblock.Store(fl&syscall.O_NONBLOCK != 0)

	return nil
}

// ioctl knows FIONREAD, the bytes to read.
func (e *pipeEnd) ioctl(req uintptr, arg unsafe.Pointer) error {
	if req != fionread || e.writer {
		return syscall.ENOTTY
	}

	pp := e.pipe
	pp.mu.Lock()
	*(*int32)(arg) = int32(pp.bytes)
	pp.mu.Unlock()

	return nil
}

// next is the chunk a read goes on with after the front: the first whole
// one, else the back.
func (pp *pipe) next() []byte {
	select {
	case c := <-pp.chunks:
		return c
	default:
	}

	c := pp.back
	pp.back = nil

	return c
}

// take reads what the pipe holds into b, under the lock: 0 at the end of
// the data, EAGAIN when there is none yet.
func (pp *pipe) take(b []byte) (int, error) {
	if pp.bytes == 0 {
		if pp.writers == 0 {
			return 0, nil
		}

		return 0, syscall.EAGAIN
	}

	n := 0

	for n < len(b) && pp.bytes > 0 {
		if len(pp.front) == 0 {
			pp.front = pp.next()
		}

		c := copy(b[n:], pp.front)
		pp.front = pp.front[c:]
		n += c
		pp.bytes -= c
	}

	return n, nil
}

// put writes b to the pipe, under the lock, as far as the room goes:
// EAGAIN when there is none, or not enough for a write that goes whole.
// The write end polls writable with a chunk of room, as Linux's does with
// a free page: then a write that goes whole does, and a larger one makes
// progress.
func (pp *pipe) put(b []byte) (int, error) {
	if pp.readers == 0 {
		return 0, syscall.EPIPE
	}

	room := pipeSize - pp.bytes

	if room == 0 || len(b) <= pipeChunk && len(b) > room {
		return 0, syscall.EAGAIN
	}

	n := min(len(b), room)

	for at := 0; at < n; {
		if pp.back == nil {
			pp.back = make([]byte, 0, pipeChunk)
		}

		c := copy(pp.back[len(pp.back):pipeChunk], b[at:n])
		pp.back = pp.back[:len(pp.back)+c]
		at += c

		if len(pp.back) == pipeChunk {
			select {
			case pp.chunks <- pp.back:
			default:
				throwFmt("internal error: pipe chunks overflow")
			}

			pp.back = nil
		}
	}

	pp.bytes += n

	return n, nil
}

func (e *pipeEnd) read(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("read") {
		return 0, syscall.EIO
	}

	if e.writer {
		return 0, syscall.EBADF
	}

	if len(b) == 0 {
		return 0, nil
	}

	pp := e.pipe

	for {
		pp.mu.Lock()
		n, err := pp.take(b)
		pp.mu.Unlock()

		if n > 0 {
			pp.waiters.wake()
		}

		if err != syscall.EAGAIN {
			return n, err
		}

		if mode.nonblock || e.nonblock.Load() {
			return 0, syscall.EAGAIN
		}

		if _, err := p.waitObject(e, pollIn, -1, true); err != nil {
			return 0, err
		}
	}
}

func (e *pipeEnd) write(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("write") {
		return 0, syscall.EIO
	}

	if !e.writer {
		return 0, syscall.EBADF
	}

	if len(b) == 0 {
		return 0, nil
	}

	pp := e.pipe
	total := 0

	for {
		pp.mu.Lock()
		empty := pp.bytes == 0
		n, err := pp.put(b[total:])
		pp.mu.Unlock()

		if n > 0 && empty {
			pp.waiters.wake()
		}

		total += n

		if err == syscall.EPIPE {
			if !mode.quiet {
				p.sendSelf(int(syscall.SIGPIPE))
			}

			if total > 0 {
				return total, nil
			}

			return 0, err
		}

		if total == len(b) {
			return total, nil
		}

		if mode.nonblock || e.nonblock.Load() {
			if total > 0 {
				return total, nil
			}

			return 0, syscall.EAGAIN
		}

		if _, err := p.waitObject(e, pollOut, -1, true); err != nil {
			if total > 0 {
				return total, nil
			}

			return 0, err
		}
	}
}

func (e *pipeEnd) pread(p *Process, b []byte, off int64) (int, error) {
	return 0, syscall.ESPIPE
}

func (e *pipeEnd) pwrite(p *Process, b []byte, off int64) (int, error) {
	return 0, syscall.ESPIPE
}
