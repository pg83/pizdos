//go:build linux && amd64

package main

import (
	"encoding/binary"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// File is an open file description: a host descriptor of the kernel's,
// shared by the descriptors and processes that refer to it. A pollable file
// can block: a pipe, a FIFO, a terminal, a socket, another character
// device. One the kernel opened itself is nonblocking on the host and a
// guest's call blocks by polling, unless the guest set O_NONBLOCK; one
// inherited from the host keeps its host flags, and a call polls first.
// Either way a blocking call polls the process's wake descriptor too, so a
// signal interrupts it.
type File struct {
	fd       int
	refs     atomic.Int32
	pollable bool
	owned    bool
	// nonblock is the guest's O_NONBLOCK.
	nonblock atomic.Bool
}

type Descriptor struct {
	file    *File
	cloexec bool
}

const (
	pollIn   = 0x1
	pollOut  = 0x4
	pollErr  = 0x8
	pollHup  = 0x10
	pollNval = 0x20
)

func newFile(fd int, owned bool) *File {
	f := &File{fd: fd, owned: owned}
	f.refs.Store(1)
	var st syscall.Stat_t

	if syscall.Fstat(fd, &st) == nil {
		switch st.Mode & syscall.S_IFMT {
		case syscall.S_IFIFO, syscall.S_IFSOCK, syscall.S_IFCHR:
			f.pollable = true
		}
	}

	fl, _ := fcntl(fd, syscall.F_GETFL, 0)
	f.nonblock.Store(fl&syscall.O_NONBLOCK != 0)

	if owned && f.pollable {
		fcntl(fd, syscall.F_SETFL, fl|syscall.O_NONBLOCK)
	}

	return f
}

func (f *File) ref() *File {
	f.refs.Add(1)

	return f
}

func (f *File) unref() {
	if f.refs.Add(-1) == 0 {
		syscall.Close(f.fd)
	}
}

// fd is the open file of a descriptor.
func (p *Process) fd(fd uint32) (*File, error) {
	if int(fd) >= len(p.files) || p.files[fd] == nil {
		return nil, syscall.EBADF
	}

	return p.files[fd].file, nil
}

// install puts a file at the lowest free descriptor from min; the
// descriptor takes the reference.
func (p *Process) install(f *File, min int, cloexec bool) (int, error) {
	limit := int(min64(p.limits[rlimitNofile].cur, 1<<20))

	for fd := min; fd < limit; fd++ {
		if fd >= len(p.files) {
			p.files = append(p.files, make([]*Descriptor, fd+1-len(p.files))...)
		}

		if p.files[fd] == nil {
			p.files[fd] = &Descriptor{file: f, cloexec: cloexec}

			return fd, nil
		}
	}

	f.unref()

	return -1, syscall.EMFILE
}

func min64(a, b uint64) uint64 {
	return min(a, b)
}

func (p *Process) closeFd(fd int) bool {
	if fd < 0 || fd >= len(p.files) || p.files[fd] == nil {
		return false
	}

	p.files[fd].file.unref()
	p.files[fd] = nil

	return true
}

// wakeSignal reports whether a signal interrupts a blocking call: one the
// process would deliver now. It drains the wake descriptor first, so a
// signal sent after the check makes the next poll return.
func (p *Process) wakeSignal() bool {
	var b [8]byte
	syscall.Read(p.wake, b[:])

	return p.sig.deliverable() != 0
}

// waitFds polls host descriptors and the wake descriptor until one is ready,
// the timeout in milliseconds passes, or a signal interrupts: then the
// error is EINTR, after the handlers ran when all of them restart calls and
// restart is set.
func (p *Process) waitFds(fds []pollFd, timeout int, restart bool) (int, error) {
	all := append(fds, pollFd{fd: int32(p.wake), events: pollIn})

	for {
		if p.wakeSignal() {
			if restart && p.interrupted() {
				continue
			}

			return 0, syscall.EINTR
		}

		for i := range all {
			all[i].revents = 0
		}

		n, err := poll(all, timeout)

		if err == syscall.EINTR {
			continue
		}

		if err != nil {
			return 0, err
		}

		if all[len(all)-1].revents != 0 {
			n--

			if n == 0 {
				continue
			}
		}

		copy(fds, all)

		return n, nil
	}
}

// transfer runs a read or a write of f, blocking as described at File.
func (p *Process) transfer(f *File, events int16, op func() (int, error)) (int, error) {
	for {
		if f.pollable && !f.owned {
			timeout := -1

			if f.nonblock.Load() {
				timeout = 0
			}

			n, err := p.waitFds([]pollFd{{fd: int32(f.fd), events: events}}, timeout, true)

			if err != nil {
				return 0, err
			}

			if n == 0 {
				return 0, syscall.EAGAIN
			}
		}

		n, err := op()

		if err == syscall.EINTR {
			continue
		}

		if err == syscall.EAGAIN && f.pollable && f.owned && !f.nonblock.Load() {
			if _, err := p.waitFds([]pollFd{{fd: int32(f.fd), events: events}}, -1, true); err != nil {
				return 0, err
			}

			continue
		}

		if err == syscall.EPIPE {
			p.sendSelf(int(syscall.SIGPIPE))
		}

		return n, err
	}
}

func (p *Process) readFile(f *File, b []byte) (int, error) {
	return p.transfer(f, pollIn, func() (int, error) {
		return syscall.Read(f.fd, b)
	})
}

func (p *Process) writeFile(f *File, b []byte) (int, error) {
	return p.transfer(f, pollOut, func() (int, error) {
		return syscall.Write(f.fd, b)
	})
}

// iovecs reads a guest iovec array as host iovecs into memory, which stays
// where it is during the call.
func (p *Process) iovecs(iovs uint32, count uint32) ([]syscall.Iovec, error) {
	if count > 1024 {
		return nil, syscall.EINVAL
	}

	table := p.span(iovs, 8*count)
	out := make([]syscall.Iovec, 0, count)

	for i := range count {
		base, n := binary.LittleEndian.Uint32(table[8*i:]), binary.LittleEndian.Uint32(table[8*i+4:])
		b := p.span(base, n)

		if n > 0 {
			out = append(out, syscall.Iovec{Base: &b[0], Len: uint64(n)})
		}
	}

	return out, nil
}

func (p *Process) vectored(fd, iovs, count uint32, events int16, call uintptr) uint32 {
	f, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	v, err := p.iovecs(iovs, count)

	if err != nil || len(v) == 0 {
		return p.result(0, err)
	}

	n, err := p.transfer(f, events, func() (int, error) {
		r, _, e := syscall.Syscall(call, uintptr(f.fd), uintptr(unsafe.Pointer(&v[0])), uintptr(len(v)))

		if e != 0 {
			return 0, e
		}

		return int(r), nil
	})

	return p.result(n, err)
}

func (k *Kernel) fileFunctions() map[string]any {
	return map[string]any{
		"read": func(ctx uintptr, fd, buf, n uint32) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err != nil {
				return p.result(0, err)
			}

			r, err := p.readFile(f, p.span(buf, n))

			return p.result(r, err)
		},
		"write": func(ctx uintptr, fd, buf, n uint32) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err != nil {
				return p.result(0, err)
			}

			r, err := p.writeFile(f, p.span(buf, n))

			return p.result(r, err)
		},
		"readv": func(ctx uintptr, fd, iovs, count uint32) uint32 {
			return proc(ctx).vectored(fd, iovs, count, pollIn, syscall.SYS_READV)
		},
		"writev": func(ctx uintptr, fd, iovs, count uint32) uint32 {
			return proc(ctx).vectored(fd, iovs, count, pollOut, syscall.SYS_WRITEV)
		},
		"lseek": func(ctx uintptr, fd uint32, off uint64, whence uint32) uint64 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err != nil {
				return p.result64(0, err)
			}

			return p.result64(syscall.Seek(f.fd, int64(off), int(whence)))
		},
		"close": func(ctx uintptr, fd uint32) uint32 {
			p := proc(ctx)

			if !p.closeFd(int(fd)) {
				return p.result(0, syscall.EBADF)
			}

			return p.result(0, nil)
		},
		"dup": func(ctx uintptr, fd uint32) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err != nil {
				return p.result(0, err)
			}

			return p.result(p.install(f.ref(), 0, false))
		},
		"dup2": func(ctx uintptr, old, new uint32) uint32 {
			p := proc(ctx)

			if _, err := p.fd(old); err == nil && old == new {
				return p.result(int(new), nil)
			}

			return p.result(p.dupTo(old, new, false))
		},
		"dup3": func(ctx uintptr, old, new, flags uint32) uint32 {
			p := proc(ctx)

			if old == new || flags&^syscall.O_CLOEXEC != 0 {
				return p.result(0, syscall.EINVAL)
			}

			return p.result(p.dupTo(old, new, flags&syscall.O_CLOEXEC != 0))
		},
		"pipe": func(ctx uintptr, fds uint32) uint32 {
			return proc(ctx).pipe(fds, 0)
		},
		"pipe2": func(ctx uintptr, fds, flags uint32) uint32 {
			return proc(ctx).pipe(fds, flags)
		},
		"fcntl":     k.fcntl,
		"ioctl":     k.ioctl,
		"isatty":    k.isatty,
		"tcgetattr": k.tcgetattr,
		"tcsetattr": k.tcsetattr,
		"poll":      k.poll,
		"fsync": func(ctx uintptr, fd uint32) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err == nil {
				err = syscall.Fsync(f.fd)
			}

			return p.result(0, err)
		},
		"ftruncate": func(ctx uintptr, fd uint32, size uint64) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err == nil {
				err = syscall.Ftruncate(f.fd, int64(size))
			}

			return p.result(0, err)
		},
		"fstat": func(ctx uintptr, fd, at uint32) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err != nil {
				return p.result(0, err)
			}

			var st syscall.Stat_t

			if err := syscall.Fstat(f.fd, &st); err != nil {
				return p.result(0, err)
			}

			p.putStat(at, &st)

			return p.result(0, nil)
		},
		"fchmod": func(ctx uintptr, fd, mode uint32) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err == nil {
				err = syscall.Fchmod(f.fd, mode&0o7777)
			}

			return p.result(0, err)
		},
		"fchown": func(ctx uintptr, fd, uid, gid uint32) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err == nil {
				err = syscall.Fchown(f.fd, int(int32(uid)), int(int32(gid)))
			}

			return p.result(0, err)
		},
		"futimens": func(ctx uintptr, fd, times uint32) uint32 {
			p := proc(ctx)
			f, err := p.fd(fd)

			if err == nil {
				err = utimensat(f.fd, nil, p.timespecs(times), 0)
			}

			return p.result(0, err)
		},
	}
}

// dupTo makes new refer to old's file, closing what new referred to.
func (p *Process) dupTo(old, new uint32, cloexec bool) (int, error) {
	f, err := p.fd(old)

	if err != nil {
		return 0, err
	}

	if uint64(new) >= p.limits[rlimitNofile].cur {
		return 0, syscall.EBADF
	}

	if int(new) >= len(p.files) {
		p.files = append(p.files, make([]*Descriptor, int(new)+1-len(p.files))...)
	}

	f.ref()
	p.closeFd(int(new))
	p.files[new] = &Descriptor{file: f, cloexec: cloexec}

	return int(new), nil
}

func (p *Process) pipe(at, flags uint32) uint32 {
	if flags&^(syscall.O_CLOEXEC|syscall.O_NONBLOCK) != 0 {
		return p.result(0, syscall.EINVAL)
	}

	var ends [2]int

	if err := syscall.Pipe2(ends[:], syscall.O_CLOEXEC); err != nil {
		return p.result(0, err)
	}

	cloexec := flags&syscall.O_CLOEXEC != 0
	var fds [2]int

	for i, host := range ends {
		f := newFile(host, true)
		f.nonblock.Store(flags&syscall.O_NONBLOCK != 0)
		fd, err := p.install(f, 0, cloexec)

		if err != nil {
			if i == 1 {
				p.closeFd(fds[0])
			} else {
				syscall.Close(ends[1])
			}

			return p.result(0, err)
		}

		fds[i] = fd
	}

	p.put32(at, uint32(fds[0]))
	p.put32(at+4, uint32(fds[1]))

	return p.result(0, nil)
}

// varArg is the i-th int of a variadic call's argument area.
func (p *Process) varArg(va uint32, i uint32) uint32 {
	return p.u32(va + 4*i)
}

const (
	fDupfd        = 0
	fGetfd        = 1
	fSetfd        = 2
	fGetfl        = 3
	fSetfl        = 4
	fDupfdCloexec = 1030
)

func (k *Kernel) fcntl(ctx uintptr, fd, cmd, va uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	switch cmd {
	case fDupfd, fDupfdCloexec:
		min := p.varArg(va, 0)

		if uint64(min) >= p.limits[rlimitNofile].cur {
			return p.result(0, syscall.EINVAL)
		}

		return p.result(p.install(f.ref(), int(min), cmd == fDupfdCloexec))
	case fGetfd:
		if p.files[fd].cloexec {
			return p.result(1, nil)
		}

		return p.result(0, nil)
	case fSetfd:
		p.files[fd].cloexec = p.varArg(va, 0)&1 != 0

		return p.result(0, nil)
	case fGetfl:
		fl, err := fcntl(f.fd, syscall.F_GETFL, 0)

		if err != nil {
			return p.result(0, err)
		}

		fl &^= syscall.O_NONBLOCK

		if f.nonblock.Load() {
			fl |= syscall.O_NONBLOCK
		}

		return p.result(fl, nil)
	case fSetfl:
		arg := int(p.varArg(va, 0))
		fl, err := fcntl(f.fd, syscall.F_GETFL, 0)

		if err != nil {
			return p.result(0, err)
		}

		fl = fl&^(syscall.O_APPEND|syscall.O_DIRECT) | arg&(syscall.O_APPEND|syscall.O_DIRECT)

		if !f.owned {
			fl = fl&^syscall.O_NONBLOCK | arg&syscall.O_NONBLOCK
		}

		if _, err := fcntl(f.fd, syscall.F_SETFL, fl); err != nil {
			return p.result(0, err)
		}

		f.nonblock.Store(arg&syscall.O_NONBLOCK != 0)

		return p.result(0, nil)
	}

	return p.result(0, syscall.EINVAL)
}

const (
	tcgets     = 0x5401
	tcsets     = 0x5402
	tiocgpgrp  = 0x540f
	tiocspgrp  = 0x5410
	tiocgwinsz = 0x5413
	tiocswinsz = 0x5414
	fionread   = 0x541b
	fionbio    = 0x5421
	fionclex   = 0x5450
	fioclex    = 0x5451
)

func ioctlPtr(fd int, req uintptr, arg unsafe.Pointer) error {
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), req, uintptr(arg)); e != 0 {
		return e
	}

	return nil
}

func (k *Kernel) ioctl(ctx uintptr, fd, req, va uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	switch req {
	case tiocgwinsz, tiocswinsz:
		var ws [8]byte
		at := p.varArg(va, 0)

		if req == tiocswinsz {
			copy(ws[:], p.span(at, 8))
		}

		if err := ioctlPtr(f.fd, uintptr(req), unsafe.Pointer(&ws)); err != nil {
			return p.result(0, err)
		}

		if req == tiocgwinsz {
			copy(p.span(at, 8), ws[:])
		}

		return p.result(0, nil)
	case fionread:
		var n int32

		if err := ioctlPtr(f.fd, fionread, unsafe.Pointer(&n)); err != nil {
			return p.result(0, err)
		}

		p.put32(p.varArg(va, 0), uint32(n))

		return p.result(0, nil)
	case tcgets:
		return k.tcgetattr(ctx, fd, p.varArg(va, 0))
	case tcsets, tcsets + 1, tcsets + 2:
		return k.tcsetattr(ctx, fd, req-tcsets, p.varArg(va, 0))
	case tiocgpgrp:
		r := k.tcgetpgrp(ctx, fd)

		if int32(r) >= 0 {
			p.put32(p.varArg(va, 0), r)
			r = 0
		}

		return r
	case tiocspgrp:
		return k.tcsetpgrp(ctx, fd, p.u32(p.varArg(va, 0)))
	case fionbio:
		f.nonblock.Store(p.u32(p.varArg(va, 0)) != 0)

		return p.result(0, nil)
	case fioclex, fionclex:
		p.files[fd].cloexec = req == fioclex

		return p.result(0, nil)
	}

	return p.result(0, syscall.ENOTTY)
}

// hostTermios is Linux's struct termios of TCGETS: the guest's has 32
// control characters and the speeds after them.
type hostTermios struct {
	iflag, oflag, cflag, lflag uint32
	line                       byte
	cc                         [19]byte
}

func (k *Kernel) isatty(ctx uintptr, fd uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err != nil {
		p.setErrno(syscall.EBADF)

		return 0
	}

	var t hostTermios

	if err := ioctlPtr(f.fd, tcgets, unsafe.Pointer(&t)); err != nil {
		p.setErrno(syscall.ENOTTY)

		return 0
	}

	return 1
}

func (k *Kernel) tcgetattr(ctx uintptr, fd, at uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	var t hostTermios

	if err := ioctlPtr(f.fd, tcgets, unsafe.Pointer(&t)); err != nil {
		return p.result(0, err)
	}

	b := p.span(at, 60)
	clear(b)
	le := binary.LittleEndian
	le.PutUint32(b, t.iflag)
	le.PutUint32(b[4:], t.oflag)
	le.PutUint32(b[8:], t.cflag)
	le.PutUint32(b[12:], t.lflag)
	b[16] = t.line
	copy(b[17:], t.cc[:])
	speed := t.cflag & 0o10017
	le.PutUint32(b[52:], speed)
	le.PutUint32(b[56:], speed)

	return p.result(0, nil)
}

func (k *Kernel) tcsetattr(ctx uintptr, fd, action, at uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	if action > 2 {
		return p.result(0, syscall.EINVAL)
	}

	b := p.span(at, 60)
	le := binary.LittleEndian
	t := hostTermios{iflag: le.Uint32(b), oflag: le.Uint32(b[4:]), cflag: le.Uint32(b[8:]), lflag: le.Uint32(b[12:]), line: b[16]}
	copy(t.cc[:], b[17:])

	return p.result(0, ioctlPtr(f.fd, uintptr(tcsets+action), unsafe.Pointer(&t)))
}

type pollFd struct {
	fd      int32
	events  int16
	revents int16
}

func poll(fds []pollFd, timeout int) (int, error) {
	r, _, e := syscall.Syscall(syscall.SYS_POLL, uintptr(unsafe.Pointer(&fds[0])), uintptr(len(fds)), uintptr(timeout))

	if e != 0 {
		return 0, e
	}

	return int(r), nil
}

func (k *Kernel) poll(ctx uintptr, at, n, timeout uint32) uint32 {
	p := proc(ctx)

	if n > 1<<16 {
		return p.result(0, syscall.EINVAL)
	}

	guest := p.span(at, 8*n)
	host := make([]pollFd, 0, n)
	slot := make([]int, n)
	invalid := 0

	for i := range n {
		e := guest[8*i:]
		fd := int32(binary.LittleEndian.Uint32(e))
		binary.LittleEndian.PutUint16(e[6:], 0)
		slot[i] = -1

		if fd < 0 {
			continue
		}

		f, err := p.fd(uint32(fd))

		if err != nil {
			binary.LittleEndian.PutUint16(e[6:], pollNval)
			invalid++

			continue
		}

		slot[i] = len(host)
		host = append(host, pollFd{fd: int32(f.fd), events: int16(binary.LittleEndian.Uint16(e[4:]))})
	}

	wait := int(int32(timeout))

	if invalid > 0 {
		wait = 0
	}

	ready, err := p.waitFds(host, wait, false)

	if err != nil {
		return p.result(0, err)
	}

	if ready > 0 {
		for i := range n {
			if slot[i] >= 0 {
				binary.LittleEndian.PutUint16(guest[8*i+6:], uint16(host[slot[i]].revents))
			}
		}
	}

	return p.result(ready+invalid, nil)
}

// putStat writes a guest struct stat.
func (p *Process) putStat(at uint32, st *syscall.Stat_t) {
	b := p.span(at, 112)
	clear(b)
	le := binary.LittleEndian
	le.PutUint64(b, st.Dev)
	le.PutUint64(b[8:], st.Ino)
	le.PutUint32(b[16:], st.Mode)
	le.PutUint32(b[20:], uint32(st.Nlink))
	le.PutUint32(b[24:], st.Uid)
	le.PutUint32(b[28:], st.Gid)
	le.PutUint64(b[32:], st.Rdev)
	le.PutUint64(b[40:], uint64(st.Size))
	le.PutUint32(b[48:], uint32(st.Blksize))
	le.PutUint64(b[56:], uint64(st.Blocks))

	for i, t := range []syscall.Timespec{st.Atim, st.Mtim, st.Ctim} {
		le.PutUint64(b[64+16*i:], uint64(t.Sec))
		le.PutUint32(b[72+16*i:], uint32(t.Nsec))
	}
}

// timespecs reads a guest struct timespec[2], or nil for a null pointer.
func (p *Process) timespecs(at uint32) []syscall.Timespec {
	if at == 0 {
		return nil
	}

	b := p.span(at, 32)
	le := binary.LittleEndian

	return []syscall.Timespec{
		{Sec: int64(le.Uint64(b)), Nsec: int64(int32(le.Uint32(b[8:])))},
		{Sec: int64(le.Uint64(b[16:])), Nsec: int64(int32(le.Uint32(b[24:])))},
	}
}

// utimensat sets the times of path in dirfd, or of dirfd for a nil path.
func utimensat(dirfd int, name *byte, ts []syscall.Timespec, flags int) error {
	var times uintptr

	if ts != nil {
		times = uintptr(unsafe.Pointer(&ts[0]))
	}

	if _, _, e := syscall.Syscall6(syscall.SYS_UTIMENSAT, uintptr(dirfd), uintptr(unsafe.Pointer(name)), times, uintptr(flags), 0, 0); e != 0 {
		return e
	}

	return nil
}

func eventfd() (int, error) {
	r, _, e := syscall.RawSyscall(syscall.SYS_EVENTFD2, 0, syscall.O_CLOEXEC|syscall.O_NONBLOCK, 0)

	if e != 0 {
		return 0, e
	}

	return int(r), nil
}

// dupHigh duplicates a host descriptor for the kernel.
func dupHigh(fd int) (int, error) {
	return fcntl(fd, syscall.F_DUPFD_CLOEXEC, 3)
}

// raiseFileLimit lets the kernel hold as many host descriptors as it may:
// every open file of every process is one.
func raiseFileLimit() {
	var l syscall.Rlimit

	if syscall.Getrlimit(syscall.RLIMIT_NOFILE, &l) == nil && l.Cur < l.Max {
		l.Cur = l.Max
		syscall.Setrlimit(syscall.RLIMIT_NOFILE, &l)
	}
}
