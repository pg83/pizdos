//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"syscall"
	"unsafe"
)

// Pseudoterminals are the host's: /dev/ptmx and /dev/pts are host devices
// in every root (kernel_path.go).

const (
	tiocsptlck = 0x40045431
	tiocgptn   = 0x80045430
)

func ptsName(n uint32) string {
	return fmt.Sprintf("/dev/pts/%d", n)
}

// ptyNumber is the number of a master's terminal.
func ptyNumber(fd int) (uint32, error) {
	var n uint32
	err := ioctlPtr(fd, tiocgptn, unsafe.Pointer(&n))

	return n, err
}

func unlockPty(fd int) error {
	var unlock int32

	return ioctlPtr(fd, tiocsptlck, unsafe.Pointer(&unlock))
}

// guestTermios reads a guest's struct termios as the host's.
func (p *Process) guestTermios(at uint32) hostTermios {
	b := p.span(at, 60)
	le := binary.LittleEndian
	t := hostTermios{iflag: le.Uint32(b), oflag: le.Uint32(b[4:]), cflag: le.Uint32(b[8:]), lflag: le.Uint32(b[12:]), line: b[16]}
	copy(t.cc[:], b[17:])

	return t
}

// openPty opens a pair of host descriptors, master and terminal, the
// terminal set to a guest's termios and winsize when they are given.
func (p *Process) openPty(termp, winp uint32) (int, int, uint32, error) {
	master, err := syscall.Open("/dev/ptmx", syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_CLOEXEC, 0)

	if err != nil {
		return -1, -1, 0, err
	}

	n, err := ptyNumber(master)

	if err == nil {
		err = unlockPty(master)
	}

	slave := -1

	if err == nil {
		slave, err = syscall.Open(ptsName(n), syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_CLOEXEC, 0)
	}

	if err == nil && termp != 0 {
		t := p.guestTermios(termp)
		err = ioctlPtr(slave, tcsets, unsafe.Pointer(&t))
	}

	if err == nil && winp != 0 {
		var ws [8]byte
		copy(ws[:], p.span(winp, 8))
		err = ioctlPtr(slave, tiocswinsz, unsafe.Pointer(&ws))
	}

	if err != nil {
		syscall.Close(master)

		if slave >= 0 {
			syscall.Close(slave)
		}

		return -1, -1, 0, err
	}

	return master, slave, n, nil
}

// forkpty forks a child whose standard descriptors are a new terminal, in
// a session of its own, and returns its pid with the master at amaster.
func (p *Process) forkpty(amaster, name, termp, winp uint32) uint32 {
	master, slave, n, err := p.openPty(termp, winp)

	if err != nil {
		return p.result(0, err)
	}

	if name != 0 {
		p.putString(name, ptsName(n))
	}

	pid, err := p.fork(func(c *Process) {
		f := newFile(slave, true)

		for fd := range 3 {
			if fd >= len(c.files) {
				c.files = append(c.files, nil)
			}

			c.closeFd(fd)

			if fd > 0 {
				f.ref()
			}

			c.files[fd] = &Descriptor{file: f}
		}

		c.k.mu.Lock()
		c.sid, c.pgid = c.pid, c.pid
		c.k.mu.Unlock()
	})

	if err != nil {
		syscall.Close(master)
		syscall.Close(slave)

		return p.result(0, err)
	}

	fd, err := p.install(newFile(master, true), 0, false)

	if err != nil {
		return p.result(0, err)
	}

	p.put32(amaster, uint32(fd))

	return p.result(int(pid), nil)
}

func (k *Kernel) ptyFunctions() map[string]any {
	return map[string]any{
		"posix_openpt": libcPosix_openpt,
		"grantpt":      libcGrantpt,
		"unlockpt":     libcUnlockpt,
		"ptsname":      libcPtsname,
		"ptsname_r":    libcPtsname_r,
		"openpty":      libcOpenpty,
		"forkpty":      libcForkpty,
	}
}

func libcPosix_openpt(ctx uintptr, flags uint32) uint32 {
	p := proc(ctx)
	fd, err := p.openat2("/dev/ptmx", int(flags&^syscall.O_CLOEXEC), 0)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(p.install(newFile(fd, true), 0, flags&syscall.O_CLOEXEC != 0))
}

// grantpt has nothing to do on Linux's devpts.
func libcGrantpt(ctx uintptr, fd uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err == nil {
		_, err = ptyNumber(f.fd)
	}

	if err == syscall.ENOTTY {
		err = syscall.EINVAL
	}

	return p.result(0, err)
}

func libcUnlockpt(ctx uintptr, fd uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err == nil {
		err = unlockPty(f.fd)
	}

	return p.result(0, err)
}

func libcPtsname(ctx uintptr, fd uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err != nil {
		p.setErrno(errnoOf(err))

		return 0
	}

	n, err := ptyNumber(f.fd)

	if err != nil {
		p.setErrno(errnoOf(err))

		return 0
	}

	return p.constant(ptsName(n))
}

// ptsname_r returns an error number.
func libcPtsname_r(ctx uintptr, fd, buf, size uint32) uint32 {
	p := proc(ctx)
	f, err := p.fd(fd)

	if err != nil {
		return uint32(syscall.EBADF)
	}

	n, err := ptyNumber(f.fd)

	if err != nil {
		return uint32(errnoOf(err))
	}

	name := ptsName(n)

	if uint32(len(name)) >= size {
		return uint32(syscall.ERANGE)
	}

	p.putString(buf, name)

	return 0
}

func libcOpenpty(ctx uintptr, amaster, aslave, name, termp, winp uint32) uint32 {
	p := proc(ctx)
	master, slave, n, err := p.openPty(termp, winp)

	if err != nil {
		return p.result(0, err)
	}

	m, err := p.install(newFile(master, true), 0, false)

	if err != nil {
		syscall.Close(slave)

		return p.result(0, err)
	}

	s, err := p.install(newFile(slave, true), 0, false)

	if err != nil {
		p.closeFd(m)

		return p.result(0, err)
	}

	p.put32(amaster, uint32(m))
	p.put32(aslave, uint32(s))

	if name != 0 {
		p.putString(name, ptsName(n))
	}

	return p.result(0, nil)
}

func libcForkpty(ctx uintptr, amaster, name, termp, winp uint32) uint32 {
	return proc(ctx).forkpty(amaster, name, termp, winp)
}
