//go:build linux && amd64

package main

import (
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// An open file is a File: what a descriptor refers to, shared by the
// descriptors and processes that refer to it, and what every kind of call
// asks of it. What else an object can do is a role, an interface a call
// casts to: a Transfer reads and writes, a Seeker seeks, a Dir lists
// entries, a Node is a file of a host file system with its metadata, a
// Socket speaks sockets, an Ioctl takes ioctls, a Locker takes locks. An
// object of the host (kernel_host.go) is a host descriptor and plays the
// roles the host's kind of file does; an object of the kernel (a pipe,
// kernel_pipe.go) plays only its own.
type File interface {
	// ref takes a reference, unref drops one; the last closes the object.
	ref()
	unref()
	// stat is fstat.
	stat(st *syscall.Stat_t) error
	// host is the host descriptor the object is, or -1 for an object of the
	// kernel: a wait on a host object polls its descriptor, a wait on an
	// object of the kernel asks its queue to be woken by its changes.
	host() int
	poll(events int16) int16
	queue() *waitQueue
	// flags are the status flags of fcntl: the access mode, O_NONBLOCK and
	// what else the object keeps; setFlags sets those F_SETFL may.
	flags() (int, error)
	setFlags(fl int) error
}

// ioMode is how one transfer goes: nonblock as with MSG_DONTWAIT, quiet
// with MSG_NOSIGNAL, an EPIPE without SIGPIPE.
type ioMode struct {
	nonblock bool
	quiet    bool
}

type Transfer interface {
	File
	read(p *Process, b []byte, mode ioMode) (int, error)
	write(p *Process, b []byte, mode ioMode) (int, error)
	pread(p *Process, b []byte, off int64) (int, error)
	pwrite(p *Process, b []byte, off int64) (int, error)
}

// A Vectored transfer runs readv, writev, preadv or pwritev on the host
// with iovecs into the guest's memory: call is the system call, off its
// offset. A Transfer without the role gathers and scatters instead.
type Vectored interface {
	Transfer
	vectored(p *Process, call uintptr, iov []syscall.Iovec, off uint64, events int16) (int, error)
}

type Seeker interface {
	File
	seek(off int64, whence int) (int64, error)
}

// A Dir lists its entries as getdents64 does, into a buffer of the guest's
// struct dirent layout, and has a host path the kernel resolves paths
// relative to.
type Dir interface {
	Pathed
	readdir(b []byte) (int, error)
}

// A Pathed object has a path on the host: a file of a host file system.
type Pathed interface {
	File
	hostPath() (string, error)
}

// A Node is a file of a host file system: what the metadata calls of a
// descriptor work on.
type Node interface {
	Pathed
	truncate(size int64) error
	sync(data bool) error
	chmod(mode uint32) error
	chown(uid, gid int) error
	utimes(ts []syscall.Timespec) error
	statfs(st *syscall.Statfs_t) error
	fadvise(off, n int64, advice int) error
	fallocate(off, n int64) error
}

// A Socket takes the socket calls as libc makes them, with the guest's
// addresses and buffers; the addresses it returns are the guest's.
type Socket interface {
	Transfer
	bind(p *Process, at, n uint32) error
	connect(p *Process, at, n uint32) error
	listen(backlog int) error
	// accept returns the connection and the peer's address.
	accept(p *Process, flags uint32) (File, []byte, error)
	sendmsg(p *Process, at, flags uint32) (int, error)
	recvmsg(p *Process, at, flags uint32) (int, error)
	sendto(p *Process, buf, n, flags, addr, alen uint32) (int, error)
	recvfrom(p *Process, buf, n, flags uint32) (int, []byte, error)
	sockname(p *Process) ([]byte, error)
	peername(p *Process) ([]byte, error)
	getsockopt(p *Process, level, name, at, lenAt uint32) error
	setsockopt(p *Process, level, name, at, n uint32) error
	shutdown(how int) error
}

// An Ioctl takes ioctls with the host's argument layout, which the libc
// functions convert the guest's to; a request that takes an int, TCSBRK,
// TCXONC, TCFLSH, takes it pointed to.
type Ioctl interface {
	File
	ioctl(req uintptr, arg unsafe.Pointer) error
}

// A Locker takes flock's locks and the record locks of fcntl, which try
// without blocking: EAGAIN or EACCES is a lock held by another.
type Locker interface {
	File
	flock(op int) error
	recordLock(cmd int, lk *syscall.Flock_t) error
}

type Descriptor struct {
	obj     File
	cloexec bool
}

const (
	pollIn     = 0x1
	pollPri    = 0x2
	pollOut    = 0x4
	pollErr    = 0x8
	pollHup    = 0x10
	pollNval   = 0x20
	pollRdnorm = 0x40
	pollWrnorm = 0x100
)

// fd is the object of a descriptor.
func (p *Process) fd(fd uint32) (File, error) {
	if int(fd) >= len(p.files) || p.files[fd] == nil {
		return nil, syscall.EBADF
	}

	return p.files[fd].obj, nil
}

// transferAt is the Transfer of a descriptor: read on a directory is
// EISDIR, as Linux has it, write and the rest EBADF.
func (p *Process) transferAt(fd uint32, read bool) (Transfer, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if t, ok := obj.(Transfer); ok {
		return t, nil
	}

	if _, ok := obj.(Dir); ok && read {
		return nil, syscall.EISDIR
	}

	return nil, syscall.EBADF
}

func (p *Process) seekerAt(fd uint32) (Seeker, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if s, ok := obj.(Seeker); ok {
		return s, nil
	}

	return nil, syscall.ESPIPE
}

func (p *Process) dirAt(fd uint32) (Dir, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if d, ok := obj.(Dir); ok {
		return d, nil
	}

	return nil, syscall.ENOTDIR
}

func (p *Process) nodeAt(fd uint32) (Node, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if n, ok := obj.(Node); ok {
		return n, nil
	}

	return nil, syscall.EINVAL
}

func (p *Process) socketAt(fd uint32) (Socket, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if s, ok := obj.(Socket); ok {
		return s, nil
	}

	return nil, syscall.ENOTSOCK
}

func (p *Process) ioctlAt(fd uint32) (Ioctl, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if i, ok := obj.(Ioctl); ok {
		return i, nil
	}

	return nil, syscall.ENOTTY
}

func (p *Process) lockerAt(fd uint32) (Locker, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if l, ok := obj.(Locker); ok {
		return l, nil
	}

	return nil, syscall.EBADF
}

// hostAt is the host descriptor of a descriptor's object, for a call the
// host makes between two of its descriptors: EBADF for an object of the
// kernel.
func (p *Process) hostAt(fd uint32) (int, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return -1, err
	}

	if host := obj.host(); host >= 0 {
		return host, nil
	}

	return -1, syscall.EBADF
}

// install puts an object at the lowest free descriptor from min; the
// descriptor takes the reference.
func (p *Process) install(obj File, min int, cloexec bool) (int, error) {
	limit := int(min64(p.limits[rlimitNofile].cur, 1<<20))

	for fd := min; fd < limit; fd++ {
		if fd >= len(p.files) {
			p.files = append(p.files, make([]*Descriptor, fd+1-len(p.files))...)
		}

		if p.files[fd] == nil {
			p.files[fd] = &Descriptor{obj: obj, cloexec: cloexec}

			return fd, nil
		}
	}

	obj.unref()

	return -1, syscall.EMFILE
}

func min64(a, b uint64) uint64 {
	return min(a, b)
}

func (p *Process) closeFd(fd int) bool {
	if fd < 0 || fd >= len(p.files) || p.files[fd] == nil {
		return false
	}

	p.files[fd].obj.unref()
	p.files[fd] = nil

	return true
}

// dupTo makes new refer to old's object, closing what new referred to.
func (p *Process) dupTo(old, new uint32, cloexec bool) (int, error) {
	obj, err := p.fd(old)

	if err != nil {
		return 0, err
	}

	if uint64(new) >= p.limits[rlimitNofile].cur {
		return 0, syscall.EBADF
	}

	p.placeAt(obj, new, cloexec)

	return int(new), nil
}

// placeAt makes a descriptor refer to an object, closing what it referred
// to; the descriptor is below the process's limit.
func (p *Process) placeAt(obj File, fd uint32, cloexec bool) {
	if int(fd) >= len(p.files) {
		p.files = append(p.files, make([]*Descriptor, int(fd)+1-len(p.files))...)
	}

	obj.ref()
	p.closeFd(int(fd))
	p.files[fd] = &Descriptor{obj: obj, cloexec: cloexec}
}

// A waitQueue is the processes waiting on an object of the kernel: a
// change of the object wakes them, as a signal would, to look again.
type waitQueue struct {
	mu    sync.Mutex
	procs []*Process
}

func (q *waitQueue) add(p *Process) {
	q.mu.Lock()
	q.procs = append(q.procs, p)
	q.mu.Unlock()
}

func (q *waitQueue) remove(p *Process) {
	q.mu.Lock()

	for i, r := range q.procs {
		if r == p {
			q.procs[i] = q.procs[len(q.procs)-1]
			q.procs = q.procs[:len(q.procs)-1]

			break
		}
	}

	q.mu.Unlock()
}

func (q *waitQueue) wake() {
	q.mu.Lock()

	for _, p := range q.procs {
		p.wakeUp()
	}

	q.mu.Unlock()
}

// wakeSignal reports whether a signal interrupts a blocking call: one the
// process would deliver now. It drains the wake descriptor first, so a
// signal sent after the check makes the next poll return.
func (p *Process) wakeSignal() bool {
	var b [8]byte
	syscall.Read(p.wake, b[:])

	return p.sig.deliverable() != 0
}

// deadline is when a wait of timeout milliseconds ends, or zero for none.
func deadline(timeout int) time.Time {
	if timeout < 0 {
		return time.Time{}
	}

	return time.Now().Add(time.Duration(timeout) * time.Millisecond)
}

// left is the milliseconds to a deadline, -1 for none, and whether it
// passed.
func left(end time.Time) (int, bool) {
	if end.IsZero() {
		return -1, false
	}

	d := time.Until(end)

	if d <= 0 {
		return 0, true
	}

	return waitMs(d), false
}

// waitFds polls host descriptors and the wake descriptor until one is ready,
// the timeout in milliseconds passes, or a signal interrupts: then the
// error is EINTR, after the handlers ran when all of them restart calls and
// restart is set.
func (p *Process) waitFds(fds []pollFd, timeout int, restart bool) (int, error) {
	all := append(fds, pollFd{fd: int32(p.wake), events: pollIn})
	end := deadline(timeout)

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

		wait, passed := left(end)
		n, err := poll(all, wait)

		if err == syscall.EINTR {
			continue
		}

		if err != nil {
			return 0, err
		}

		if all[len(all)-1].revents != 0 {
			n--

			if n == 0 && !passed {
				continue
			}
		}

		copy(fds, all)

		return n, nil
	}
}

// pollEntry is an object and the events asked of it, revents those ready.
type pollEntry struct {
	obj     File
	events  int16
	revents int16
}

// waitObjects waits as waitFds does until an object is ready for its
// events: a host object when the host says so, an object of the kernel
// when it does, asked again whenever it wakes the process. It returns how
// many are ready.
func (p *Process) waitObjects(entries []pollEntry, timeout int, restart bool) (int, error) {
	host := make([]pollFd, 0, len(entries)+1)
	from := make([]int, 0, len(entries))
	kernel := false

	for i := range entries {
		e := &entries[i]
		e.revents = 0

		if fd := e.obj.host(); fd >= 0 {
			host = append(host, pollFd{fd: int32(fd), events: e.events})
			from = append(from, i)
		} else if q := e.obj.queue(); q != nil {
			q.add(p)
			defer q.remove(p)
			kernel = true
		}
	}

	host = append(host, pollFd{fd: int32(p.wake), events: pollIn})
	end := deadline(timeout)

	for {
		if p.wakeSignal() {
			if restart && p.interrupted() {
				continue
			}

			return 0, syscall.EINTR
		}

		ready := 0

		if kernel {
			for i := range entries {
				if e := &entries[i]; e.obj.host() < 0 {
					e.revents = e.obj.poll(e.events)

					if e.revents != 0 {
						ready++
					}
				}
			}
		}

		for i := range host {
			host[i].revents = 0
		}

		wait, passed := left(end)

		if ready > 0 {
			wait = 0
		}

		n, err := poll(host, wait)

		if err == syscall.EINTR {
			continue
		}

		if err != nil {
			return 0, err
		}

		if host[len(host)-1].revents != 0 {
			n--
		}

		if n == 0 && ready == 0 && !passed {
			continue
		}

		for k, i := range from {
			entries[i].revents = host[k].revents
		}

		return ready + n, nil
	}
}

// waitObject waits until one object is ready for events.
func (p *Process) waitObject(obj File, events int16, timeout int, restart bool) (int, error) {
	return p.waitObjects([]pollEntry{{obj: obj, events: events}}, timeout, restart)
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

// waitMs is a timeout in milliseconds of poll, rounded up and bounded.
func waitMs(d time.Duration) int {
	return int(min((d+time.Millisecond-1)/time.Millisecond, maxPollWaitMs))
}

const maxPollWaitMs = 1 << 30
