//go:build linux && amd64

package main

import (
	"encoding/binary"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// A seqpacket pair is the kernel's, as a pipe is: two channels, one each
// way, a Go channel each with its buffer for the queue, and an end of the
// pair reads one and writes the other. A packet is bytes and the files
// sent with them, SCM_RIGHTS: files of the kernel pass as well as files
// of the host, and the receiver's table takes them. A read takes one
// packet, as much of it as fits, and its files only when a control buffer
// asks for them. The last close of an end closes the channel it wrote,
// EOF to the other end, and marks the one it read done, EPIPE to the
// other end's writes. These pairs carry the file protocol
// (kernel_remote.go): the kernel itself, calling a server for a process,
// waits on the channels as any goroutine does.
const (
	sockSeqpacket = 5
	packetMax     = 65536
	queueMax      = 64
	msgTrunc      = 0x20
	msgPeek       = 0x2
	soType        = 3
	soError       = 4
	soSndbuf      = 7
	soRcvbuf      = 8
	soPeercred    = 17
	shutRd        = 0
	shutWr        = 1
	shutRdwr      = 2
)

type packet struct {
	data  []byte
	files []File
}

// A channel carries packets one way. done is closed with its reader, so
// a writer learns of it; ch is closed by its writer.
type channel struct {
	ch   chan packet
	done chan struct{}
}

func newChannel() *channel {
	return &channel{ch: make(chan packet, queueMax), done: make(chan struct{})}
}

// finished tells whether the channel's reader is gone.
func (c *channel) finished() bool {
	select {
	case <-c.done:
		return true
	default:
		return false
	}
}

// drain drops what the channel holds, files and all, when nobody reads it
// any more.
func (c *channel) drain() {
	for {
		select {
		case pk, ok := <-c.ch:
			if !ok {
				return
			}

			dropFiles(pk.files)
		default:
			return
		}
	}
}

func dropFiles(files []File) {
	for _, f := range files {
		f.unref()
	}
}

// pairState is what both ends share: the waiters of both, the inode.
type pairState struct {
	waiters waitQueue
	ino     uint64
	made    time.Time
}

type pairEnd struct {
	shared *pairState
	peer   *pairEnd
	// to is what the end writes, from what it reads.
	to, from *channel
	refs     atomic.Int32
	nonblock atomic.Bool
	// dead is set once the end closed its channels; noWrite and noRead
	// are shutdown's.
	dead    atomic.Bool
	noWrite atomic.Bool
	noRead  atomic.Bool
	// front is a packet MSG_PEEK took out of the channel, the next read's.
	mu    sync.Mutex
	front *packet
}

var _ Socket = (*pairEnd)(nil)

var pairInodes atomic.Uint64

// newPair makes a seqpacket pair, each end with one reference.
func newPair(nonblock bool) (*pairEnd, *pairEnd) {
	s := &pairState{ino: pairInodes.Add(1), made: time.Now()}
	ab, ba := newChannel(), newChannel()
	a, b := &pairEnd{shared: s, to: ab, from: ba}, &pairEnd{shared: s, to: ba, from: ab}
	a.peer, b.peer = b, a

	for _, e := range []*pairEnd{a, b} {
		e.refs.Store(1)
		e.nonblock.Store(nonblock)
	}

	return a, b
}

func (e *pairEnd) ref() {
	e.refs.Add(1)
}

// endpoint is the end itself: what a directory of a server is made of
// (kernel_remote.go asRoot).
func (e *pairEnd) endpoint() *pairEnd {
	return e
}

// unref closes the end when its last reference goes: what it read is
// done, and drained; what it wrote is closed, and drained too when its
// reader went first, so no packet's files outlive both ends.
func (e *pairEnd) unref() {
	if e.refs.Add(-1) != 0 {
		return
	}

	close(e.from.done)
	e.from.drain()
	e.mu.Lock()

	if e.front != nil {
		dropFiles(e.front.files)
		e.front = nil
	}

	e.mu.Unlock()

	if e.to.finished() {
		e.to.drain()
	}

	close(e.to.ch)
	e.dead.Store(true)
	e.shared.waiters.wake()
}

func (e *pairEnd) watch(w *waitSet, entry *pollEntry) {
	w.queued(entry, &e.shared.waiters)
}

func (e *pairEnd) stat(st *syscall.Stat_t) error {
	s := e.shared
	*st = syscall.Stat_t{Ino: s.ino, Mode: syscall.S_IFSOCK | 0o777, Nlink: 1, Uid: uint32(syscall.Getuid()), Gid: uint32(syscall.Getgid()), Blksize: 4096}
	st.Atim = syscall.NsecToTimespec(s.made.UnixNano())
	st.Mtim, st.Ctim = st.Atim, st.Atim

	return nil
}

func (e *pairEnd) poll(events int16) int16 {
	mask := int16(0)
	e.mu.Lock()
	front := e.front != nil
	e.mu.Unlock()

	if front || len(e.from.ch) > 0 || e.peer.dead.Load() || e.peer.noWrite.Load() || e.noRead.Load() {
		mask |= pollIn | pollRdnorm
	}

	if e.peer.dead.Load() || e.to.finished() {
		mask |= pollHup | pollOut | pollWrnorm
	} else if len(e.to.ch) < cap(e.to.ch) {
		mask |= pollOut | pollWrnorm
	}

	return mask & (events | pollErr | pollHup)
}

func (e *pairEnd) flags() (int, error) {
	fl := syscall.O_RDWR

	if e.nonblock.Load() {
		fl |= syscall.O_NONBLOCK
	}

	return fl, nil
}

func (e *pairEnd) setFlags(fl int) error {
	e.nonblock.Store(fl&syscall.O_NONBLOCK != 0)

	return nil
}

// send queues a packet for the other end, blocking for room unless
// nonblock; the files are the packet's references. Without a process the
// wait is the kernel's own, on the channel. EPIPE comes without SIGPIPE,
// as Linux has it for these.
func (e *pairEnd) send(p *Process, data []byte, files []File, mode ioMode) error {
	if len(data) > packetMax {
		return syscall.EMSGSIZE
	}

	pk := packet{data: append([]byte(nil), data...), files: files}
	nonblock := mode.nonblock || e.nonblock.Load()

	for {
		if e.noWrite.Load() || e.to.finished() {
			return syscall.EPIPE
		}

		select {
		case e.to.ch <- pk:
			e.shared.waiters.wake()

			return nil
		default:
		}

		if nonblock {
			return syscall.EAGAIN
		}

		if p == nil {
			select {
			case e.to.ch <- pk:
				e.shared.waiters.wake()

				return nil
			case <-e.to.done:
				return syscall.EPIPE
			}
		}

		if _, err := p.waitObject(e, pollOut, -1, true); err != nil {
			return err
		}
	}
}

// take is the next packet without waiting: the one peeked, else the
// channel's; eof when the writer is gone, ok false when there is none
// yet.
func (e *pairEnd) take(peek bool) (pk packet, eof, ok bool) {
	e.mu.Lock()
	defer e.mu.Unlock()

	if e.front != nil {
		pk = *e.front

		if !peek {
			e.front = nil
		}

		return pk, false, true
	}

	if e.noRead.Load() {
		return packet{}, true, true
	}

	select {
	case pk, live := <-e.from.ch:
		if !live {
			return packet{}, true, true
		}

		if peek {
			e.front = &pk
		}

		return pk, false, true
	default:
	}

	// Nothing queued: the end of the data when the other end shut its
	// writing down.
	if e.peer.noWrite.Load() {
		return packet{}, true, true
	}

	return packet{}, false, false
}

// recv takes the next packet, or looks at it with peek: EOF is an empty
// packet with eof set. Without a process the wait is the kernel's own.
func (e *pairEnd) recv(p *Process, mode ioMode, peek bool) (packet, bool, error) {
	nonblock := mode.nonblock || e.nonblock.Load()

	for {
		pk, eof, ok := e.take(peek)

		if ok {
			if !eof && !peek {
				e.shared.waiters.wake()
			}

			return pk, eof, nil
		}

		if nonblock {
			return packet{}, false, syscall.EAGAIN
		}

		if p == nil {
			pk, live := <-e.from.ch

			if !live {
				return packet{}, true, nil
			}

			e.shared.waiters.wake()

			return pk, false, nil
		}

		if _, err := p.waitObject(e, pollIn, -1, true); err != nil {
			return packet{}, false, err
		}
	}
}

// receive reads a packet into the guest's buffers; its files go to a
// control buffer of room bytes at control when there is one, else they
// are dropped. It returns the bytes read, the control bytes written and
// the flags of the message: MSG_TRUNC, MSG_CTRUNC.
func (e *pairEnd) receive(p *Process, bufs [][]byte, flags uint32, control, room uint32) (int, uint32, uint32, error) {
	mode := ioMode{nonblock: flags&msgDontwait != 0}
	pk, eof, err := e.recv(p, mode, flags&msgPeek != 0)

	if err != nil || eof {
		return 0, 0, 0, err
	}

	n, out := 0, uint32(0)
	data := pk.data

	for _, b := range bufs {
		c := copy(b, data)
		data = data[c:]
		n += c
	}

	if len(data) > 0 {
		out |= msgTrunc
	}

	if flags&msgPeek != 0 {
		return n, 0, out, nil
	}

	written, ctrunc := p.putRights(pk.files, control, room, flags&msgCmsgCloexec != 0)

	return n, written, out | ctrunc, nil
}

func (e *pairEnd) read(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("read") {
		return 0, syscall.EIO
	}

	n, _, _, err := e.receive(p, [][]byte{b}, 0, 0, 0)

	return n, err
}

func (e *pairEnd) write(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("write") {
		return 0, syscall.EIO
	}

	if err := e.send(p, b, nil, mode); err != nil {
		return 0, err
	}

	return len(b), nil
}

func (e *pairEnd) pread(p *Process, b []byte, off int64) (int, error) {
	return 0, syscall.ESPIPE
}

func (e *pairEnd) pwrite(p *Process, b []byte, off int64) (int, error) {
	return 0, syscall.ESPIPE
}

// The socket calls: a pair is connected and unnamed.

func (e *pairEnd) bind(p *Process, at, n uint32) error {
	return syscall.EINVAL
}

func (e *pairEnd) connect(p *Process, at, n uint32) error {
	return syscall.EISCONN
}

func (e *pairEnd) listen(backlog int) error {
	return syscall.EINVAL
}

func (e *pairEnd) accept(p *Process, flags uint32) (File, []byte, error) {
	return nil, nil, syscall.EINVAL
}

func (e *pairEnd) sockname(p *Process) ([]byte, error) {
	return unixAddress(nil), nil
}

func (e *pairEnd) peername(p *Process) ([]byte, error) {
	return unixAddress(nil), nil
}

func (e *pairEnd) shutdown(how int) error {
	if how < shutRd || how > shutRdwr {
		return syscall.EINVAL
	}

	if how != shutWr {
		e.noRead.Store(true)
	}

	if how != shutRd {
		e.noWrite.Store(true)
	}

	e.shared.waiters.wake()

	return nil
}

func (e *pairEnd) getsockopt(p *Process, level, name, at, lenAt uint32) error {
	if level != solSocket {
		return syscall.ENOPROTOOPT
	}

	var value uint32

	switch name {
	case soType:
		value = sockSeqpacket
	case soError:
		value = 0
	case soSndbuf, soRcvbuf:
		value = queueMax * packetMax
	case soPeercred:
		// The peer is in this kernel: the ids are the process's own.
		b := make([]byte, 12)
		binary.LittleEndian.PutUint32(b, uint32(p.pid))
		binary.LittleEndian.PutUint32(b[4:], uint32(syscall.Getuid()))
		binary.LittleEndian.PutUint32(b[8:], uint32(syscall.Getgid()))
		length := min(p.u32(lenAt), 12)
		copy(p.span(at, length), b)
		p.put32(lenAt, length)

		return nil
	default:
		return syscall.ENOPROTOOPT
	}

	length := min(p.u32(lenAt), 4)
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], value)
	copy(p.span(at, length), b[:])
	p.put32(lenAt, length)

	return nil
}

func (e *pairEnd) setsockopt(p *Process, level, name, at, n uint32) error {
	if level != solSocket {
		return syscall.ENOPROTOOPT
	}

	return nil
}

func (e *pairEnd) sendto(p *Process, buf, n, flags, addr, alen uint32) (int, error) {
	if addr != 0 {
		return 0, syscall.EISCONN
	}

	mode := ioMode{nonblock: flags&msgDontwait != 0, quiet: flags&msgNosignal != 0}

	if err := e.send(p, p.span(buf, n), nil, mode); err != nil {
		return 0, err
	}

	return int(n), nil
}

func (e *pairEnd) recvfrom(p *Process, buf, n, flags uint32) (int, []byte, error) {
	got, _, _, err := e.receive(p, [][]byte{p.span(buf, n)}, flags, 0, 0)

	return got, unixAddress(nil), err
}

// sendmsg sends the guest's msghdr: its iovecs gathered, the descriptors
// of its SCM_RIGHTS as the packet's files.
func (e *pairEnd) sendmsg(p *Process, at, flags uint32) (int, error) {
	if p.u32(at) != 0 {
		return 0, syscall.EISCONN
	}

	spans, err := p.ioSpans(p.u32(at+8), p.u32(at+12))

	if err != nil {
		return 0, err
	}

	files, err := p.rights(p.u32(at+16), p.u32(at+20))

	if err != nil {
		return 0, err
	}

	var data []byte

	for _, b := range spans {
		data = append(data, b...)
	}

	mode := ioMode{nonblock: flags&msgDontwait != 0, quiet: flags&msgNosignal != 0}

	if err := e.send(p, data, files, mode); err != nil {
		dropFiles(files)

		return 0, err
	}

	return len(data), nil
}

// recvmsg reads a packet into the guest's msghdr: the iovecs scattered,
// the files into its control buffer, the flags into msg_flags.
func (e *pairEnd) recvmsg(p *Process, at, flags uint32) (int, error) {
	spans, err := p.ioSpans(p.u32(at+8), p.u32(at+12))

	if err != nil {
		return 0, err
	}

	n, written, out, err := e.receive(p, spans, flags, p.u32(at+16), p.u32(at+20))

	if err != nil {
		return 0, err
	}

	if name := p.u32(at); name != 0 {
		addr := unixAddress(nil)
		copy(p.span(name, min(p.u32(at+4), uint32(len(addr)))), addr)
		p.put32(at+4, uint32(len(addr)))
	}

	p.put32(at+20, written)
	p.put32(at+24, out|flags&msgCmsgCloexec)

	return n, nil
}

// rights is the files of the SCM_RIGHTS of a guest's control buffer, each
// with a reference for the packet; no other control message is known.
func (p *Process) rights(at, n uint32) ([]File, error) {
	var files []File

	for off := uint32(0); off+guestCmsgHeader <= n; {
		length := p.u32(at + off)

		if length < guestCmsgHeader || off+length > n {
			return nil, syscall.EINVAL
		}

		level, kind := p.u32(at+off+4), p.u32(at+off+8)

		if level != solSocket || kind != scmRights {
			return nil, syscall.EINVAL
		}

		data := p.span(at+off+guestCmsgHeader, length-guestCmsgHeader)

		for i := 0; i+4 <= len(data); i += 4 {
			f, err := p.fd(binary.LittleEndian.Uint32(data[i:]))

			if err != nil {
				dropFiles(files)

				return nil, err
			}

			f.ref()
			files = append(files, f)
		}

		off += guestAlign(length)
	}

	return files, nil
}

// putRights installs a packet's files as the process's descriptors and
// writes them as one SCM_RIGHTS to the guest's control buffer of room
// bytes; those that do not fit, or all of them without a buffer, are
// dropped with MSG_CTRUNC. It returns the control bytes written and the
// flag.
func (p *Process) putRights(files []File, at, room uint32, cloexec bool) (uint32, uint32) {
	if len(files) == 0 {
		return 0, 0
	}

	var fds []byte
	flags := uint32(0)

	for _, f := range files {
		if guestCmsgHeader+uint32(len(fds))+4 > room {
			f.unref()
			flags |= msgCtrunc

			continue
		}

		fd, err := p.install(f, 0, cloexec)

		if err != nil {
			flags |= msgCtrunc

			continue
		}

		fds = binary.LittleEndian.AppendUint32(fds, uint32(fd))
	}

	if len(fds) == 0 {
		return 0, flags
	}

	size := guestCmsgHeader + uint32(len(fds))
	p.put32(at, size)
	p.put32(at+4, solSocket)
	p.put32(at+8, scmRights)
	copy(p.span(at+guestCmsgHeader, uint32(len(fds))), fds)

	return min(guestAlign(size), room), flags
}
