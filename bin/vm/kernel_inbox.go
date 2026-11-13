//go:build linux && amd64

package main

import (
	"encoding/binary"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// The input of a host stream that can block, a terminal, a FIFO, a socket,
// is its inbox: a channel of packets a goroutine of the host layer fills by
// reading the host descriptor, so that to the kernel the stream is a
// channel as a pipe or a pair is, and a wait on it a select. A read of a
// file is a packet of bytes; a message of a socket is one with the
// sender's address, its control messages and the files of its SCM_RIGHTS;
// a connection a listening socket accepts is a packet whose file is the
// socket. The end of the data is an empty packet marked eof, the last one
// unless the stream is a terminal, whose end of file is one read; an
// error is the last packet. A reader that takes the last packet marks the
// inbox final, and the next read starts another.
//
// The goroutine starts with the first read or the first wait for input and
// stops when the stream closes: it sleeps in the host's poll on the
// descriptor and an eventfd of its own, which the stop writes, so it is
// gone before the descriptor closes. A read that finds the inbox empty
// while the host has input reads it in itself (help), in order with the
// goroutine, so a nonblocking read, or a poll with no time to wait, sees
// what the host has at once. The descriptor the inbox reads is
// nonblocking, then: the stream's own when the kernel opened it, else a
// description of its own (hostObject.inboxFd). A terminal's inbox is
// shared by the files open on the terminal, since each read of a terminal
// takes the next line whichever descriptor asks (terminals).

// inboxKind is what a stream is to its inbox: how it reads, and what a
// packet is.
type inboxKind int32

const (
	// inboxNone is a stream that does not block: no inbox, reads on the
	// host.
	inboxNone inboxKind = iota
	// inboxFile reads bytes: a FIFO, which ends with the data.
	inboxFile
	// inboxTerminal reads bytes too, and goes on after an end of file.
	inboxTerminal
	// inboxStream receives messages of a stream socket, read as bytes.
	inboxStream
	// inboxMessage receives messages of a datagram or seqpacket socket,
	// one a read.
	inboxMessage
	// inboxListen accepts connections, one a read.
	inboxListen
)

// message tells whether a read takes one packet.
func (k inboxKind) message() bool {
	return k == inboxMessage || k == inboxListen
}

func (k inboxKind) socket() bool {
	return k == inboxStream || k == inboxMessage || k == inboxListen
}

type inbox struct {
	fd   int
	kind inboxKind
	// own says fd is the inbox's own description, closed with it.
	own bool
	ch  chan packet
	// changed wakes the waits on the inbox with every packet; stop tells
	// the goroutine to end, with a write to event, and done says it did;
	// final is set by the read that took the last packet.
	changed changed
	stop    chan struct{}
	done    chan struct{}
	event   int
	final   atomic.Bool
	once    sync.Once
	// io is held, a token in it, by whoever reads the host into the
	// channel, the goroutine or a helping read, with the buffers they read
	// into; ended says the last packet is queued.
	io      chan struct{}
	buf     []byte
	control []byte
	ended   bool
	// mu guards front, the packets taken out of the channel by a peek or
	// left by a read that took part of one, the next read's first; bytes
	// counts the data queued, in front and in the channel, for FIONREAD.
	mu    sync.Mutex
	front []packet
	bytes atomic.Int64
}

const (
	sysEventfd2 = 290
	efdCloexec  = syscall.O_CLOEXEC
	efdNonblock = syscall.O_NONBLOCK
	// inboxRead is the most a read of a file or a stream takes at once,
	// inboxMessageMax the longest message a socket of messages can get
	// whole: the host's default socket buffer.
	inboxRead       = packetMax
	inboxMessageMax = 262144
	inboxControlMax = 1024
	tiocgdev        = 0x80045432
)

func eventfd() (int, error) {
	r, _, e := syscall.Syscall(sysEventfd2, 0, efdCloexec|efdNonblock, 0)

	if e != 0 {
		return -1, e
	}

	return int(r), nil
}

// startInbox makes the inbox of a descriptor and starts its goroutine; a
// failure is the host's, out of descriptors for the eventfd.
func startInbox(fd int, own bool, kind inboxKind) (*inbox, error) {
	event, err := eventfd()

	if err != nil {
		if own {
			syscall.Close(fd)
		}

		return nil, err
	}

	b := &inbox{fd: fd, kind: kind, own: own, ch: make(chan packet, queueMax), stop: make(chan struct{}), done: make(chan struct{}), event: event,
		io: make(chan struct{}, 1)}
	size := inboxRead

	if kind == inboxMessage {
		size = inboxMessageMax
	}

	b.buf = make([]byte, size)

	if kind.socket() {
		b.control = make([]byte, inboxControlMax)
	}

	go b.run()

	return b, nil
}

// run reads the descriptor into the channel until the stop or the last
// packet.
func (b *inbox) run() {
	defer func() {
		close(b.ch)
		b.changed.bump()
		close(b.done)
	}()

	for {
		revents, ok := b.wait()

		if !ok {
			return
		}

		b.io <- struct{}{}
		more := !b.ended && b.step(revents)
		<-b.io

		if !more {
			return
		}
	}
}

// step is one read of the host into the channel, with io held: the
// packet, or the error as the last packet; it tells whether the inbox
// goes on, false once the last packet is queued or the stop came. EAGAIN
// is nothing to read now.
func (b *inbox) step(revents int16) bool {
	pk, err := b.fetch(b.buf, b.control, revents)

	if err == syscall.EAGAIN || err == syscall.EINTR {
		return true
	}

	if err != nil {
		pk = packet{err: err}
	}

	last := pk.err != nil || pk.eof && b.kind != inboxTerminal

	if !b.put(pk) {
		return false
	}

	if last {
		b.ended = true
	}

	return !last
}

// help reads the host into the channel for a read that found the inbox
// empty, when the host has input now; it tells whether there is a packet
// to take: one it queued, or one the goroutine queued meanwhile, as it
// waits for the token or a change, whichever comes first, so a goroutine
// waiting for room in the channel never holds it up.
func (b *inbox) help() bool {
	changed := b.changed.wait()

	if b.ready() {
		return true
	}

	select {
	case b.io <- struct{}{}:
	case <-changed:
		return true
	}

	defer func() { <-b.io }()

	if b.ready() {
		return true
	}

	if b.ended || closed(b.stop) || pollNow(b.fd, pollIn)&(pollIn|pollHup|pollErr) == 0 {
		return false
	}

	b.step(pollIn)

	return b.ready()
}

// wait sleeps in the host's poll until the descriptor has input, or the
// stop is written: then ok is false.
func (b *inbox) wait() (revents int16, ok bool) {
	fds := [2]pollFd{{fd: int32(b.fd), events: pollIn}, {fd: int32(b.event), events: pollIn}}

	for {
		if closed(b.stop) {
			return 0, false
		}

		_, _, e := syscall.Syscall(syscall.SYS_POLL, uintptr(unsafe.Pointer(&fds[0])), 2, ^uintptr(0))

		if e == syscall.EINTR {
			continue
		}

		if e != 0 {
			return pollErr, true
		}

		if fds[1].revents != 0 || closed(b.stop) {
			return 0, false
		}

		return fds[0].revents, true
	}
}

// fetch is one read of the descriptor as a packet: of the kind's call,
// into buf, a socket's control messages into control.
func (b *inbox) fetch(buf, control []byte, revents int16) (packet, error) {
	switch b.kind {
	case inboxListen:
		addr := make([]byte, sockaddrMax)
		length := uint32(sockaddrMax)
		conn, err := sockcall(sysAccept4, uintptr(b.fd), bufferPointer(addr), uintptr(unsafe.Pointer(&length)), syscall.SOCK_CLOEXEC)

		if err != nil {
			return packet{}, err
		}

		return packet{files: []File{newSocket(conn, 0)}, addr: addr[:min(length, sockaddrMax)]}, nil
	case inboxStream, inboxMessage:
		addr := make([]byte, sockaddrMax)
		iov := syscall.Iovec{Base: &buf[0], Len: uint64(len(buf))}
		m := hostMsghdr{name: bufferPointer(addr), namelen: sockaddrMax, iov: uintptr(unsafe.Pointer(&iov)), iovlen: 1,
			control: bufferPointer(control), controllen: uint64(len(control))}
		n, err := sockcall(syscall.SYS_RECVMSG, uintptr(b.fd), uintptr(unsafe.Pointer(&m)), msgDontwait|msgCmsgCloexec)

		if err != nil {
			return packet{}, err
		}

		pk := packet{data: append([]byte(nil), buf[:n]...), addr: addr[:min(m.namelen, sockaddrMax)], flags: uint32(m.flags) &^ msgCmsgCloexec}
		pk.files, pk.control = splitControl(control[:min(m.controllen, uint64(len(control)))])

		// No bytes and no control is the end of a stream; of messages, an
		// empty one, unless the host says the peer hung up.
		if n == 0 && len(pk.files) == 0 && len(pk.control) == 0 && (b.kind == inboxStream || revents&pollHup != 0) {
			pk.eof = true
		}

		return pk, nil
	}

	n, err := syscall.Read(b.fd, buf)

	if err != nil {
		return packet{}, err
	}

	if n == 0 {
		return packet{eof: true}, nil
	}

	return packet{data: append([]byte(nil), buf[:n]...)}, nil
}

// put queues a packet, waiting for room unless stopped meanwhile.
func (b *inbox) put(pk packet) bool {
	select {
	case b.ch <- pk:
		b.bytes.Add(int64(len(pk.data)))
		b.changed.bump()

		return true
	case <-b.stop:
		dropFiles(pk.files)

		return false
	}
}

// halt stops the goroutine, waits for it, drops what is queued and closes
// the inbox's descriptors: once, by whoever lets the inbox go.
func (b *inbox) halt() {
	b.once.Do(func() {
		close(b.stop)
		one := [8]byte{1}
		syscall.Write(b.event, one[:])
		<-b.done

		for pk := range b.ch {
			dropFiles(pk.files)
		}

		b.flush()
		syscall.Close(b.event)

		if b.own {
			syscall.Close(b.fd)
		}
	})
}

// flush drops the packets in front: those of a terminal's input the guest
// flushes.
func (b *inbox) flush() {
	b.mu.Lock()
	defer b.mu.Unlock()

	for _, pk := range b.front {
		dropFiles(pk.files)
		b.bytes.Add(-int64(len(pk.data)))
	}

	b.front = nil
}

// drain drops what is queued, in front and in the channel: TCIFLUSH.
func (b *inbox) drain() {
	b.flush()

	for {
		select {
		case pk, live := <-b.ch:
			if !live {
				return
			}

			dropFiles(pk.files)
			b.bytes.Add(-int64(len(pk.data)))
		default:
			return
		}
	}
}

// ready tells whether a read would take something now.
func (b *inbox) ready() bool {
	b.mu.Lock()
	front := len(b.front) > 0
	b.mu.Unlock()

	return front || len(b.ch) > 0
}

// pull makes sure front holds i+1 packets, taking them out of the
// channel; false when the channel has no more.
func (b *inbox) pull(i int) bool {
	for i >= len(b.front) {
		select {
		case pk, live := <-b.ch:
			if !live {
				return false
			}

			b.front = append(b.front, pk)
		default:
			return false
		}
	}

	return true
}

// take is what the inbox has for a read now, into bufs, without waiting:
// of a stream, the bytes of the packets queued as far as they fit, up to
// one that carries files or control messages, which a read takes alone;
// of messages, one packet, what does not fit dropped with MSG_TRUNC. It
// returns the first packet, whole, with its address, control, files and
// flags, the bytes copied and ok, false with nothing queued. The last
// packet, eof or err, comes alone and marks the inbox final; it stays
// when first is false, a read that has bytes already. A peek copies and
// leaves everything, its files and control included.
func (b *inbox) take(bufs [][]byte, peek, first bool) (pk packet, n int, ok bool) {
	b.mu.Lock()
	defer b.mu.Unlock()

	if !b.pull(0) {
		return packet{}, 0, false
	}

	message := b.kind.message()
	room := 0

	for _, buf := range bufs {
		room += len(buf)
	}

	used, dropped := 0, 0

	for i := 0; b.pull(i); i++ {
		cur := &b.front[i]

		if i == 0 {
			pk = *cur

			if cur.eof || cur.err != nil {
				if !first {
					return pk, 0, true
				}

				used = 1

				break
			}
		} else if message || cur.eof || cur.err != nil || len(cur.files) > 0 || len(cur.control) > 0 || n >= room {
			break
		}

		c := scatter(bufs, n, cur.data)
		n += c

		if c < len(cur.data) {
			if message {
				pk.flags |= msgTrunc
				dropped += len(cur.data)
				used = i + 1
			} else if !peek {
				dropped += c
				*cur = packet{data: cur.data[c:]}
			}

			break
		}

		dropped += c
		used = i + 1

		if message {
			break
		}
	}

	if peek {
		pk.files, pk.control = nil, nil

		return pk, n, true
	}

	if used > 0 && (b.front[0].err != nil || b.front[0].eof && b.kind != inboxTerminal) {
		b.final.Store(true)
	}

	b.front = append(b.front[:0:0], b.front[used:]...)
	b.bytes.Add(-int64(dropped))

	return pk, n, true
}

// scatter copies data into bufs from offset at, as far as it fits.
func scatter(bufs [][]byte, at int, data []byte) int {
	copied := 0

	for _, buf := range bufs {
		if at >= len(buf) {
			at -= len(buf)

			continue
		}

		c := copy(buf[at:], data)
		data = data[c:]
		copied += c
		at = 0

		if len(data) == 0 {
			break
		}
	}

	return copied
}

// splitControl takes the host's control messages of a received message
// apart: the descriptors of SCM_RIGHTS become files, the others stay as
// they are, in the host's layout, for the guest's buffer.
func splitControl(host []byte) ([]File, []byte) {
	var files []File
	var rest []byte

	for len(host) >= hostCmsgHeader {
		length := int(binary.LittleEndian.Uint64(host))

		if length < hostCmsgHeader || length > len(host) {
			break
		}

		level, kind := binary.LittleEndian.Uint32(host[8:]), binary.LittleEndian.Uint32(host[12:])
		data := host[hostCmsgHeader:length]

		if level == solSocket && kind == scmRights {
			for i := 0; i+4 <= len(data); i += 4 {
				files = append(files, newHostObject(int(binary.LittleEndian.Uint32(data[i:])), true))
			}
		} else {
			rest = append(rest, host[:length]...)
			rest = append(rest, make([]byte, hostAlign(len(rest))-len(rest))...)
		}

		host = host[min(hostAlign(length), len(host)):]
	}

	return files, rest
}

// An inboxSlot holds the inbox of a stream, or of a terminal, while it
// has one; a final one goes when the next read asks.
type inboxSlot struct {
	mu sync.Mutex
	in *inbox
}

// current is the inbox there is, nil for none or a final one.
func (s *inboxSlot) current() *inbox {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.in == nil || s.in.final.Load() {
		return nil
	}

	return s.in
}

// get is the inbox, started with start when there is none.
func (s *inboxSlot) get(start func() (*inbox, error)) (*inbox, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.in != nil && s.in.final.Load() {
		s.in.halt()
		s.in = nil
	}

	if s.in == nil {
		in, err := start()

		if err != nil {
			return nil, err
		}

		s.in = in
	}

	return s.in, nil
}

// close halts the inbox there is.
func (s *inboxSlot) close() {
	s.mu.Lock()
	in := s.in
	s.in = nil
	s.mu.Unlock()

	if in != nil {
		in.halt()
	}
}

// terminals are the terminals with files open, by the terminal's device
// and side: a file of a terminal joins when made and leaves with its last
// close; the terminal's inbox starts with the first read of any of them,
// on a description of that file's, and ends with the last file.
var terminals = struct {
	mu   sync.Mutex
	open map[terminalKey]*terminal
}{open: map[terminalKey]*terminal{}}

// terminalKey tells a terminal: the device TIOCGDEV names, which is the
// terminal behind /dev/tty, and whether the file is the master side of a
// pseudoterminal, as TIOCGDEV names the slave for both.
type terminalKey struct {
	dev    uint64
	master bool
}

type terminal struct {
	key   terminalKey
	files int
	slot  inboxSlot
}

const ptmxDevice = 5<<8 | 2

// isTerminal tells whether a character device is a terminal: TCGETS says.
func isTerminal(fd int) bool {
	var t hostTermios

	return ioctlPtr(fd, tcgets, unsafe.Pointer(&t)) == nil
}

// joinTerminal is the terminal of a descriptor, with the file counted.
func joinTerminal(fd int) *terminal {
	var st syscall.Stat_t
	syscall.Fstat(fd, &st)
	key := terminalKey{dev: st.Rdev, master: st.Rdev == ptmxDevice}
	var dev uint32

	if ioctlPtr(fd, tiocgdev, unsafe.Pointer(&dev)) == nil {
		key.dev = uint64(dev)
	}

	terminals.mu.Lock()
	defer terminals.mu.Unlock()
	t := terminals.open[key]

	if t == nil {
		t = &terminal{key: key}
		terminals.open[key] = t
	}

	t.files++

	return t
}

// leave takes a file off the terminal; the last one ends its inbox.
func (t *terminal) leave() {
	terminals.mu.Lock()
	t.files--
	last := t.files == 0

	if last {
		delete(terminals.open, t.key)
	}

	terminals.mu.Unlock()

	if last {
		t.slot.close()
	}
}
