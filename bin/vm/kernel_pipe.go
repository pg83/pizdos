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
// the channel's change of its side, the reader on what fills it, the
// writer on what drains it.
//
// The pipe of a FIFO (kernel_fifo.go) has a name, and ends join it as the
// FIFO is opened: a reader that joins after the readers have gone, or a
// writer after the writers, starts it again on a new channel, which takes
// what there was to read.

const pipeChunk = 4096

type pipe struct {
	// ch is the channel, a FIFO's new one when it starts again; eof says
	// its writers closed it. front is what a read left of the packet it
	// took, the next read's; bytes counts it and the packets queued, for
	// FIONREAD; readers and writers count the open ends, readerOpens and
	// writerOpens the ends that ever joined. mu guards them, and the
	// channel's sends and takes, so a take knows whether it made room in a
	// full pipe.
	mu          sync.Mutex
	ch          *channel
	eof         bool
	front       []byte
	bytes       atomic.Int64
	readers     int
	writers     int
	readerOpens uint64
	writerOpens uint64
	// opened changes as an end joins, for the opens of a FIFO that wait for
	// the other side; name is a FIFO's.
	opened changed
	name   *fifoName
	ino    uint64
	made   time.Time
}

// pipeEnd is one end of a pipe, an open file: the reader, the writer, or
// both, a FIFO's opened O_RDWR.
type pipeEnd struct {
	pipe     *pipe
	reads    bool
	writes   bool
	refs     atomic.Int32
	nonblock atomic.Bool
}

var (
	_ Transfer = (*pipeEnd)(nil)
	_ Ioctl    = (*pipeEnd)(nil)
)

var pipeInodes atomic.Uint64

// newPipe is a pipe without an end.
func newPipe() *pipe {
	return &pipe{ch: newChannel(), ino: pipeInodes.Add(1), made: time.Now()}
}

// pipe makes a pipe and installs its ends at the lowest descriptors.
func (p *Process) pipe(at, flags uint32) uint32 {
	if flags&^(syscall.O_CLOEXEC|syscall.O_NONBLOCK) != 0 {
		return p.result(0, syscall.EINVAL)
	}

	pp := newPipe()
	pp.readers, pp.writers = 1, 1
	ends := [2]*pipeEnd{{pipe: pp, reads: true}, {pipe: pp, writes: true}}
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

// channel is the pipe's channel now.
func (pp *pipe) channel() *channel {
	pp.mu.Lock()
	defer pp.mu.Unlock()

	return pp.ch
}

// join adds an end to a FIFO's pipe, and tells how many readers and
// writers have joined it, this one too: a reader that joins after the
// readers have gone, or a writer after the writers, starts the pipe on a
// new channel, with what there was to read.
func (pp *pipe) join(reads, writes bool) (uint64, uint64) {
	pp.mu.Lock()
	old := pp.ch
	renew := reads && pp.readers == 0 && old.finished() || writes && pp.writers == 0 && pp.eof

	if renew {
		ch := newChannel()

		for moving := !old.finished(); moving; {
			select {
			case pk, live := <-old.ch:
				if live {
					ch.ch <- pk
				}

				moving = live
			default:
				moving = false
			}
		}

		pp.ch, pp.eof = ch, false
	}

	if reads {
		pp.readers++
		pp.readerOpens++
	}

	if writes {
		pp.writers++
		pp.writerOpens++
	}

	readerOpens, writerOpens := pp.readerOpens, pp.writerOpens
	pp.mu.Unlock()

	if renew {
		old.filled.bump()
		old.drained.bump()
	}

	pp.opened.bump()

	return readerOpens, writerOpens
}

// met tells whether an open of one side of a FIFO, after the joins it saw,
// has its other side: an end there now, or one that joined since.
func (pp *pipe) met(reads bool, readerOpens, writerOpens uint64) bool {
	pp.mu.Lock()
	defer pp.mu.Unlock()

	if reads {
		return pp.writers > 0 || pp.writerOpens > writerOpens
	}

	return pp.readers > 0 || pp.readerOpens > readerOpens
}

func (e *pipeEnd) ref() {
	e.refs.Add(1)
}

// unref closes the end when its last reference goes: the last reader
// finishes the channel, so a writer learns of it, the last writer closes
// it, so a reader sees the end of the data. A FIFO's pipe with no end left
// leaves the kernel's table.
func (e *pipeEnd) unref() {
	if e.refs.Add(-1) != 0 {
		return
	}

	pp := e.pipe
	pp.mu.Lock()
	ch := pp.ch
	ended, finished := false, false

	if e.writes {
		pp.writers--

		if pp.writers == 0 {
			close(ch.ch)
			pp.eof, ended = true, true
		}
	}

	if e.reads {
		pp.readers--

		if pp.readers == 0 {
			close(ch.done)
			ch.drain()
			pp.front = nil
			pp.bytes.Store(0)
			finished = true
		}
	}

	gone := pp.readers == 0 && pp.writers == 0
	pp.mu.Unlock()

	if ended {
		ch.filled.bump()
	}

	if finished {
		ch.drained.bump()
	}

	if gone && pp.name != nil {
		pp.name.forget(pp)
	}
}

// changes are the channel's of the end's sides: what fills it for a
// reader, what drains it for a writer.
func (e *pipeEnd) changes(events int16) []<-chan struct{} {
	ch := e.pipe.channel()
	in, out := e.reads, e.writes

	if in && out && events&(pollIn|pollRdnorm|pollOut|pollWrnorm) != 0 {
		in, out = events&(pollIn|pollRdnorm) != 0, events&(pollOut|pollWrnorm) != 0
	}

	var chans []<-chan struct{}

	if in {
		chans = append(chans, ch.filled.wait())
	}

	if out {
		chans = append(chans, ch.drained.wait())
	}

	return chans
}

// stat is a FIFO's: the ends of a pipe share an inode of the kernel's, the
// ends of a FIFO's pipe the FIFO's.
func (e *pipeEnd) stat(st *syscall.Stat_t) error {
	pp := e.pipe
	*st = syscall.Stat_t{Ino: pp.ino, Mode: syscall.S_IFIFO | 0o600, Nlink: 1, Uid: uint32(syscall.Getuid()), Gid: uint32(syscall.Getgid()), Blksize: pipeChunk}

	if n := pp.name; n != nil {
		st.Dev, st.Ino, st.Mode = n.key.dev, n.key.ino, n.mode
	}

	st.Atim = syscall.NsecToTimespec(pp.made.UnixNano())
	st.Mtim, st.Ctim = st.Atim, st.Atim

	return nil
}

func (e *pipeEnd) poll(events int16) int16 {
	pp := e.pipe
	pp.mu.Lock()
	ch, front, readers, writers := pp.ch, len(pp.front), pp.readers, pp.writers
	queued := len(ch.ch)
	pp.mu.Unlock()
	mask := int16(0)

	if e.writes {
		if queued < cap(ch.ch) {
			mask |= pollOut | pollWrnorm
		}

		if readers == 0 {
			mask |= pollErr
		}
	}

	if e.reads {
		if front > 0 || queued > 0 || writers == 0 {
			mask |= pollIn | pollRdnorm
		}

		if writers == 0 {
			mask |= pollHup
		}
	}

	return mask & (events | pollErr | pollHup)
}

func (e *pipeEnd) flags() (int, error) {
	fl := syscall.O_RDWR

	if !e.writes {
		fl = syscall.O_RDONLY
	} else if !e.reads {
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
	if req != fionread || !e.reads {
		return syscall.ENOTTY
	}

	*(*int32)(arg) = int32(e.pipe.bytes.Load())

	return nil
}

// take is what the pipe has for a read now, into b, as far as it fits:
// what a read left first, then the packets queued, of which what does not
// fit is left. ok is false with nothing queued, eof with the writers gone
// and nothing left; freed says a full pipe has room now, a change to its
// writers, of the channel ch.
func (pp *pipe) take(b []byte) (n int, eof, ok, freed bool, ch *channel) {
	pp.mu.Lock()
	defer pp.mu.Unlock()

	ch = pp.ch

	for n < len(b) {
		if len(pp.front) == 0 {
			full := len(ch.ch) == cap(ch.ch)

			select {
			case pk, live := <-ch.ch:
				if !live {
					pp.bytes.Add(-int64(n))

					return n, n == 0, true, freed, ch
				}

				pp.front = pk.data
				freed = freed || full
			default:
				pp.bytes.Add(-int64(n))

				return n, false, n > 0, freed, ch
			}
		}

		c := copy(b[n:], pp.front)
		pp.front = pp.front[c:]
		n += c
	}

	pp.bytes.Add(-int64(n))

	return n, false, true, freed, ch
}

func (e *pipeEnd) read(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("read") {
		return 0, syscall.EIO
	}

	if !e.reads {
		return 0, syscall.EBADF
	}

	if len(b) == 0 {
		return 0, nil
	}

	pp := e.pipe

	for {
		n, _, ok, freed, ch := pp.take(b)

		if ok {
			if freed {
				ch.drained.bump()
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

	if !e.writes {
		return 0, syscall.EBADF
	}

	pp := e.pipe
	total := 0

	for total < len(b) {
		chunk := b[total:min(len(b), total+pipeChunk)]
		pk := packet{data: append([]byte(nil), chunk...)}

		// Under the pipe's lock, as take is: so take knows whether the
		// pipe was full when it made room.
		pp.mu.Lock()
		ch := pp.ch
		finished, sent := ch.finished(), false

		if !finished {
			select {
			case ch.ch <- pk:
				pp.bytes.Add(int64(len(chunk)))
				sent = true
			default:
			}
		}

		pp.mu.Unlock()

		if finished {
			if !mode.quiet {
				p.sendSelf(int(syscall.SIGPIPE))
			}

			if total > 0 {
				return total, nil
			}

			return 0, syscall.EPIPE
		}

		if sent {
			ch.filled.bump()
			total += len(chunk)

			continue
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
