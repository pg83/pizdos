//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"slices"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// Sockets are the host's: a socket a process makes is a host socket of the
// kernel's, nonblocking on the host as every pollable file it owns
// (kernel_host.go); what it receives, messages or connections, comes
// through its inbox (kernel_inbox.go), what it sends goes to the host as
// it is sent. The address structures of lib/c are Linux's, so most
// addresses go to the host as they are: AF_INET and AF_INET6 are the host's
// network. An AF_UNIX path resolves inside the root, as every path does,
// through the /proc/self/fd path of its directory, to bind, or of the
// socket, to connect; an abstract name lives under this kernel's prefix, so
// a process reaches neither the host's abstract sockets nor another
// kernel's. The kernel keeps the guest path of each socket bound to one and
// gives it back for the host's /proc path.

const (
	afUnix         = 1
	afInet         = 2
	afInet6        = 10
	sockNonblock   = 0o4000
	sockCloexec    = 0o2000000
	msgDontwait    = 0x40
	msgWaitall     = 0x100
	msgNosignal    = 0x4000
	msgCtrunc      = 0x8
	msgCmsgCloexec = 0x40000000
	solSocket      = 1
	soRcvtimeo     = 20
	soSndtimeo     = 21
	soAcceptconn   = 30
	sockStreamType = 1
	scmRights      = 1
	sunPathMax     = 108
	sockaddrMax    = 128
	siocatmark     = 0x8905
	sysAccept4     = 288
)

// hostSocket is a socket of the host; unixName is the guest path an
// AF_UNIX socket is bound to.
type hostSocket struct {
	hostStream
	unixName atomic.Pointer[string]
}

var (
	_ Socket = (*hostSocket)(nil)
	_ Ioctl  = (*hostSocket)(nil)
)

// newSocket makes the object of a host socket, with the guest's
// SOCK_NONBLOCK.
func newSocket(host int, flags uint32) *hostSocket {
	s := &hostSocket{}
	s.init(host, true, socketInboxKind(host))
	s.nonblock.Store(flags&sockNonblock != 0)

	return s
}

// socketInboxKind is what a host socket is to its inbox: one listening accepts,
// a stream reads bytes, the others messages.
func socketInboxKind(fd int) inboxKind {
	if v, err := syscall.GetsockoptInt(fd, syscall.SOL_SOCKET, soAcceptconn); err == nil && v != 0 {
		return inboxListen
	}

	if v, err := syscall.GetsockoptInt(fd, syscall.SOL_SOCKET, soType); err == nil && v != sockStreamType {
		return inboxMessage
	}

	return inboxStream
}

func unixAddress(path []byte) []byte {
	return append([]byte{afUnix, 0}, path...)
}

// withAddress runs a call with the host's address of a guest's: for bind
// the path names a socket to make, else one there is.
func (p *Process) withAddress(at, n uint32, bind bool, call func(addr []byte) error) error {
	if n > sockaddrMax {
		return syscall.EINVAL
	}

	addr := slices.Clone(p.span(at, n))

	if n <= 2 || binary.LittleEndian.Uint16(addr) != afUnix {
		return call(addr)
	}

	path := addr[2:]

	if path[0] == 0 {
		name := append(append([]byte{0}, p.k.abstract...), path[1:]...)

		if len(name) > sunPathMax {
			return syscall.EINVAL
		}

		return call(unixAddress(name))
	}

	if i := bytes.IndexByte(path, 0); i >= 0 {
		path = path[:i]
	}

	guest := string(path)
	full, err := p.full(guest)

	if err != nil {
		return err
	}

	// A path names a socket of the host: in a root of the host.
	root, ok := p.root.(*hostDir)

	if !ok {
		return syscall.EOPNOTSUPP
	}

	if bind {
		return root.onName(full, syscall.EADDRINUSE, func(dirfd int, name string) error {
			host := fmt.Sprintf("/proc/self/fd/%d/%s", dirfd, name)

			if len(host) >= sunPathMax {
				return syscall.ENAMETOOLONG
			}

			err := call(unixAddress([]byte(host)))

			if err == nil {
				p.k.unixNames.Store(host, guest)
			}

			return err
		})
	}

	return root.onPath(full, true, func(fd int) error {
		return call(unixAddress([]byte(procFd(fd))))
	})
}

// guestAddress is the guest's address of a host's: an abstract name
// without this kernel's prefix, a path the guest's it was bound by.
func (p *Process) guestAddress(host []byte) []byte {
	if len(host) <= 2 || binary.LittleEndian.Uint16(host) != afUnix {
		return host
	}

	path := host[2:]

	if path[0] == 0 {
		if rest, ok := bytes.CutPrefix(path[1:], []byte(p.k.abstract)); ok {
			return unixAddress(append([]byte{0}, rest...))
		}

		return host
	}

	if i := bytes.IndexByte(path, 0); i >= 0 {
		path = path[:i]
	}

	if guest, ok := p.k.unixNames.Load(string(path)); ok {
		return append(unixAddress([]byte(guest.(string))), 0)
	}

	return append(unixAddress(path), 0)
}

// putAddress writes an address the way accept and getsockname do: as much
// as the buffer at at holds, and the whole length at lenAt.
func (p *Process) putAddress(addr []byte, at, lenAt uint32) {
	if at == 0 || lenAt == 0 {
		return
	}

	room := p.u32(lenAt)
	copy(p.span(at, min(room, uint32(len(addr)))), addr)
	p.put32(lenAt, uint32(len(addr)))
}

// sockcall makes a socket call of the host; its arguments may be pointers,
// which the directive keeps where they are for the call.
//
//go:uintptrescapes
func sockcall(trap uintptr, a ...uintptr) (int, error) {
	args := [6]uintptr{}
	copy(args[:], a)
	r, _, e := syscall.Syscall6(trap, args[0], args[1], args[2], args[3], args[4], args[5])

	if e != 0 {
		return 0, e
	}

	return int(r), nil
}

func bufferPointer(b []byte) uintptr {
	if len(b) == 0 {
		return 0
	}

	return uintptr(unsafe.Pointer(&b[0]))
}

// installSocket makes a host socket a descriptor, with the guest's
// SOCK_NONBLOCK and SOCK_CLOEXEC.
func (p *Process) installSocket(host int, flags uint32) (int, error) {
	return p.install(newSocket(host, flags), 0, flags&sockCloexec != 0)
}

func (s *hostSocket) bind(p *Process, at, n uint32) error {
	err := p.withAddress(at, n, true, func(a []byte) error {
		_, err := sockcall(syscall.SYS_BIND, uintptr(s.fd), bufferPointer(a), uintptr(len(a)))

		return err
	})

	if err == nil && n > 2 && p.u16(at) == afUnix && p.u8(at+2) != 0 {
		name := p.str(at + 2)
		s.unixName.Store(&name)
	}

	return err
}

// connect connects as a blocking socket does, unless the guest asked for a
// nonblocking one: it waits for the host's connection and its error.
func (s *hostSocket) connect(p *Process, at, n uint32) error {
	return p.withAddress(at, n, false, func(addr []byte) error {
		return s.connectTo(p, addr)
	})
}

func (s *hostSocket) connectTo(p *Process, addr []byte) error {
	timeout := s.timeout(pollOut)
	var deadline time.Time

	if timeout >= 0 {
		deadline = time.Now().Add(time.Duration(timeout) * time.Millisecond)
	}

	for {
		_, err := sockcall(syscall.SYS_CONNECT, uintptr(s.fd), bufferPointer(addr), uintptr(len(addr)))

		if err == syscall.EINTR {
			continue
		}

		if s.nonblock.Load() || err != syscall.EINPROGRESS && err != syscall.EAGAIN {
			return err
		}

		if err == syscall.EAGAIN {
			// A full UNIX listen queue leaves this socket unconnected: poll
			// reports OUT|HUP immediately. Retry after an interruptible wait,
			// keeping one send deadline across retries, as Linux connect does.
			wait := 10

			if timeout >= 0 {
				remaining := time.Until(deadline)

				if remaining <= 0 {
					return syscall.EAGAIN
				}

				wait = min(wait, int((remaining+time.Millisecond-1)/time.Millisecond))
			}

			if werr := p.idle(wait, timeout < 0); werr != nil {
				return werr
			}

			continue
		}

		ready, werr := p.waitObject(s, pollOut, timeout, timeout < 0)

		if werr != nil {
			return werr
		}

		if ready == 0 {
			// SO_SNDTIMEO ends the guest's wait; the host's nonblocking
			// connection still runs and may finish after this call returns.
			return syscall.EINPROGRESS
		}

		code, gerr := syscall.GetsockoptInt(s.fd, syscall.SOL_SOCKET, syscall.SO_ERROR)

		if gerr != nil {
			return gerr
		}

		if code != 0 {
			return syscall.Errno(code)
		}

		return nil
	}
}

// configured tells whether a family has an address: as musl's AI_ADDRCONFIG,
// a datagram socket of it connects to its loopback.
func configured(family int) bool {
	fd, err := syscall.Socket(family, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, ipprotoUDP)

	if err != nil {
		return false
	}

	defer syscall.Close(fd)

	if family == afInet {
		return syscall.Connect(fd, &syscall.SockaddrInet4{Port: 65535, Addr: [4]byte{127, 0, 0, 1}}) == nil
	}

	return syscall.Connect(fd, &syscall.SockaddrInet6{Port: 65535, Addr: [16]byte{15: 1}}) == nil
}

// listen makes the socket's inbox accept from now on.
func (s *hostSocket) listen(backlog int) error {
	if err := syscall.Listen(s.fd, backlog); err != nil {
		return err
	}

	s.slot.close()
	s.kind.Store(int32(inboxListen))

	return nil
}

// accept takes a connection from the inbox, and returns it with the
// peer's guest address.
func (s *hostSocket) accept(p *Process, flags uint32) (File, []byte, error) {
	if inboxKind(s.kind.Load()) != inboxListen {
		return nil, nil, syscall.EINVAL
	}

	_, pk, err := s.receive(p, nil, 0, ioMode{})

	if err != nil {
		return nil, nil, err
	}

	if len(pk.files) == 0 {
		return nil, nil, syscall.EINVAL
	}

	conn := pk.files[0].(*hostSocket)
	conn.nonblock.Store(flags&sockNonblock != 0)

	return conn, p.guestAddress(pk.addr), nil
}

// recvfrom receives into a guest buffer, with MSG_WAITALL until it is full
// or the stream ends; the sender's address is the guest's. With MSG_TRUNC
// the count is a message's whole length.
func (s *hostSocket) recvfrom(p *Process, buf, n, flags uint32) (int, []byte, error) {
	got, pk, err := s.receive(p, [][]byte{p.span(buf, n)}, flags, ioMode{nonblock: flags&msgDontwait != 0})

	if err != nil {
		return 0, nil, err
	}

	dropFiles(pk.files)

	if flags&msgTrunc != 0 && inboxKind(s.kind.Load()).message() {
		got = len(pk.data)
	}

	return got, p.guestAddress(pk.addr), nil
}

func (s *hostSocket) sendto(p *Process, buf, n, flags, addr, alen uint32) (int, error) {
	mode := ioMode{nonblock: flags&msgDontwait != 0, quiet: flags&msgNosignal != 0}
	send := func(to []byte) (int, error) {
		return s.transfer(p, pollOut, mode, func() (int, error) {
			return sockcall(syscall.SYS_SENDTO, uintptr(s.fd), bufferPointer(p.span(buf, n)), uintptr(n), uintptr(flags|msgNosignal),
				bufferPointer(to), uintptr(len(to)))
		})
	}

	if addr == 0 {
		return send(nil)
	}

	sent := 0
	err := p.withAddress(addr, alen, false, func(to []byte) error {
		var e error
		sent, e = send(to)

		return e
	})

	return sent, err
}

// Messages. A guest's msghdr is seven words; its control messages are
// headed by three words and aligned to four bytes, the host's by a long
// and two words and aligned to eight.
const (
	guestCmsgHeader = 12
	hostCmsgHeader  = 16
)

func guestAlign(n uint32) uint32 {
	return (n + 3) &^ 3
}

func hostAlign(n int) int {
	return (n + 7) &^ 7
}

// hostControl is the host's control messages of a guest's: the descriptors
// of SCM_RIGHTS become the host's; a file of the kernel has none for the
// host to pass, EINVAL.
func (p *Process) hostControl(at, n uint32) ([]byte, error) {
	var out []byte

	for off := uint32(0); off+guestCmsgHeader <= n; {
		length := p.u32(at + off)

		if length < guestCmsgHeader || off+length > n {
			return nil, syscall.EINVAL
		}

		level, kind := p.u32(at+off+4), p.u32(at+off+8)
		data := p.span(at+off+guestCmsgHeader, length-guestCmsgHeader)

		if level == solSocket && kind == scmRights {
			fds := make([]byte, 0, len(data))

			for i := 0; i+4 <= len(data); i += 4 {
				obj, err := p.fd(binary.LittleEndian.Uint32(data[i:]))

				if err != nil {
					return nil, err
				}

				fd, ok := hostFdOf(obj)

				if !ok {
					return nil, syscall.EINVAL
				}

				fds = binary.LittleEndian.AppendUint32(fds, uint32(fd))
			}

			data = fds
		}

		head := make([]byte, hostCmsgHeader)
		binary.LittleEndian.PutUint64(head, uint64(hostCmsgHeader+len(data)))
		binary.LittleEndian.PutUint32(head[8:], level)
		binary.LittleEndian.PutUint32(head[12:], kind)
		out = append(append(out, head...), data...)
		out = append(out, make([]byte, hostAlign(len(out))-len(out))...)
		off += guestAlign(length)
	}

	return out, nil
}

// putControl writes the host's control messages of a received message,
// those that are not SCM_RIGHTS (splitControl), to a guest's buffer of
// room bytes at at, in the guest's layout; one that does not fit sets
// MSG_CTRUNC and ends them. It returns the length written and the flag.
func (p *Process) putControl(host []byte, at, room uint32) (uint32, uint32) {
	off := uint32(0)

	for len(host) >= hostCmsgHeader {
		length := int(binary.LittleEndian.Uint64(host))

		if length < hostCmsgHeader || length > len(host) {
			break
		}

		level, kind := binary.LittleEndian.Uint32(host[8:]), binary.LittleEndian.Uint32(host[12:])
		data := host[hostCmsgHeader:length]
		host = host[min(hostAlign(length), len(host)):]
		size := guestCmsgHeader + uint32(len(data))

		if off+size > room {
			return off, msgCtrunc
		}

		p.put32(at+off, size)
		p.put32(at+off+4, level)
		p.put32(at+off+8, kind)
		copy(p.span(at+off+guestCmsgHeader, uint32(len(data))), data)
		off += min(guestAlign(size), room-off)
	}

	return off, 0
}

// hostMsghdr is the host's struct msghdr.
type hostMsghdr struct {
	name       uintptr
	namelen    uint32
	_          uint32
	iov        uintptr
	iovlen     uint64
	control    uintptr
	controllen uint64
	flags      int32
	_          int32
}

func (s *hostSocket) sendmsg(p *Process, at, flags uint32) (int, error) {
	iov, err := p.iovecs(p.u32(at+8), p.u32(at+12))

	if err != nil {
		return 0, err
	}

	control, err := p.hostControl(p.u32(at+16), p.u32(at+20))

	if err != nil {
		return 0, err
	}

	send := func(addr []byte) (int, error) {
		mode := ioMode{nonblock: flags&msgDontwait != 0, quiet: flags&msgNosignal != 0}

		return s.transfer(p, pollOut, mode, func() (int, error) {
			m := hostMsghdr{name: bufferPointer(addr), namelen: uint32(len(addr)), control: bufferPointer(control), controllen: uint64(len(control))}

			if len(iov) > 0 {
				m.iov, m.iovlen = uintptr(unsafe.Pointer(&iov[0])), uint64(len(iov))
			}

			return sockcall(syscall.SYS_SENDMSG, uintptr(s.fd), uintptr(unsafe.Pointer(&m)), uintptr(flags|msgNosignal))
		})
	}

	if name := p.u32(at); name != 0 {
		var n int
		err := p.withAddress(name, p.u32(at+4), false, func(addr []byte) error {
			var e error
			n, e = send(addr)

			return e
		})

		return n, err
	}

	return send(nil)
}

// recvmsg takes a message from the inbox into the guest's msghdr: the
// iovecs scattered, the sender's address, the files of SCM_RIGHTS
// installed and the other control messages written into its control
// buffer, the flags into msg_flags.
func (s *hostSocket) recvmsg(p *Process, at, flags uint32) (int, error) {
	spans, err := p.ioSpans(p.u32(at+8), p.u32(at+12))

	if err != nil {
		return 0, err
	}

	n, pk, err := s.receive(p, spans, flags, ioMode{nonblock: flags&msgDontwait != 0})

	if err != nil {
		return 0, err
	}

	if name := p.u32(at); name != 0 {
		guest := p.guestAddress(pk.addr)
		copy(p.span(name, min(p.u32(at+4), uint32(len(guest)))), guest)
		p.put32(at+4, uint32(len(guest)))
	}

	control, room := p.u32(at+16), p.u32(at+20)
	written, truncated := p.putRights(pk.files, control, room, flags&msgCmsgCloexec != 0)
	more, cut := p.putControl(pk.control, control+written, room-written)
	p.put32(at+20, written+more)
	p.put32(at+24, pk.flags|flags&msgCmsgCloexec|truncated|cut)

	if flags&msgTrunc != 0 && inboxKind(s.kind.Load()).message() {
		n = len(pk.data)
	}

	return n, nil
}

// sockopt is a socket's option: the timeouts the kernel keeps, the host's
// others.
func (s *hostSocket) getsockopt(p *Process, level, name, at, lenAt uint32) error {
	if level == solSocket && (name == soRcvtimeo || name == soSndtimeo) {
		ms := s.recvTimeout.Load()

		if name == soSndtimeo {
			ms = s.sendTimeout.Load()
		}

		var b [16]byte
		binary.LittleEndian.PutUint64(b[:], uint64(ms/1000))
		binary.LittleEndian.PutUint64(b[8:], uint64(ms%1000*1000))
		length := min(p.u32(lenAt), 16)
		copy(p.span(at, length), b[:])
		p.put32(lenAt, length)

		return nil
	}

	length := min(p.u32(lenAt), 1024)
	buf := make([]byte, length)

	if _, err := sockcall(syscall.SYS_GETSOCKOPT, uintptr(s.fd), uintptr(level), uintptr(name), bufferPointer(buf), uintptr(unsafe.Pointer(&length))); err != nil {
		return err
	}

	copy(p.span(at, min(length, uint32(len(buf)))), buf)
	p.put32(lenAt, length)

	return nil
}

func (s *hostSocket) setsockopt(p *Process, level, name, at, n uint32) error {
	if level == solSocket && (name == soRcvtimeo || name == soSndtimeo) {
		if n < 16 {
			return syscall.EINVAL
		}

		d, err := p.timeval(at)

		if err != nil {
			return syscall.EDOM
		}

		ms := int64((d + time.Millisecond - 1) / time.Millisecond)

		if name == soRcvtimeo {
			s.recvTimeout.Store(ms)
		} else {
			s.sendTimeout.Store(ms)
		}

		return nil
	}

	if n > 1024 {
		return syscall.EINVAL
	}

	value := slices.Clone(p.span(at, n))
	_, err := sockcall(syscall.SYS_SETSOCKOPT, uintptr(s.fd), uintptr(level), uintptr(name), bufferPointer(value), uintptr(n))

	return err
}

// sockname is the socket's guest address: the path it was bound by, when
// a path.
func (s *hostSocket) sockname(p *Process) ([]byte, error) {
	buf := make([]byte, sockaddrMax)
	length := uint32(sockaddrMax)

	if _, err := sockcall(syscall.SYS_GETSOCKNAME, uintptr(s.fd), bufferPointer(buf), uintptr(unsafe.Pointer(&length))); err != nil {
		return nil, err
	}

	if name := s.unixName.Load(); name != nil {
		return append(unixAddress([]byte(*name)), 0), nil
	}

	return p.guestAddress(buf[:min(length, sockaddrMax)]), nil
}

func (s *hostSocket) peername(p *Process) ([]byte, error) {
	buf := make([]byte, sockaddrMax)
	length := uint32(sockaddrMax)

	if _, err := sockcall(syscall.SYS_GETPEERNAME, uintptr(s.fd), bufferPointer(buf), uintptr(unsafe.Pointer(&length))); err != nil {
		return nil, err
	}

	return p.guestAddress(buf[:min(length, sockaddrMax)]), nil
}

func (s *hostSocket) shutdown(how int) error {
	return syscall.Shutdown(s.fd, how)
}

func (p *Process) libcSocket(domain, kind, protocol uint32) uint32 {
	switch domain {
	case afUnix, afInet, afInet6:
	default:
		return p.result(0, syscall.EAFNOSUPPORT)
	}

	host, err := syscall.Socket(int(domain), int(kind&^(sockNonblock|sockCloexec))|syscall.SOCK_CLOEXEC, int(protocol))

	if err != nil {
		return p.result(0, err)
	}

	return p.result(p.installSocket(host, kind))
}

// socketpair makes a pair of the kernel's for SOCK_SEQPACKET
// (kernel_endpoint.go), of the host's for the other kinds.
func (p *Process) libcSocketpair(domain, kind, protocol, at uint32) uint32 {
	if domain != afUnix {
		return p.result(0, syscall.EOPNOTSUPP)
	}

	if kind&^(sockNonblock|sockCloexec) == sockSeqpacket {
		if protocol != 0 {
			return p.result(0, syscall.EPROTONOSUPPORT)
		}

		a, b := newPair(kind&sockNonblock != 0)
		cloexec := kind&sockCloexec != 0
		x, err := p.install(a, 0, cloexec)

		if err != nil {
			b.unref()

			return p.result(0, err)
		}

		y, err := p.install(b, 0, cloexec)

		if err != nil {
			p.closeFd(x)

			return p.result(0, err)
		}

		p.put32(at, uint32(x))
		p.put32(at+4, uint32(y))

		return p.result(0, nil)
	}

	pair, err := syscall.Socketpair(int(domain), int(kind&^(sockNonblock|sockCloexec))|syscall.SOCK_CLOEXEC, int(protocol))

	if err != nil {
		return p.result(0, err)
	}

	a, err := p.installSocket(pair[0], kind)

	if err != nil {
		syscall.Close(pair[1])

		return p.result(0, err)
	}

	b, err := p.installSocket(pair[1], kind)

	if err != nil {
		p.closeFd(a)

		return p.result(0, err)
	}

	p.put32(at, uint32(a))
	p.put32(at+4, uint32(b))

	return p.result(0, nil)
}

func (p *Process) libcBind(fd, addr, n uint32) uint32 {
	s, err := p.socketAt(fd)

	if err == nil {
		err = s.bind(p, addr, n)
	}

	return p.result(0, err)
}

func (p *Process) libcConnect(fd, addr, n uint32) uint32 {
	s, err := p.socketAt(fd)

	if err == nil {
		err = s.connect(p, addr, n)
	}

	return p.result(0, err)
}

func (p *Process) libcListen(fd, backlog uint32) uint32 {
	s, err := p.socketAt(fd)

	if err == nil {
		err = s.listen(int(int32(backlog)))
	}

	return p.result(0, err)
}

func (p *Process) libcAccept(fd, addr, lenAt uint32) uint32 {
	return p.socketAccept(fd, addr, lenAt, 0)
}

func (p *Process) libcAccept4(fd, addr, lenAt, flags uint32) uint32 {
	return p.socketAccept(fd, addr, lenAt, flags)
}

func (p *Process) libcGetsockname(fd, addr, lenAt uint32) uint32 {
	s, err := p.socketAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	name, err := s.sockname(p)

	if err != nil {
		return p.result(0, err)
	}

	p.putAddress(name, addr, lenAt)

	return p.result(0, nil)
}

func (p *Process) libcGetpeername(fd, addr, lenAt uint32) uint32 {
	s, err := p.socketAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	name, err := s.peername(p)

	if err != nil {
		return p.result(0, err)
	}

	p.putAddress(name, addr, lenAt)

	return p.result(0, nil)
}

func (p *Process) libcGetsockopt(fd, level, name, at, lenAt uint32) uint32 {
	s, err := p.socketAt(fd)

	if err == nil {
		err = s.getsockopt(p, level, name, at, lenAt)
	}

	return p.result(0, err)
}

func (p *Process) libcSetsockopt(fd, level, name, at, n uint32) uint32 {
	s, err := p.socketAt(fd)

	if err == nil {
		err = s.setsockopt(p, level, name, at, n)
	}

	return p.result(0, err)
}

func (p *Process) libcSend(fd, buf, n, flags uint32) uint32 {
	return p.socketSend(fd, buf, n, flags, 0, 0)
}

func (p *Process) libcSendto(fd, buf, n, flags, addr, alen uint32) uint32 {
	return p.socketSend(fd, buf, n, flags, addr, alen)
}

func (p *Process) libcRecv(fd, buf, n, flags uint32) uint32 {
	return p.socketRecv(fd, buf, n, flags, 0, 0)
}

func (p *Process) libcRecvfrom(fd, buf, n, flags, addr, lenAt uint32) uint32 {
	return p.socketRecv(fd, buf, n, flags, addr, lenAt)
}

func (p *Process) libcSendmsg(fd, msg, flags uint32) uint32 {
	s, err := p.socketAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(s.sendmsg(p, msg, flags))
}

func (p *Process) libcRecvmsg(fd, msg, flags uint32) uint32 {
	s, err := p.socketAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(s.recvmsg(p, msg, flags))
}

func (p *Process) libcShutdown(fd, how uint32) uint32 {
	s, err := p.socketAt(fd)

	if err == nil {
		err = s.shutdown(int(how))
	}

	return p.result(0, err)
}

func (p *Process) libcSockatmark(fd uint32) uint32 {
	s, err := p.socketAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	i, ok := s.(Ioctl)

	if !ok {
		return p.result(0, syscall.ENOTTY)
	}

	var mark int32

	if err := i.ioctl(siocatmark, unsafe.Pointer(&mark)); err != nil {
		return p.result(0, err)
	}

	return p.result(int(mark), nil)
}

func (p *Process) socketAccept(fd, addr, lenAt, flags uint32) uint32 {
	s, err := p.socketAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	if flags&^(sockNonblock|sockCloexec) != 0 {
		return p.result(0, syscall.EINVAL)
	}

	conn, peer, err := s.accept(p, flags)

	if err != nil {
		return p.result(0, err)
	}

	guest, err := p.install(conn, 0, flags&sockCloexec != 0)

	if err != nil {
		return p.result(0, err)
	}

	p.putAddress(peer, addr, lenAt)

	return p.result(guest, nil)
}

func (p *Process) socketSend(fd, buf, n, flags, addr, alen uint32) uint32 {
	s, err := p.socketAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(s.sendto(p, buf, n, flags, addr, alen))
}

func (p *Process) socketRecv(fd, buf, n, flags, addr, lenAt uint32) uint32 {
	s, err := p.socketAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	got, from, err := s.recvfrom(p, buf, n, flags)

	if err == nil && addr != 0 {
		p.putAddress(from, addr, lenAt)
	}

	return p.result(got, err)
}
