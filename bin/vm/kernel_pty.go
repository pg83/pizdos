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

// ioctlCall takes an ioctl: an Ioctl of a descriptor, or a host
// descriptor the kernel holds.
type ioctlCall interface {
	ioctl(req uintptr, arg unsafe.Pointer) error
}

// ptyNumber is the number of a master's terminal.
func ptyNumber(i ioctlCall) (uint32, error) {
	var n uint32
	err := i.ioctl(tiocgptn, unsafe.Pointer(&n))

	return n, err
}

func unlockPty(i ioctlCall) error {
	var unlock int32

	return i.ioctl(tiocsptlck, unsafe.Pointer(&unlock))
}

// hostIoctl is the ioctlCall of a host descriptor the kernel holds.
type hostIoctl int

func (fd hostIoctl) ioctl(req uintptr, arg unsafe.Pointer) error {
	return ioctlPtr(int(fd), req, arg)
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

	n, err := ptyNumber(hostIoctl(master))

	if err == nil {
		err = unlockPty(hostIoctl(master))
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

	// Reserve the parent's descriptor before creating a child: EMFILE must
	// leave no child behind. The child drops its inherited master below.
	fd, err := p.install(newHostObject(master, true), 0, false)

	if err != nil {
		syscall.Close(slave)

		return p.result(0, err)
	}

	pid, err := p.fork(func(c *Process) {
		c.closeFd(fd)
		f := newHostObject(slave, true)

		for fd := range 3 {
			// inherit preserves the initial three descriptor slots, even closed.
			c.closeFd(fd)

			if fd > 0 {
				f.ref()
			}

			c.files[fd] = &Descriptor{obj: f}
		}

		c.k.mu.Lock()
		c.sid, c.pgid = c.pid, c.pid
		c.k.mu.Unlock()
	})

	if err != nil {
		p.closeFd(fd)
		syscall.Close(slave)

		return p.result(0, err)
	}

	p.put32(amaster, uint32(fd))

	return p.result(int(pid), nil)
}

func (p *Process) libcPosix_openpt(flags uint32) uint32 {
	f, err := p.openPath("/dev/ptmx", int(flags&^syscall.O_CLOEXEC), 0)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(p.install(f, 0, flags&syscall.O_CLOEXEC != 0))
}

// grantpt has nothing to do on Linux's devpts.
func (p *Process) libcGrantpt(fd uint32) uint32 {
	i, err := p.ioctlAt(fd)

	if err == nil {
		_, err = ptyNumber(i)
	}

	if err == syscall.ENOTTY {
		err = syscall.EINVAL
	}

	return p.result(0, err)
}

func (p *Process) libcUnlockpt(fd uint32) uint32 {
	i, err := p.ioctlAt(fd)

	if err == nil {
		err = unlockPty(i)
	}

	return p.result(0, err)
}

func (p *Process) libcPtsname(fd uint32) uint32 {
	i, err := p.ioctlAt(fd)

	if err != nil {
		p.setErrno(errnoOf(err))

		return 0
	}

	n, err := ptyNumber(i)

	if err != nil {
		p.setErrno(errnoOf(err))

		return 0
	}

	return p.constant(ptsName(n))
}

// ptsname_r returns an error number.
func (p *Process) libcPtsname_r(fd, buf, size uint32) uint32 {
	i, err := p.ioctlAt(fd)

	if err != nil {
		return uint32(errnoOf(err))
	}

	n, err := ptyNumber(i)

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

func (p *Process) libcOpenpty(amaster, aslave, name, termp, winp uint32) uint32 {
	master, slave, n, err := p.openPty(termp, winp)

	if err != nil {
		return p.result(0, err)
	}

	m, err := p.install(newHostObject(master, true), 0, false)

	if err != nil {
		syscall.Close(slave)

		return p.result(0, err)
	}

	s, err := p.install(newHostObject(slave, true), 0, false)

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

func (p *Process) libcForkpty(amaster, name, termp, winp uint32) uint32 {
	return p.forkpty(amaster, name, termp, winp)
}
