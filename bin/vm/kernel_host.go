//go:build linux && amd64

package main

import (
	"fmt"
	"os"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// A file of the host is a host descriptor of the kernel's, kept here and
// in kernel_hostdir.go, kernel_socket.go and kernel_pty.go, nowhere else:
// a hostFile is a regular file, a device, a terminal or a FIFO of the
// root, a hostDir a directory, a hostSocket a socket. A pollable one can
// block: a FIFO, a terminal, a socket, another character device. To a
// wait it is a file of the kernel like any other: a queue, which the
// host's readiness wakes through a goroutine sleeping in the host's poll
// (tell). One the kernel opened itself is nonblocking on the host and a
// guest's call blocks by waiting, unless the guest set O_NONBLOCK; one
// inherited from the host keeps its host flags, and a call waits first.
type hostObject struct {
	fd       int
	refs     atomic.Int32
	pollable bool
	owned    bool
	// nonblock is the guest's O_NONBLOCK.
	nonblock atomic.Bool
	// changed wakes the waits on the object; telling says a goroutine is
	// waiting on the host for input, and one for output.
	changed changed
	telling [2]atomic.Bool
}

// hostStream is a host object that transfers: a file or a socket. A
// socket's SO_RCVTIMEO and SO_SNDTIMEO in milliseconds, 0 for none, bound
// its blocking calls.
type hostStream struct {
	hostObject
	recvTimeout atomic.Int64
	sendTimeout atomic.Int64
}

type hostFile struct {
	hostStream
}

type hostDir struct {
	hostObject
}

var (
	_ Vectored = (*hostFile)(nil)
	_ Seeker   = (*hostFile)(nil)
	_ Node     = (*hostFile)(nil)
	_ Ioctl    = (*hostFile)(nil)
	_ Locker   = (*hostFile)(nil)
	_ Dir      = (*hostDir)(nil)
	_ Seeker   = (*hostDir)(nil)
	_ Node     = (*hostDir)(nil)
	_ Locker   = (*hostDir)(nil)
)

// newHostObject makes the object of a host descriptor, of the kind the
// host says it is; owned tells whether the kernel opened it itself.
func newHostObject(fd int, owned bool) File {
	var st syscall.Stat_t
	kind := uint32(0)

	if syscall.Fstat(fd, &st) == nil {
		kind = st.Mode & syscall.S_IFMT
	}

	switch kind {
	case syscall.S_IFDIR:
		d := &hostDir{}
		d.init(fd, owned, false)

		return d
	case syscall.S_IFSOCK:
		s := &hostSocket{}
		s.init(fd, owned, true)

		return s
	}

	f := &hostFile{}
	f.init(fd, owned, kind == syscall.S_IFIFO || kind == syscall.S_IFCHR)

	return f
}

// newHostDir is the object of a host directory descriptor the kernel
// resolves paths in: the root of a process.
func newHostDir(fd int) *hostDir {
	d := &hostDir{}
	d.init(fd, true, false)

	return d
}

func (o *hostObject) init(fd int, owned, pollable bool) {
	o.fd, o.owned, o.pollable = fd, owned, pollable
	o.refs.Store(1)
	fl, _ := fcntl(fd, syscall.F_GETFL, 0)
	o.nonblock.Store(fl&syscall.O_NONBLOCK != 0)

	if owned && pollable {
		fcntl(fd, syscall.F_SETFL, fl|syscall.O_NONBLOCK)
	}
}

func (o *hostObject) ref() {
	o.refs.Add(1)
}

func (o *hostObject) unref() {
	if o.refs.Add(-1) == 0 {
		syscall.Close(o.fd)
	}
}

// hostFdOf is the host descriptor of a file of the host, for a call the
// host makes between two of them.
func hostFdOf(f File) (int, bool) {
	h, ok := f.(interface{ hostFd() int })

	if !ok {
		return -1, false
	}

	return h.hostFd(), true
}

func (o *hostObject) hostFd() int {
	return o.fd
}

// changes tells the host of the wait and gives the change the host's
// readiness bumps; an object that cannot block is ready whenever asked.
func (o *hostObject) changes(events int16) []<-chan struct{} {
	if !o.pollable {
		return nil
	}

	ch := o.changed.wait()
	o.tell(events)

	return []<-chan struct{}{ch}
}

// tell has a goroutine sleep in the host's poll for the input or the
// output asked, and bump the change once: one goroutine a direction at a
// time, as a change wakes every wait. One that outlives the waits wakes
// nobody.
func (o *hostObject) tell(events int16) {
	for i, bits := range [2]int16{pollIn | pollPri | pollRdnorm, pollOut | pollWrnorm} {
		if events&bits == 0 || !o.telling[i].CompareAndSwap(false, true) {
			continue
		}

		go func() {
			pollWait(o.fd, bits)
			o.telling[i].Store(false)
			o.changed.bump()
		}()
	}
}

func (o *hostObject) stat(st *syscall.Stat_t) error {
	return syscall.Fstat(o.fd, st)
}

func (o *hostObject) poll(events int16) int16 {
	return pollNow(o.fd, events)
}

type pollFd struct {
	fd      int32
	events  int16
	revents int16
}

// pollNow asks the host which of events a descriptor is ready for now; a
// refusal is an error, which the call that follows tells.
func pollNow(fd int, events int16) int16 {
	fds := []pollFd{{fd: int32(fd), events: events}}

	for {
		r, _, e := syscall.Syscall(syscall.SYS_POLL, uintptr(unsafe.Pointer(&fds[0])), 1, 0)

		switch {
		case e == syscall.EINTR:
			continue
		case e != 0:
			return pollErr
		case r == 0:
			return 0
		}

		return fds[0].revents
	}
}

// pollWait sleeps in the host's poll until a descriptor is ready for
// events, or the host refuses.
func pollWait(fd int, events int16) {
	fds := []pollFd{{fd: int32(fd), events: events}}

	for {
		if _, _, e := syscall.Syscall(syscall.SYS_POLL, uintptr(unsafe.Pointer(&fds[0])), 1, ^uintptr(0)); e != syscall.EINTR {
			return
		}
	}
}

// flags are the host's, with the guest's O_NONBLOCK.
func (o *hostObject) flags() (int, error) {
	fl, err := fcntl(o.fd, syscall.F_GETFL, 0)

	if err != nil {
		return 0, err
	}

	fl &^= syscall.O_NONBLOCK

	if o.nonblock.Load() {
		fl |= syscall.O_NONBLOCK
	}

	return fl, nil
}

// setFlags sets O_APPEND and O_DIRECT on the host, and O_NONBLOCK too for a
// descriptor inherited from it; an owned pollable one stays nonblocking on
// the host, as its blocking calls poll.
func (o *hostObject) setFlags(arg int) error {
	fl, err := fcntl(o.fd, syscall.F_GETFL, 0)

	if err != nil {
		return err
	}

	fl = fl&^(syscall.O_APPEND|syscall.O_DIRECT) | arg&(syscall.O_APPEND|syscall.O_DIRECT)

	if !o.owned {
		fl = fl&^syscall.O_NONBLOCK | arg&syscall.O_NONBLOCK
	}

	if _, err := fcntl(o.fd, syscall.F_SETFL, fl); err != nil {
		return err
	}

	o.nonblock.Store(arg&syscall.O_NONBLOCK != 0)

	return nil
}

// hostPath is the host path of the object's descriptor.
func (o *hostObject) hostPath() (string, error) {
	return hostPath(o.fd)
}

// hostPath is the host path of a host descriptor.
func hostPath(fd int) (string, error) {
	if chaosFail("readlink") {
		return "", syscall.EIO
	}

	return os.Readlink(procFd(fd))
}

func procFd(fd int) string {
	return fmt.Sprintf("/proc/self/fd/%d", fd)
}

// fileStat reads metadata for a guest operation on a path. Resource setup
// and the loader use Fstat and File.stat directly, so a one-shot failure
// can be observed by the guest.
func fileStat(fd int, st *syscall.Stat_t) error {
	if chaosFail("file-stat") {
		return syscall.EIO
	}

	return syscall.Fstat(fd, st)
}

func (o *hostObject) truncate(size int64) error {
	return syscall.Ftruncate(o.fd, size)
}

func (o *hostObject) syncfs() error {
	if _, _, e := syscall.Syscall(sysSyncfs, uintptr(o.fd), 0, 0); e != 0 {
		return e
	}

	return nil
}

// access checks the file for mode, as the real ids do or, with AT_EACCESS,
// the effective ones.
func (o *hostObject) access(mode, flags uint32) error {
	return access(o.fd, mode, flags)
}

// readlink reads the link the descriptor is, O_PATH of one: EINVAL for
// any other file.
func (o *hostObject) readlink(out []byte) (int, error) {
	return readlinkFd(o.fd, out, false)
}

// copyRange is the host's copy_file_range; another node is a file of the
// host too, or no file for it: EINVAL.
func (o *hostObject) copyRange(dst Node, from, to *int64, n int, flags int) (int, error) {
	fd, ok := hostFdOf(dst)

	if !ok {
		return 0, syscall.EINVAL
	}

	r, _, e := syscall.Syscall6(sysCopyFileRange, uintptr(o.fd), uintptr(unsafe.Pointer(from)), uintptr(fd),
		uintptr(unsafe.Pointer(to)), uintptr(n), uintptr(flags))

	if e != 0 {
		return 0, e
	}

	return int(r), nil
}

func (o *hostObject) sync(data bool) error {
	if data {
		return syscall.Fdatasync(o.fd)
	}

	return syscall.Fsync(o.fd)
}

func (o *hostObject) chmod(mode uint32) error {
	return syscall.Fchmod(o.fd, mode)
}

func (o *hostObject) chown(uid, gid int) error {
	return syscall.Fchown(o.fd, uid, gid)
}

func (o *hostObject) utimes(ts []syscall.Timespec) error {
	return utimensat(o.fd, nil, ts, 0)
}

func (o *hostObject) statfs(st *syscall.Statfs_t) error {
	return syscall.Fstatfs(o.fd, st)
}

func (o *hostObject) fadvise(off, n int64, advice int) error {
	if _, _, e := syscall.Syscall6(syscall.SYS_FADVISE64, uintptr(o.fd), uintptr(off), uintptr(n), uintptr(advice), 0, 0); e != 0 {
		return e
	}

	return nil
}

func (o *hostObject) fallocate(off, n int64) error {
	for {
		err := syscall.Fallocate(o.fd, 0, off, n)

		if err != syscall.EINTR {
			return err
		}
	}
}

func (o *hostObject) ioctl(req uintptr, arg unsafe.Pointer) error {
	switch req {
	case tcsbrk, tcxonc, tcflsh:
		if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, uintptr(o.fd), req, uintptr(*(*int)(arg))); e != 0 {
			return e
		}

		return nil
	}

	return ioctlPtr(o.fd, req, arg)
}

func ioctlPtr(fd int, req uintptr, arg unsafe.Pointer) error {
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), req, uintptr(arg)); e != 0 {
		return e
	}

	return nil
}

func (o *hostObject) flock(op int) error {
	return syscall.Flock(o.fd, op|syscall.LOCK_NB)
}

// Record locks are the host's open file description locks: a guest's lock
// is its open file's, which fork shares, and does not end when another
// descriptor of the process closes.
func (o *hostObject) recordLock(cmd int, lk *syscall.Flock_t) error {
	if _, _, e := syscall.Syscall(syscall.SYS_FCNTL, uintptr(o.fd), uintptr(cmd), uintptr(unsafe.Pointer(lk))); e != 0 {
		return e
	}

	return nil
}

// A file and a directory seek; a socket does not, so it is no Seeker: a
// mapping of one is no device for it.
func (f *hostFile) seek(off int64, whence int) (int64, error) {
	return syscall.Seek(f.fd, off, whence)
}

func (d *hostDir) seek(off int64, whence int) (int64, error) {
	return syscall.Seek(d.fd, off, whence)
}

func (d *hostDir) readdir(b []byte) (int, error) {
	return syscall.Getdents(d.fd, b)
}

// timeout is the milliseconds a blocking transfer of events waits, or -1.
func (s *hostStream) timeout(events int16) int {
	ms := s.sendTimeout.Load()

	if events&pollIn != 0 {
		ms = s.recvTimeout.Load()
	}

	if ms == 0 {
		return -1
	}

	return int(min(ms, maxPollWaitMs))
}

// transfer runs a read or a write of the stream, blocking as described at
// hostObject; a timeout ends a blocking wait with EAGAIN.
func (s *hostStream) transfer(p *Process, events int16, mode ioMode, op func() (int, error)) (int, error) {
	nonblock := mode.nonblock || s.nonblock.Load()
	timeout := s.timeout(events)

	for {
		if s.pollable && !s.owned {
			wait := timeout

			if nonblock {
				wait = 0
			}

			n, err := p.waitObject(s, events, wait, timeout < 0)

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

		if err == syscall.EAGAIN && s.pollable && s.owned && !nonblock {
			ready, err := p.waitObject(s, events, timeout, timeout < 0)

			if err != nil {
				return 0, err
			}

			if ready == 0 {
				return 0, syscall.EAGAIN
			}

			continue
		}

		if err == syscall.EPIPE && !mode.quiet {
			p.sendSelf(int(syscall.SIGPIPE))
		}

		return n, err
	}
}

func (s *hostStream) read(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("read") {
		return 0, syscall.EIO
	}

	return s.transfer(p, pollIn, mode, func() (int, error) {
		return syscall.Read(s.fd, b)
	})
}

func (s *hostStream) write(p *Process, b []byte, mode ioMode) (int, error) {
	if chaosFail("write") {
		return 0, syscall.EIO
	}

	return s.transfer(p, pollOut, mode, func() (int, error) {
		return syscall.Write(s.fd, b)
	})
}

func (s *hostStream) pread(p *Process, b []byte, off int64) (int, error) {
	return s.transfer(p, pollIn, ioMode{}, func() (int, error) {
		return syscall.Pread(s.fd, b, off)
	})
}

func (s *hostStream) pwrite(p *Process, b []byte, off int64) (int, error) {
	return s.transfer(p, pollOut, ioMode{}, func() (int, error) {
		return syscall.Pwrite(s.fd, b, off)
	})
}

func (s *hostStream) vectored(p *Process, call uintptr, iov []syscall.Iovec, off uint64, events int16) (int, error) {
	return s.transfer(p, events, ioMode{}, func() (int, error) {
		r, _, e := syscall.Syscall6(call, uintptr(s.fd), uintptr(unsafe.Pointer(&iov[0])), uintptr(len(iov)), uintptr(off), 0, 0)

		if e != 0 {
			return 0, e
		}

		return int(r), nil
	})
}

// dupHigh duplicates a host descriptor for the kernel.
func dupHigh(fd int) (int, error) {
	return fcntl(fd, syscall.F_DUPFD_CLOEXEC, 3)
}

// hostStdio is the three standard descriptors of vm as files of the
// first process, nil where vm has none.
func hostStdio() []File {
	files := make([]File, 3)

	for fd := range 3 {
		if host, err := dupHigh(fd); err == nil {
			files[fd] = newHostObject(host, false)
		}
	}

	return files
}

// openHostRoot opens a directory of the host for the root of the first
// process, and tells its path.
func openHostRoot(path string) (Dir, string, error) {
	fd, err := syscall.Open(path, oPath|syscall.O_DIRECTORY|syscall.O_CLOEXEC, 0)

	if err != nil {
		return nil, "", err
	}

	real, err := os.Readlink(procFd(fd))

	if err != nil {
		syscall.Close(fd)

		return nil, "", err
	}

	return newHostDir(fd), real, nil
}

// openHostFile opens a file of the host to read: the program vm runs.
func openHostFile(path string) (File, error) {
	fd, err := syscall.Open(path, syscall.O_RDONLY|syscall.O_CLOEXEC, 0)

	if err != nil {
		return nil, err
	}

	return newHostObject(fd, true), nil
}

// raiseFileLimit lets the kernel hold as many host descriptors as it may:
// every open file of the host of every process is one.
func raiseFileLimit() {
	var l syscall.Rlimit

	if syscall.Getrlimit(syscall.RLIMIT_NOFILE, &l) == nil && l.Cur < l.Max {
		l.Cur = l.Max
		syscall.Setrlimit(syscall.RLIMIT_NOFILE, &l)
	}
}
