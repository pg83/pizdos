//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"slices"
	"syscall"
	"time"
	"unsafe"
)

// Sockets are the host's: a socket a process makes is a host socket of the
// kernel's, nonblocking on the host as every pollable file it owns
// (kernel_file.go). The address structures of lib/c are Linux's, so most
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
	scmRights      = 1
	sunPathMax     = 108
	sockaddrMax    = 128
	siocatmark     = 0x8905
	sysAccept4     = 288
)

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

	if bind {
		return p.onName(guest, syscall.EADDRINUSE, func(dirfd int, name string) error {
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

	return p.onPath(guest, true, func(fd int) error {
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
	f := newFile(host, true)
	f.nonblock.Store(flags&sockNonblock != 0)

	return p.install(f, 0, flags&sockCloexec != 0)
}

func (p *Process) socketFile(fd uint32) (*File, error) {
	f, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	var st syscall.Stat_t

	if syscall.Fstat(f.fd, &st) == nil && st.Mode&syscall.S_IFMT != syscall.S_IFSOCK {
		return nil, syscall.ENOTSOCK
	}

	return f, nil
}

// connect connects as a blocking socket does, unless the guest asked for a
// nonblocking one: it waits for the host's connection and its error.
func (p *Process) connect(f *File, addr []byte) error {
	timeout := f.timeout(pollOut)
	var deadline time.Time

	if timeout >= 0 {
		deadline = time.Now().Add(time.Duration(timeout) * time.Millisecond)
	}

	for {
		_, err := sockcall(syscall.SYS_CONNECT, uintptr(f.fd), bufferPointer(addr), uintptr(len(addr)))

		if err == syscall.EINTR {
			continue
		}

		if f.nonblock.Load() || err != syscall.EINPROGRESS && err != syscall.EAGAIN {
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

			if _, werr := p.waitFds(nil, wait, timeout < 0); werr != nil {
				return werr
			}

			continue
		}

		ready, werr := p.waitFds([]pollFd{{fd: int32(f.fd), events: pollOut}}, timeout, timeout < 0)

		if werr != nil {
			return werr
		}

		if ready == 0 {
			return syscall.ETIMEDOUT
		}

		code, gerr := syscall.GetsockoptInt(f.fd, syscall.SOL_SOCKET, syscall.SO_ERROR)

		if gerr != nil {
			return gerr
		}

		if code != 0 {
			return syscall.Errno(code)
		}

		return nil
	}
}

// recvfrom receives into a guest buffer, with MSG_WAITALL until it is full
// or the stream ends.
func (p *Process) recvfrom(f *File, buf, n, flags uint32) (int, []byte, error) {
	mode := ioMode{nonblock: flags&msgDontwait != 0}
	addr := make([]byte, sockaddrMax)
	total := 0

	for {
		var length uint32
		got, err := p.transferMode(f, pollIn, mode, func() (int, error) {
			length = sockaddrMax

			return sockcall(syscall.SYS_RECVFROM, uintptr(f.fd), bufferPointer(p.span(buf+uint32(total), n-uint32(total))),
				uintptr(n-uint32(total)), uintptr(flags&^msgWaitall), bufferPointer(addr), uintptr(unsafe.Pointer(&length)))
		})

		if err != nil {
			if total > 0 {
				return total, addr[:0], nil
			}

			return 0, nil, err
		}

		total += got

		if flags&msgWaitall == 0 || mode.nonblock || got == 0 || uint32(total) >= n {
			return total, addr[:min(length, sockaddrMax)], nil
		}
	}
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
// of SCM_RIGHTS become the host's.
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
				f, err := p.fd(binary.LittleEndian.Uint32(data[i:]))

				if err != nil {
					return nil, err
				}

				fds = binary.LittleEndian.AppendUint32(fds, uint32(f.fd))
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

// guestControl writes the host's control messages to a guest's buffer:
// received descriptors become the process's, those that do not fit are
// closed and MSG_CTRUNC set. It returns the length written and the flag.
func (p *Process) guestControl(host []byte, at, room uint32, cloexec bool) (uint32, uint32) {
	off, flags := uint32(0), uint32(0)

	for len(host) >= hostCmsgHeader {
		length := int(binary.LittleEndian.Uint64(host))

		if length < hostCmsgHeader || length > len(host) {
			break
		}

		level, kind := binary.LittleEndian.Uint32(host[8:]), binary.LittleEndian.Uint32(host[12:])
		data := host[hostCmsgHeader:length]
		host = host[min(hostAlign(length), len(host)):]

		if level == solSocket && kind == scmRights {
			var fds []byte

			for i := 0; i+4 <= len(data); i += 4 {
				fd := int(binary.LittleEndian.Uint32(data[i:]))

				if off+guestCmsgHeader+uint32(len(fds))+4 > room {
					syscall.Close(fd)
					flags |= msgCtrunc

					continue
				}

				guest, err := p.install(newFile(fd, true), 0, cloexec)

				if err != nil {
					flags |= msgCtrunc

					continue
				}

				fds = binary.LittleEndian.AppendUint32(fds, uint32(guest))
			}

			if len(fds) == 0 {
				continue
			}

			data = fds
		}

		size := guestCmsgHeader + uint32(len(data))

		if off+size > room {
			flags |= msgCtrunc

			break
		}

		p.put32(at+off, size)
		p.put32(at+off+4, level)
		p.put32(at+off+8, kind)
		copy(p.span(at+off+guestCmsgHeader, uint32(len(data))), data)
		off += min(guestAlign(size), room-off)
	}

	return off, flags
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

func (p *Process) sendmsg(f *File, at, flags uint32) (int, error) {
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

		return p.transferMode(f, pollOut, mode, func() (int, error) {
			m := hostMsghdr{name: bufferPointer(addr), namelen: uint32(len(addr)), control: bufferPointer(control), controllen: uint64(len(control))}

			if len(iov) > 0 {
				m.iov, m.iovlen = uintptr(unsafe.Pointer(&iov[0])), uint64(len(iov))
			}

			return sockcall(syscall.SYS_SENDMSG, uintptr(f.fd), uintptr(unsafe.Pointer(&m)), uintptr(flags|msgNosignal))
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

func (p *Process) recvmsg(f *File, at, flags uint32) (int, error) {
	iov, err := p.iovecs(p.u32(at+8), p.u32(at+12))

	if err != nil {
		return 0, err
	}

	room := p.u32(at + 20)
	control := make([]byte, 2*room+64)
	addr := make([]byte, sockaddrMax)
	var m hostMsghdr
	n, err := p.transferMode(f, pollIn, ioMode{nonblock: flags&msgDontwait != 0}, func() (int, error) {
		m = hostMsghdr{name: bufferPointer(addr), namelen: sockaddrMax, control: bufferPointer(control), controllen: uint64(len(control))}

		if len(iov) > 0 {
			m.iov, m.iovlen = uintptr(unsafe.Pointer(&iov[0])), uint64(len(iov))
		}

		return sockcall(syscall.SYS_RECVMSG, uintptr(f.fd), uintptr(unsafe.Pointer(&m)), uintptr(flags|msgCmsgCloexec))
	})

	if err != nil {
		return 0, err
	}

	if name := p.u32(at); name != 0 {
		guest := p.guestAddress(addr[:min(m.namelen, sockaddrMax)])
		copy(p.span(name, min(p.u32(at+4), uint32(len(guest)))), guest)
		p.put32(at+4, uint32(len(guest)))
	}

	length, truncated := p.guestControl(control[:m.controllen], p.u32(at+16), room, flags&msgCmsgCloexec != 0)
	p.put32(at+20, length)
	p.put32(at+24, uint32(m.flags)&^msgCmsgCloexec|flags&msgCmsgCloexec|truncated)

	return n, nil
}

// sockopt is a socket's option: the timeouts the kernel keeps, the host's
// others.
func (p *Process) getsockopt(f *File, level, name, at, lenAt uint32) error {
	if level == solSocket && (name == soRcvtimeo || name == soSndtimeo) {
		ms := f.recvTimeout.Load()

		if name == soSndtimeo {
			ms = f.sendTimeout.Load()
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

	if _, err := sockcall(syscall.SYS_GETSOCKOPT, uintptr(f.fd), uintptr(level), uintptr(name), bufferPointer(buf), uintptr(unsafe.Pointer(&length))); err != nil {
		return err
	}

	copy(p.span(at, min(length, uint32(len(buf)))), buf)
	p.put32(lenAt, length)

	return nil
}

func (p *Process) setsockopt(f *File, level, name, at, n uint32) error {
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
			f.recvTimeout.Store(ms)
		} else {
			f.sendTimeout.Store(ms)
		}

		return nil
	}

	if n > 1024 {
		return syscall.EINVAL
	}

	value := slices.Clone(p.span(at, n))
	_, err := sockcall(syscall.SYS_SETSOCKOPT, uintptr(f.fd), uintptr(level), uintptr(name), bufferPointer(value), uintptr(n))

	return err
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

func (p *Process) libcSocketpair(domain, kind, protocol, at uint32) uint32 {
	if domain != afUnix {
		return p.result(0, syscall.EOPNOTSUPP)
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
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	err = p.withAddress(addr, n, true, func(a []byte) error {
		_, err := sockcall(syscall.SYS_BIND, uintptr(f.fd), bufferPointer(a), uintptr(len(a)))

		return err
	})

	if err == nil && n > 2 && p.u16(addr) == afUnix && p.u8(addr+2) != 0 {
		name := p.str(addr + 2)
		f.unixName.Store(&name)
	}

	return p.result(0, err)
}

func (p *Process) libcConnect(fd, addr, n uint32) uint32 {
	f, err := p.socketFile(fd)

	if err == nil {
		err = p.withAddress(addr, n, false, func(a []byte) error { return p.connect(f, a) })
	}

	return p.result(0, err)
}

func (p *Process) libcListen(fd, backlog uint32) uint32 {
	f, err := p.socketFile(fd)

	if err == nil {
		err = syscall.Listen(f.fd, int(int32(backlog)))
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
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	buf := make([]byte, sockaddrMax)
	length := uint32(sockaddrMax)

	if _, err := sockcall(syscall.SYS_GETSOCKNAME, uintptr(f.fd), bufferPointer(buf), uintptr(unsafe.Pointer(&length))); err != nil {
		return p.result(0, err)
	}

	guest := p.guestAddress(buf[:min(length, sockaddrMax)])

	if name := f.unixName.Load(); name != nil {
		guest = append(unixAddress([]byte(*name)), 0)
	}

	p.putAddress(guest, addr, lenAt)

	return p.result(0, nil)
}

func (p *Process) libcGetpeername(fd, addr, lenAt uint32) uint32 {
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	buf := make([]byte, sockaddrMax)
	length := uint32(sockaddrMax)

	if _, err := sockcall(syscall.SYS_GETPEERNAME, uintptr(f.fd), bufferPointer(buf), uintptr(unsafe.Pointer(&length))); err != nil {
		return p.result(0, err)
	}

	p.putAddress(p.guestAddress(buf[:min(length, sockaddrMax)]), addr, lenAt)

	return p.result(0, nil)
}

func (p *Process) libcGetsockopt(fd, level, name, at, lenAt uint32) uint32 {
	f, err := p.socketFile(fd)

	if err == nil {
		err = p.getsockopt(f, level, name, at, lenAt)
	}

	return p.result(0, err)
}

func (p *Process) libcSetsockopt(fd, level, name, at, n uint32) uint32 {
	f, err := p.socketFile(fd)

	if err == nil {
		err = p.setsockopt(f, level, name, at, n)
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
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(p.sendmsg(f, msg, flags))
}

func (p *Process) libcRecvmsg(fd, msg, flags uint32) uint32 {
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(p.recvmsg(f, msg, flags))
}

func (p *Process) libcShutdown(fd, how uint32) uint32 {
	f, err := p.socketFile(fd)

	if err == nil {
		err = syscall.Shutdown(f.fd, int(how))
	}

	return p.result(0, err)
}

func (p *Process) libcSockatmark(fd uint32) uint32 {
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	var mark int32

	if err := ioctlPtr(f.fd, siocatmark, unsafe.Pointer(&mark)); err != nil {
		return p.result(0, err)
	}

	return p.result(int(mark), nil)
}

func (p *Process) socketAccept(fd, addr, lenAt, flags uint32) uint32 {
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	if flags&^(sockNonblock|sockCloexec) != 0 {
		return p.result(0, syscall.EINVAL)
	}

	buf := make([]byte, sockaddrMax)
	var length uint32
	host, err := p.transfer(f, pollIn, func() (int, error) {
		length = sockaddrMax

		return sockcall(sysAccept4, uintptr(f.fd), bufferPointer(buf), uintptr(unsafe.Pointer(&length)), syscall.SOCK_CLOEXEC)
	})

	if err != nil {
		return p.result(0, err)
	}

	guest, err := p.installSocket(host, flags)

	if err != nil {
		return p.result(0, err)
	}

	p.putAddress(p.guestAddress(buf[:min(length, sockaddrMax)]), addr, lenAt)

	return p.result(guest, nil)
}

func (p *Process) socketSend(fd, buf, n, flags, addr, alen uint32) uint32 {
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	mode := ioMode{nonblock: flags&msgDontwait != 0, quiet: flags&msgNosignal != 0}
	send := func(to []byte) (int, error) {
		return p.transferMode(f, pollOut, mode, func() (int, error) {
			return sockcall(syscall.SYS_SENDTO, uintptr(f.fd), bufferPointer(p.span(buf, n)), uintptr(n), uintptr(flags|msgNosignal),
				bufferPointer(to), uintptr(len(to)))
		})
	}

	if addr == 0 {
		return p.result(send(nil))
	}

	sent := 0
	err = p.withAddress(addr, alen, false, func(to []byte) error {
		var e error
		sent, e = send(to)

		return e
	})

	return p.result(sent, err)
}

func (p *Process) socketRecv(fd, buf, n, flags, addr, lenAt uint32) uint32 {
	f, err := p.socketFile(fd)

	if err != nil {
		return p.result(0, err)
	}

	got, from, err := p.recvfrom(f, buf, n, flags)

	if err == nil && addr != 0 {
		p.putAddress(p.guestAddress(from), addr, lenAt)
	}

	return p.result(got, err)
}
