//go:build linux && amd64

package main

import (
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// A pipe is one direction of a pair (kernel_endpoint.go): a channel of
// packets, each a write, read as a stream of bytes. A write of at most
// pipeChunk bytes is one packet, whole or not at all, as POSIX asks of a
// write of at most PIPE_BUF; a larger one goes as several, which POSIX
// allows of a larger write. The pipe holds queueMax packets: a capacity
// in writes, where POSIX asks only that an empty pipe take PIPE_BUF. The
// last writer's close ends the data, the last reader's makes a write EPIPE
// and SIGPIPE. A blocking end waits as every file of the kernel does: on
// the pipe's change.

const pipeChunk = 4096

type pipe struct {
	ch *channel
	// front is what a read left of the packet it took, the next read's;
	// bytes counts it and the packets queued, for FIONREAD; readers and
	// writers count the open ends.
	mu      sync.Mutex
	front   []byte
	bytes   atomic.Int64
	readers int
	writers int
	changed changed
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

	pp := &pipe{ch: newChannel(), readers: 1, writers: 1, ino: pipeInodes.Add(1), made: time.Now()}
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

// unref closes the end when its last reference goes: the last reader
// finishes the channel, so a writer learns of it, the last writer closes
// it, so a reader sees the end of the data.
func (e *pipeEnd) unref() {
	if e.refs.Add(-1) != 0 {
		return
	}

	pp := e.pipe
	pp.mu.Lock()

	if e.writer {
		pp.writers--

		if pp.writers == 0 {
			close(pp.ch.ch)
		}
	} else {
		pp.readers--

		if pp.readers == 0 {
			close(pp.ch.done)
			pp.ch.drain()
			pp.front = nil
		}
	}

	pp.mu.Unlock()
	pp.changed.bump()
}

func (e *pipeEnd) changes(events int16) []<-chan struct{} {
	return []<-chan struct{}{e.pipe.changed.wait()}
}

// stat is a FIFO's: the ends share an inode of the kernel's.
func (e *pipeEnd) stat(st *syscall.Stat_t) error {
	pp := e.pipe
	*st = syscall.Stat_t{Ino: pp.ino, Mode: syscall.S_IFIFO | 0o600, Nlink: 1, Uid: uint32(syscall.Getuid()), Gid: uint32(syscall.Getgid()), Blksize: pipeChunk}
	st.Atim = syscall.NsecToTimespec(pp.made.UnixNano())
	st.Mtim, st.Ctim = st.Atim, st.Atim

	return nil
}

func (e *pipeEnd) poll(events int16) int16 {
	pp := e.pipe
	pp.mu.Lock()
	front, readers, writers := len(pp.front), pp.readers, pp.writers
	pp.mu.Unlock()
	mask := int16(0)

	if e.writer {
		if len(pp.ch.ch) < cap(pp.ch.ch) {
			mask |= pollOut | pollWrnorm
		}

		if readers == 0 {
			mask |= pollErr
		}
	} else {
		if front > 0 || len(pp.ch.ch) > 0 || writers == 0 {
			mask |= pollIn | pollRdnorm
		}

		if writers == 0 {
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

	*(*int32)(arg) = int32(e.pipe.bytes.Load())

	return nil
}

// take is what the pipe has for a read now, into b, as far as it fits:
// what a read left first, then the packets queued, of which what does not
// fit is left. ok is false with nothing queued, eof with the writers gone
// and nothing left.
func (pp *pipe) take(b []byte) (n int, eof, ok bool) {
	pp.mu.Lock()
	defer pp.mu.Unlock()

	for n < len(b) {
		if len(pp.front) == 0 {
			select {
			case pk, live := <-pp.ch.ch:
				if !live {
					pp.bytes.Add(-int64(n))

					return n, n == 0, true
				}

				pp.front = pk.data
			default:
				pp.bytes.Add(-int64(n))

				return n, false, n > 0
			}
		}

		c := copy(b[n:], pp.front)
		pp.front = pp.front[c:]
		n += c
	}

	pp.bytes.Add(-int64(n))

	return n, false, true
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
		n, eof, ok := pp.take(b)

		if ok {
			if !eof {
				pp.changed.bump()
			}

			return n, nil
		}

		if mode.nonblock || e.nonblock.Load() {
			return 0, syscall.EAGAIN
		}

		if _, err := p.waitObject(e, pollIn, -1, true); err != nil {
			return 0, err
		}
	}
}

// write queues b as packets of at most pipeChunk bytes, each whole or not
// at all, waiting for room between them; what was queued before a signal
// or EAGAIN is the count.
func (e *pipeEnd) write(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("write") {
		return 0, syscall.EIO
	}

	if !e.writer {
		return 0, syscall.EBADF
	}

	pp := e.pipe
	total := 0

	for total < len(b) {
		if pp.ch.finished() {
			if !mode.quiet {
				p.sendSelf(int(syscall.SIGPIPE))
			}

			if total > 0 {
				return total, nil
			}

			return 0, syscall.EPIPE
		}

		chunk := b[total:min(len(b), total+pipeChunk)]
		pk := packet{data: append([]byte(nil), chunk...)}

		select {
		case pp.ch.ch <- pk:
			pp.bytes.Add(int64(len(chunk)))
			pp.changed.bump()
			total += len(chunk)

			continue
		default:
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

	return total, nil
}

func (e *pipeEnd) pread(p *Process, b []byte, off int64) (int, error) {
	return 0, syscall.ESPIPE
}

func (e *pipeEnd) pwrite(p *Process, b []byte, off int64) (int, error) {
	return 0, syscall.ESPIPE
}
