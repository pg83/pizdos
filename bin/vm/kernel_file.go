//go:build linux && amd64

package main

import (
	"encoding/binary"
	"syscall"
	"time"
	"unsafe"
)

// The calls on a descriptor: each finds the role its object plays
// (kernel_object.go) and leaves the rest to it.

// ioSpans reads a guest iovec array as spans of memory, which stays where
// it is during the call.
func (p *Process) ioSpans(iovs uint32, count uint32) ([][]byte, error) {
	if count > 1024 {
		return nil, syscall.EINVAL
	}

	table := p.span(iovs, 8*count)
	out := make([][]byte, 0, count)

	for i := range count {
		base, n := binary.LittleEndian.Uint32(table[8*i:]), binary.LittleEndian.Uint32(table[8*i+4:])
		b := p.span(base, n)

		if n > 0 {
			out = append(out, b)
		}
	}

	return out, nil
}

// iovecs reads a guest iovec array as host iovecs into memory.
func (p *Process) iovecs(iovs uint32, count uint32) ([]syscall.Iovec, error) {
	spans, err := p.ioSpans(iovs, count)

	if err != nil {
		return nil, err
	}

	out := make([]syscall.Iovec, 0, len(spans))

	for _, b := range spans {
		out = append(out, syscall.Iovec{Base: &b[0], Len: uint64(len(b))})
	}

	return out, nil
}

// vectored runs readv or writev, or with an offset preadv or pwritev: the
// host's on a host object, else one transfer of the gathered bytes, which
// a read scatters.
func (p *Process) vectored(fd, iovs, count uint32, events int16, call uintptr, off uint64) uint32 {
	t, err := p.transferAt(fd, events == pollIn)

	if err != nil {
		return p.result(0, err)
	}

	spans, err := p.ioSpans(iovs, count)

	if err != nil || len(spans) == 0 {
		return p.result(0, err)
	}

	if v, ok := t.(Vectored); ok {
		iov := make([]syscall.Iovec, 0, len(spans))

		for _, b := range spans {
			iov = append(iov, syscall.Iovec{Base: &b[0], Len: uint64(len(b))})
		}

		return p.result(v.vectored(p, call, iov, off, events))
	}

	total := 0

	for _, b := range spans {
		total += len(b)
	}

	buf := make([]byte, 0, total)
	positioned := call == syscall.SYS_PREADV || call == syscall.SYS_PWRITEV

	if events == pollOut {
		for _, b := range spans {
			buf = append(buf, b...)
		}

		if positioned {
			return p.result(t.pwrite(p, buf, int64(off)))
		}

		return p.result(t.write(p, buf, ioMode{}))
	}

	buf = buf[:total]
	var n int

	if positioned {
		n, err = t.pread(p, buf, int64(off))
	} else {
		n, err = t.read(p, buf, ioMode{})
	}

	at := 0

	for _, b := range spans {
		if at >= n {
			break
		}

		at += copy(b, buf[at:n])
	}

	return p.result(n, err)
}

// waitLock takes a lock with try, which does not block: when it would and
// block is set, it tries again until a signal interrupts.
func (p *Process) waitLock(block bool, try func() error) error {
	for {
		err := try()

		if !block || err != syscall.EAGAIN && err != syscall.EACCES {
			return err
		}

		if _, err := p.waitFds(nil, 10, true); err != nil {
			return err
		}
	}
}

const (
	fGetlk    = 5
	fSetlk    = 6
	fSetlkw   = 7
	fOfdGetlk = 36
	fOfdSetlk = 37
	fRdlck    = 0
	fWrlck    = 1
	fUnlck    = 2
)

// recordLock runs F_GETLK, F_SETLK or F_SETLKW with a guest struct flock.
func (p *Process) recordLock(l Locker, cmd, at uint32) error {
	lk := syscall.Flock_t{Type: int16(p.u16(at)), Whence: int16(p.u16(at + 2)), Start: int64(p.u64(at + 8)), Len: int64(p.u64(at + 16))}

	if cmd == fGetlk {
		if err := l.recordLock(fOfdGetlk, &lk); err != nil {
			return err
		}

		p.put16(at, uint16(lk.Type))
		p.put16(at+2, uint16(lk.Whence))
		p.put64(at+8, uint64(lk.Start))
		p.put64(at+16, uint64(lk.Len))
		p.put32(at+24, uint32(lk.Pid))

		return nil
	}

	return p.waitLock(cmd == fSetlkw, func() error {
		return l.recordLock(fOfdSetlk, &lk)
	})
}

// fdSet is a guest fd_set: 1024 bits in words of 32.
type fdSet struct {
	at uint32
}

func (s fdSet) has(p *Process, fd int) bool {
	return s.at != 0 && p.u32(s.at+uint32(fd/32*4))&(1<<(fd%32)) != 0
}

func (s fdSet) set(p *Process, fd int) {
	p.put32(s.at+uint32(fd/32*4), p.u32(s.at+uint32(fd/32*4))|1<<(fd%32))
}

const (
	sysSyncfs        = 306
	sysCopyFileRange = 326
	fdSetsize        = 1024
	pollInSet        = pollIn | pollHup | pollErr
	pollOutSet       = pollOut | pollErr
)

// selectFds waits as select does, with a timeout in milliseconds, and
// returns how many bits of the sets it left set.
func (p *Process) selectFds(n uint32, sets [3]fdSet, timeout int) (int, error) {
	if n > fdSetsize {
		return 0, syscall.EINVAL
	}

	var entries []pollEntry
	var fds []int

	for fd := range int(n) {
		events := int16(0)

		for i, ev := range []int16{pollIn, pollOut, pollPri} {
			if sets[i].has(p, fd) {
				events |= ev
			}
		}

		if events == 0 {
			continue
		}

		obj, err := p.fd(uint32(fd))

		if err != nil {
			return 0, err
		}

		entries = append(entries, pollEntry{obj: obj, events: events})
		fds = append(fds, fd)
	}

	ready, err := p.waitObjects(entries, timeout, false)

	if err != nil {
		return 0, err
	}

	for _, s := range sets {
		if s.at != 0 {
			clear(p.span(s.at, (n+31)/32*4))
		}
	}

	count := 0

	for i, e := range entries {
		if ready == 0 {
			break
		}

		// Each descriptor was validated above and remains in this process's
		// table throughout the non-restarting wait. A signal returns EINTR
		// before a handler could close one and let the wait poll it again.
		for k, mask := range []int16{pollInSet, pollOutSet, pollPri} {
			if e.events&[]int16{pollIn, pollOut, pollPri}[k] != 0 && e.revents&mask != 0 {
				sets[k].set(p, fds[i])
				count++
			}
		}
	}

	return count, nil
}

// withMask runs a wait with the signal mask of pselect or ppoll, when
// mask is not 0: a signal the wait took runs its handler before the old
// mask returns.
func (p *Process) withMask(mask uint32, wait func() (int, error)) (int, error) {
	if mask == 0 {
		return wait()
	}

	old := p.setMask(p.u64(mask))
	n, err := wait()

	if err == syscall.EINTR {
		p.handleSignals()
	}

	p.setMask(old)

	return n, err
}

func (p *Process) libcRead(fd, buf, n uint32) uint32 {
	t, err := p.transferAt(fd, true)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(t.read(p, p.span(buf, n), ioMode{}))
}

func (p *Process) libcWrite(fd, buf, n uint32) uint32 {
	t, err := p.transferAt(fd, false)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(t.write(p, p.span(buf, n), ioMode{}))
}

func (p *Process) libcPread(fd, buf, n uint32, off uint64) uint32 {
	t, err := p.transferAt(fd, true)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(t.pread(p, p.span(buf, n), int64(off)))
}

func (p *Process) libcPwrite(fd, buf, n uint32, off uint64) uint32 {
	t, err := p.transferAt(fd, false)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(t.pwrite(p, p.span(buf, n), int64(off)))
}

// copy_file_range is the host's between the host descriptors; an offset
// given is read from the guest's memory and written back. An object of
// the kernel is not a file of the host: EINVAL, as a pipe is to Linux.
func (p *Process) libcCopy_file_range(in, offIn, out, offOut, n, flags uint32) uint32 {
	src, err := p.fd(in)

	if err != nil {
		return p.result(0, err)
	}

	dst, err := p.fd(out)

	if err != nil {
		return p.result(0, err)
	}

	if src.host() < 0 || dst.host() < 0 {
		return p.result(0, syscall.EINVAL)
	}

	var from, to *int64

	if offIn != 0 {
		from = new(int64)
		*from = int64(p.u64(offIn))
	}

	if offOut != 0 {
		to = new(int64)
		*to = int64(p.u64(offOut))
	}

	r, _, e := syscall.Syscall6(sysCopyFileRange, uintptr(src.host()), uintptr(unsafe.Pointer(from)), uintptr(dst.host()),
		uintptr(unsafe.Pointer(to)), uintptr(n), uintptr(flags))

	if e != 0 {
		return p.result(0, e)
	}

	if from != nil {
		p.put64(offIn, uint64(*from))
	}

	if to != nil {
		p.put64(offOut, uint64(*to))
	}

	return p.result(int(r), nil)
}

func (p *Process) libcFdatasync(fd uint32) uint32 {
	n, err := p.nodeAt(fd)

	if err == nil {
		err = n.sync(true)
	}

	return p.result(0, err)
}

func (p *Process) libcSync() {
	syscall.Sync()
}

// syncfs syncs the host file system of a host object; an object of the
// kernel has nothing to sync.
func (p *Process) libcSyncfs(fd uint32) uint32 {
	obj, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	if host := obj.host(); host >= 0 {
		if _, _, e := syscall.Syscall(sysSyncfs, uintptr(host), 0, 0); e != 0 {
			return p.result(0, e)
		}
	}

	return p.result(0, nil)
}

func (p *Process) libcFlock(fd, op uint32) uint32 {
	l, err := p.lockerAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(0, p.waitLock(op&syscall.LOCK_NB == 0, func() error {
		return l.flock(int(op))
	}))
}

// lockf locks from the file's offset, as fcntl's write locks.
func (p *Process) libcLockf(fd, cmd uint32, n uint64) uint32 {
	l, err := p.lockerAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	lk := syscall.Flock_t{Type: fWrlck, Whence: 1, Len: int64(n)}

	switch cmd {
	case 0:
		lk.Type = fUnlck
		err = l.recordLock(fOfdSetlk, &lk)
	case 1, 2:
		err = p.waitLock(cmd == 1, func() error {
			return l.recordLock(fOfdSetlk, &lk)
		})
	case 3:
		lk.Type = fRdlck

		if err = l.recordLock(fOfdGetlk, &lk); err == nil && lk.Type != fUnlck {
			err = syscall.EACCES
		}
	default:
		err = syscall.EINVAL
	}

	return p.result(0, err)
}

// posix_fadvise and posix_fallocate return an error number.
func (p *Process) libcPosix_fadvise(fd uint32, off, n uint64, advice uint32) uint32 {
	node, err := p.nodeAt(fd)

	if err != nil {
		return uint32(errnoOf(err))
	}

	if err := node.fadvise(int64(off), int64(n), int(advice)); err != nil {
		return uint32(errnoOf(err))
	}

	return 0
}

func (p *Process) libcPosix_fallocate(fd uint32, off, n uint64) uint32 {
	node, err := p.nodeAt(fd)

	if err != nil {
		return uint32(errnoOf(err))
	}

	if err := node.fallocate(int64(off), int64(n)); err != nil {
		return uint32(errnoOf(err))
	}

	return 0
}

// select writes back the time left, as Linux's.
func (p *Process) libcSelect(n, r, w, e, tv uint32) uint32 {
	timeout := -1
	var d time.Duration

	if tv != 0 {
		var err error

		if d, err = p.timeval(tv); err != nil {
			return p.result(0, err)
		}

		timeout = waitMs(d)
	}

	start := time.Now()
	count, err := p.selectFds(n, [3]fdSet{{r}, {w}, {e}}, timeout)

	if tv != 0 && err != syscall.EINVAL {
		p.putTimeval(tv, max(d-time.Since(start), 0))
	}

	return p.result(count, err)
}

func (p *Process) libcPselect(n, r, w, e, ts, mask uint32) uint32 {
	timeout := -1

	if ts != 0 {
		d, err := p.timespec(ts)

		if err != nil {
			return p.result(0, err)
		}

		timeout = waitMs(d)
	}

	return p.result(p.withMask(mask, func() (int, error) {
		return p.selectFds(n, [3]fdSet{{r}, {w}, {e}}, timeout)
	}))
}

func (p *Process) libcLseek(fd uint32, off uint64, whence uint32) uint64 {
	s, err := p.seekerAt(fd)

	if err != nil {
		return p.result64(0, err)
	}

	return p.result64(s.seek(int64(off), int(whence)))
}

func (p *Process) libcClose(fd uint32) uint32 {
	if !p.closeFd(int(fd)) {
		return p.result(0, syscall.EBADF)
	}

	return p.result(0, nil)
}

func (p *Process) libcDup(fd uint32) uint32 {
	obj, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	obj.ref()

	return p.result(p.install(obj, 0, false))
}

func (p *Process) libcDup2(old, new uint32) uint32 {
	if _, err := p.fd(old); err == nil && old == new {
		return p.result(int(new), nil)
	}

	return p.result(p.dupTo(old, new, false))
}

func (p *Process) libcDup3(old, new, flags uint32) uint32 {
	if old == new || flags&^syscall.O_CLOEXEC != 0 {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(p.dupTo(old, new, flags&syscall.O_CLOEXEC != 0))
}

func (p *Process) libcTcflow(fd, action uint32) uint32 {
	return p.termiosIoctl(fd, tcxonc, int(action))
}

func (p *Process) libcTcflush(fd, queue uint32) uint32 {
	return p.termiosIoctl(fd, tcflsh, int(queue))
}

// tcgetsid is the caller's session: the kernel keeps no session of
// a terminal.
func (p *Process) libcTcgetsid(fd uint32) uint32 {
	if p.libcIsatty(fd) == 0 {
		return p.result(0, p.errno())
	}

	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	return p.result(int(p.sid), nil)
}

func (p *Process) libcCfgetospeed(at uint32) uint32 {
	return p.u32(at+8) & cbaud
}

func (p *Process) libcCfgetispeed(at uint32) uint32 {
	return p.u32(at+8) & cbaud
}

// cfsetispeed's 0 is the output speed, as POSIX has it.
func (p *Process) libcCfsetispeed(at, speed uint32) uint32 {
	if speed == 0 {
		return 0
	}

	return p.cfsetspeed(at, speed)
}

func (p *Process) libcFsync(fd uint32) uint32 {
	n, err := p.nodeAt(fd)

	if err == nil {
		err = n.sync(false)
	}

	return p.result(0, err)
}

func (p *Process) libcFtruncate(fd uint32, size uint64) uint32 {
	n, err := p.nodeAt(fd)

	if err == nil {
		err = n.truncate(int64(size))
	}

	return p.result(0, err)
}

func (p *Process) libcFstat(fd, at uint32) uint32 {
	obj, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	var st syscall.Stat_t

	if err := obj.stat(&st); err != nil {
		return p.result(0, err)
	}

	p.putStat(at, &st)

	return p.result(0, nil)
}

func (p *Process) libcFchmod(fd, mode uint32) uint32 {
	n, err := p.nodeAt(fd)

	if err == nil {
		err = n.chmod(mode & 0o7777)
	}

	return p.result(0, err)
}

func (p *Process) libcFchown(fd, uid, gid uint32) uint32 {
	n, err := p.nodeAt(fd)

	if err == nil {
		err = n.chown(int(int32(uid)), int(int32(gid)))
	}

	return p.result(0, err)
}

func (p *Process) libcFutimes(fd, times uint32) uint32 {
	n, err := p.nodeAt(fd)

	if err == nil {
		err = n.utimes(p.timevals(times))
	}

	return p.result(0, err)
}

func (p *Process) libcFutimens(fd, times uint32) uint32 {
	n, err := p.nodeAt(fd)

	if err == nil {
		err = n.utimes(p.timespecs(times))
	}

	return p.result(0, err)
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

func (p *Process) libcFcntl(fd, cmd, va uint32) uint32 {
	obj, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	switch cmd {
	case fDupfd, fDupfdCloexec:
		min := p.varArg(va, 0)

		if uint64(min) >= p.limits[rlimitNofile].cur {
			return p.result(0, syscall.EINVAL)
		}

		obj.ref()

		return p.result(p.install(obj, int(min), cmd == fDupfdCloexec))
	case fGetfd:
		if p.files[fd].cloexec {
			return p.result(1, nil)
		}

		return p.result(0, nil)
	case fSetfd:
		p.files[fd].cloexec = p.varArg(va, 0)&1 != 0

		return p.result(0, nil)
	case fGetfl:
		return p.result(obj.flags())
	case fGetlk, fSetlk, fSetlkw:
		l, err := p.lockerAt(fd)

		if err != nil {
			return p.result(0, err)
		}

		return p.result(0, p.recordLock(l, cmd, p.varArg(va, 0)))
	case fSetfl:
		return p.result(0, obj.setFlags(int(p.varArg(va, 0))))
	}

	return p.result(0, syscall.EINVAL)
}

const (
	tcgets     = 0x5401
	tcsets     = 0x5402
	tcsbrk     = 0x5409
	tcxonc     = 0x540a
	tcflsh     = 0x540b
	tiocgpgrp  = 0x540f
	tiocspgrp  = 0x5410
	tiocgwinsz = 0x5413
	tiocswinsz = 0x5414
	fionread   = 0x541b
	fionbio    = 0x5421
	fionclex   = 0x5450
	fioclex    = 0x5451
)

// struct termios flags and control characters, the guest's termios.h.
const (
	ignbrk = 0o000001
	brkint = 0o000002
	parmrk = 0o000010
	istrip = 0o000040
	inlcr  = 0o000100
	igncr  = 0o000200
	icrnl  = 0o000400
	ixon   = 0o002000
	opost  = 0o000001
	csize  = 0o000060
	cs8    = 0o000060
	parenb = 0o000400
	echo   = 0o000010
	echonl = 0o000100
	icanon = 0o000002
	isig   = 0o000001
	iexten = 0o100000
	vtime  = 5
	vmin   = 6
)

// ioctl: the descriptor's own requests, FIONBIO, FIOCLEX and FIONCLEX,
// are the kernel's; the rest go to the object with the host's layout.
func (p *Process) libcIoctl(fd, req, va uint32) uint32 {
	obj, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	switch req {
	case fionbio:
		fl, err := obj.flags()

		if err != nil {
			return p.result(0, err)
		}

		fl &^= syscall.O_NONBLOCK

		if p.u32(p.varArg(va, 0)) != 0 {
			fl |= syscall.O_NONBLOCK
		}

		return p.result(0, obj.setFlags(fl))
	case fioclex, fionclex:
		p.files[fd].cloexec = req == fioclex

		return p.result(0, nil)
	}

	i, ok := obj.(Ioctl)

	if !ok {
		return p.result(0, syscall.ENOTTY)
	}

	switch req {
	case tiocgwinsz, tiocswinsz:
		var ws [8]byte
		at := p.varArg(va, 0)

		if req == tiocswinsz {
			copy(ws[:], p.span(at, 8))
		}

		if err := i.ioctl(uintptr(req), unsafe.Pointer(&ws)); err != nil {
			return p.result(0, err)
		}

		if req == tiocgwinsz {
			copy(p.span(at, 8), ws[:])
		}

		return p.result(0, nil)
	case fionread:
		var n int32

		if err := i.ioctl(fionread, unsafe.Pointer(&n)); err != nil {
			return p.result(0, err)
		}

		p.put32(p.varArg(va, 0), uint32(n))

		return p.result(0, nil)
	case tcgets:
		return p.libcTcgetattr(fd, p.varArg(va, 0))
	case tcsets, tcsets + 1, tcsets + 2:
		return p.libcTcsetattr(fd, req-tcsets, p.varArg(va, 0))
	case tiocgpgrp:
		r := p.libcTcgetpgrp(fd)

		if int32(r) >= 0 {
			p.put32(p.varArg(va, 0), r)
			r = 0
		}

		return r
	case tiocspgrp:
		return p.libcTcsetpgrp(fd, p.u32(p.varArg(va, 0)))
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

// terminal is the Ioctl of a descriptor that is a terminal: one that
// answers TCGETS.
func (p *Process) terminal(fd uint32) (Ioctl, *hostTermios, error) {
	i, err := p.ioctlAt(fd)

	if err != nil {
		return nil, nil, err
	}

	var t hostTermios

	if err := i.ioctl(tcgets, unsafe.Pointer(&t)); err != nil {
		return nil, nil, err
	}

	return i, &t, nil
}

func (p *Process) libcIsatty(fd uint32) uint32 {
	if _, err := p.fd(fd); err != nil {
		p.setErrno(syscall.EBADF)

		return 0
	}

	if _, _, err := p.terminal(fd); err != nil {
		p.setErrno(syscall.ENOTTY)

		return 0
	}

	return 1
}

func (p *Process) libcTcgetattr(fd, at uint32) uint32 {
	_, t, err := p.terminal(fd)

	if err != nil {
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

func (p *Process) libcTcsetattr(fd, action, at uint32) uint32 {
	i, err := p.ioctlAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	if action > 2 {
		return p.result(0, syscall.EINVAL)
	}

	t := p.guestTermios(at)

	return p.result(0, i.ioctl(uintptr(tcsets+action), unsafe.Pointer(&t)))
}

// termiosIoctl runs an ioctl of a terminal's with an int argument, which
// the object takes pointed to.
func (p *Process) termiosIoctl(fd uint32, req uintptr, arg int) uint32 {
	i, err := p.ioctlAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(0, i.ioctl(req, unsafe.Pointer(&arg)))
}

// cbaud is the speed of termios.h's c_cflag.
const cbaud = 0o010017

// cfsetspeed sets the speed of a guest's struct termios, as musl's: both
// speeds are c_cflag's.
func (p *Process) cfsetspeed(at, speed uint32) uint32 {
	if speed&^cbaud != 0 {
		p.setErrno(syscall.EINVAL)

		return ^uint32(0)
	}

	p.put32(at+8, p.u32(at+8)&^cbaud|speed)

	return 0
}

// cfmakeraw puts the guest's struct termios in raw mode, the flags of
// termios.h: no line editing, no signals, eight bit words.
func (p *Process) libcCfmakeraw(at uint32) {
	b := p.span(at, 60)
	le := binary.LittleEndian

	off := func(o int, clear, set uint32) {
		le.PutUint32(b[o:], le.Uint32(b[o:])&^clear|set)
	}

	off(0, ignbrk|brkint|parmrk|istrip|inlcr|igncr|icrnl|ixon, 0)
	off(4, opost, 0)
	off(8, csize|parenb, cs8)
	off(12, echo|echonl|icanon|isig|iexten, 0)
	b[17+vtime] = 0
	b[17+vmin] = 1
}

func (p *Process) libcPoll(at, n, timeout uint32) uint32 {
	return p.result(p.poll(at, n, int(int32(timeout))))
}

func (p *Process) libcPpoll(at, n, ts, mask uint32) uint32 {
	timeout := -1

	if ts != 0 {
		d, err := p.timespec(ts)

		if err != nil {
			return p.result(0, err)
		}

		timeout = waitMs(d)
	}

	return p.result(p.withMask(mask, func() (int, error) {
		return p.poll(at, n, timeout)
	}))
}

// poll polls a guest's array of struct pollfd.
func (p *Process) poll(at, n uint32, timeout int) (int, error) {
	if n > 1<<16 {
		return 0, syscall.EINVAL
	}

	guest := p.span(at, 8*n)
	entries := make([]pollEntry, 0, n)
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

		obj, err := p.fd(uint32(fd))

		if err != nil {
			binary.LittleEndian.PutUint16(e[6:], pollNval)
			invalid++

			continue
		}

		slot[i] = len(entries)
		entries = append(entries, pollEntry{obj: obj, events: int16(binary.LittleEndian.Uint16(e[4:]))})
	}

	if invalid > 0 {
		timeout = 0
	}

	ready, err := p.waitObjects(entries, timeout, false)

	if err != nil {
		return 0, err
	}

	if ready > 0 {
		guest = p.span(at, 8*n)

		for i := range n {
			if slot[i] >= 0 {
				binary.LittleEndian.PutUint16(guest[8*i+6:], uint16(entries[slot[i]].revents))
			}
		}
	}

	return ready + invalid, nil
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

func (p *Process) libcReadv(fd, iovs, count uint32) uint32 {
	return p.vectored(fd, iovs, count, pollIn, syscall.SYS_READV, 0)
}

func (p *Process) libcWritev(fd, iovs, count uint32) uint32 {
	return p.vectored(fd, iovs, count, pollOut, syscall.SYS_WRITEV, 0)
}

func (p *Process) libcPreadv(fd, iovs, count uint32, off uint64) uint32 {
	return p.vectored(fd, iovs, count, pollIn, syscall.SYS_PREADV, off)
}

func (p *Process) libcPwritev(fd, iovs, count uint32, off uint64) uint32 {
	return p.vectored(fd, iovs, count, pollOut, syscall.SYS_PWRITEV, off)
}

func (p *Process) libcPipe(fds uint32) uint32 {
	return p.pipe(fds, 0)
}

func (p *Process) libcPipe2(fds, flags uint32) uint32 {
	return p.pipe(fds, flags)
}

func (p *Process) libcTcdrain(fd uint32) uint32 {
	return p.termiosIoctl(fd, tcsbrk, 1)
}

func (p *Process) libcTcsendbreak(fd, duration uint32) uint32 {
	return p.termiosIoctl(fd, tcsbrk, 0)
}

func (p *Process) libcCfsetospeed(at, speed uint32) uint32 {
	return p.cfsetspeed(at, speed)
}

func (p *Process) libcCfsetspeed(at, speed uint32) uint32 {
	return p.cfsetspeed(at, speed)
}
