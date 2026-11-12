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
// asks of it. What else a file can do is a role, an interface a call
// casts to: a Transfer reads and writes, a Seeker seeks, a Dir lists
// entries, a Node is a file of a host file system with its metadata, a
// Socket speaks sockets, an Ioctl takes ioctls, a Locker takes locks. A
// file of the host (kernel_host.go) plays the roles the host's kind of
// file does and keeps its host descriptor to itself; a file of the kernel
// (a pipe, kernel_pipe.go) plays only its own.
type File interface {
	// ref takes a reference, unref drops one; the last closes the object.
	ref()
	unref()
	// stat is fstat.
	stat(st *syscall.Stat_t) error
	// poll is the events ready now of those asked; watch adds the file to
	// a wait (waitSet): what the host polls for it, or the queue its
	// changes wake the process through.
	poll(events int16) int16
	watch(p *Process, w *waitSet, e *pollEntry)
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

// A Dir resolves paths. A process's start at its root, a Dir, and go to
// it whole, absolute; what the root is decides who resolves them: a
// directory of the host with openat2 (kernel_hostdir.go), a server with
// the file protocol (kernel_remote.go). An open directory is a Dir too:
// it lists its entries as getdents64 does, into a buffer of the guest's
// struct dirent layout, and knows its name, which the calls relative to
// its descriptor start from.
type Dir interface {
	File
	name(p *Process) (string, error)
	readdir(b []byte) (int, error)
	open(p *Process, path string, flags int, mode uint32) (File, error)
	statPath(p *Process, path string, follow bool, st *syscall.Stat_t) error
	readlinkPath(p *Process, path string) (string, error)
	mkdir(p *Process, path string, mode uint32) error
	unlink(p *Process, path string, dir bool) error
	rename(p *Process, old, new string) error
	link(p *Process, old, new string, follow bool) error
	symlink(p *Process, target, path string) error
	mknod(p *Process, path string, mode uint32, dev uint64) error
	accessPath(p *Process, path string, mode, flags uint32) error
	chmodPath(p *Process, path string, mode uint32) error
	chownPath(p *Process, path string, uid, gid int, follow bool) error
	utimesPath(p *Process, path string, ts []syscall.Timespec, follow bool) error
	truncatePath(p *Process, path string, size int64) error
	statfsPath(p *Process, path string, st *syscall.Statfs_t) error
	realpath(p *Process, path string) (string, error)
}

// A Pathed object has a path on the host: a file of a host file system.
type Pathed interface {
	File
	hostPath() (string, error)
}

// An Attrs has the attributes a descriptor's calls set: a file of the
// host and an open file of a server (kernel_remote.go) both.
type Attrs interface {
	File
	truncate(size int64) error
	sync(data bool) error
	chmod(mode uint32) error
	chown(uid, gid int) error
	utimes(ts []syscall.Timespec) error
}

// A Node is a file of a host file system: what the calls the host makes
// on a descriptor work on.
type Node interface {
	Attrs
	Pathed
	syncfs() error
	access(mode, flags uint32) error
	readlink(out []byte) (int, error)
	statfs(st *syscall.Statfs_t) error
	fadvise(off, n int64, advice int) error
	fallocate(off, n int64) error
	// copyRange is copy_file_range to another node, with the offsets of
	// the call when given.
	copyRange(dst Node, from, to *int64, n int, flags int) (int, error)
}

// A Socket takes the socket calls as libc makes them, with the guest's
// addresses and buffers; the addresses it returns are the guest's. A
// socket of the host or a pair of the kernel is a Transfer too; an open
// file of a server (kernel_remote.go) is a Socket for the program that
// speaks the protocol on it itself, and a Transfer only when a file.
type Socket interface {
	File
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

func (p *Process) attrsAt(fd uint32) (Attrs, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if a, ok := obj.(Attrs); ok {
		return a, nil
	}

	return nil, syscall.EINVAL
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

// A waitQueue is the processes waiting on a file of the kernel: a
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

// A waitSet is what one wait looks at: the host descriptors it polls, by
// the entries they are of, and the entries of files of the kernel it asks
// again whenever their queues wake the process.
type waitSet struct {
	host   []pollFd
	polled []*pollEntry
	asked  []*pollEntry
	queues []*waitQueue
}

// hostFd has the wait poll a host descriptor for an entry.
func (w *waitSet) hostFd(e *pollEntry, fd int) {
	w.host = append(w.host, pollFd{fd: int32(fd), events: e.events})
	w.polled = append(w.polled, e)
}

// queued has the wait ask an entry's file again when its queue wakes the
// process.
func (w *waitSet) queued(p *Process, e *pollEntry, q *waitQueue) {
	q.add(p)
	w.queues = append(w.queues, q)
	w.asked = append(w.asked, e)
}

// always has the wait ask an entry's file, which is ready whenever asked.
func (w *waitSet) always(e *pollEntry) {
	w.asked = append(w.asked, e)
}

func (w *waitSet) leave(p *Process) {
	for _, q := range w.queues {
		q.remove(p)
	}
}

// waitObjects waits as waitFds does until an object is ready for its
// events: a file of the host when the host says so, a file of the kernel
// when it does, asked again whenever it wakes the process. It returns how
// many are ready.
func (p *Process) waitObjects(entries []pollEntry, timeout int, restart bool) (int, error) {
	var w waitSet

	for i := range entries {
		entries[i].revents = 0
		entries[i].obj.watch(p, &w, &entries[i])
	}

	defer w.leave(p)
	host := append(w.host, pollFd{fd: int32(p.wake), events: pollIn})
	end := deadline(timeout)

	for {
		if p.wakeSignal() {
			if restart && p.interrupted() {
				continue
			}

			return 0, syscall.EINTR
		}

		ready := 0

		for _, e := range w.asked {
			e.revents = e.obj.poll(e.events)

			if e.revents != 0 {
				ready++
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

		for k, e := range w.polled {
			e.revents = host[k].revents
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
