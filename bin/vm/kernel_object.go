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
// Socket speaks sockets, an Ioctl takes ioctls, a Locker takes locks, an
// Epoll watches files (kernel_epoll.go). A file of the host
// (kernel_host.go) plays the roles the host's kind of file does and keeps
// its host descriptor to itself; a file of the kernel (a pipe,
// kernel_pipe.go) plays only its own.
type File interface {
	// ref takes a reference, unref drops one; the last closes the object.
	ref()
	unref()
	// stat is fstat.
	stat(st *syscall.Stat_t) error
	// poll is the events ready now of those asked; watch adds the file to
	// a wait (waitSet): the queue its changes wake the waiter through.
	poll(events int16) int16
	watch(w *waitSet, e *pollEntry)
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

// A waker is what a change of a file wakes to look again: a process in a
// blocking call, or an epoll the file is in.
type waker interface {
	wakeUp()
}

// A waitQueue is the wakers waiting on a file of the kernel: a change of
// the object wakes them, as a signal would a process, to look again.
type waitQueue struct {
	mu     sync.Mutex
	wakers []waker
}

func (q *waitQueue) add(w waker) {
	q.mu.Lock()
	q.wakers = append(q.wakers, w)
	q.mu.Unlock()
}

func (q *waitQueue) remove(w waker) {
	q.mu.Lock()

	for i, r := range q.wakers {
		if r == w {
			q.wakers[i] = q.wakers[len(q.wakers)-1]
			q.wakers = q.wakers[:len(q.wakers)-1]

			break
		}
	}

	q.mu.Unlock()
}

func (q *waitQueue) wake() {
	q.mu.Lock()

	for _, w := range q.wakers {
		w.wakeUp()
	}

	q.mu.Unlock()
}

// wakeSignal reports whether a signal interrupts a blocking call: one the
// process would deliver now. It takes the wake first, so a wake after the
// check ends the next sleep.
func (p *Process) wakeSignal() bool {
	select {
	case <-p.wake:
	default:
	}

	return p.sig.deliverable() != 0
}

// sleepUntil sleeps until the process is woken or a deadline passes, zero
// for none; it tells whether the deadline passed.
func (p *Process) sleepUntil(end time.Time) bool {
	if end.IsZero() {
		<-p.wake

		return false
	}

	d := time.Until(end)

	if d <= 0 {
		return true
	}

	t := time.NewTimer(d)
	defer t.Stop()

	select {
	case <-p.wake:
		return false
	case <-t.C:
		return true
	}
}

// idle waits for timeout milliseconds, forever when negative, or until a
// signal interrupts: then the error is EINTR, after the handlers ran when
// all of them restart calls and restart is set.
func (p *Process) idle(timeout int, restart bool) error {
	_, err := p.waitObjects(nil, timeout, restart)

	return err
}

// deadline is when a wait of timeout milliseconds ends, or zero for none.
func deadline(timeout int) time.Time {
	if timeout < 0 {
		return time.Time{}
	}

	return time.Now().Add(time.Duration(timeout) * time.Millisecond)
}

// pollEntry is an object and the events asked of it, revents those ready.
type pollEntry struct {
	obj     File
	events  int16
	revents int16
}

// A waitSet is what one wait of a waker looks at: the entries it asks
// again whenever their files' queues wake the waiter, what it arms before
// every sleep, and what it undoes when it leaves.
type waitSet struct {
	by     waker
	asked  []*pollEntry
	queues []*waitQueue
	arms   []func()
	leaves []func()
}

// queued has the wait ask an entry's file again when its queue wakes the
// waiter.
func (w *waitSet) queued(e *pollEntry, q *waitQueue) {
	q.add(w.by)
	w.queues = append(w.queues, q)
	w.asked = append(w.asked, e)
}

// always has the wait ask an entry's file, which is ready whenever asked.
func (w *waitSet) always(e *pollEntry) {
	w.asked = append(w.asked, e)
}

// armed has the wait run arm before every sleep, after it asked the files:
// a file that is told of a wait from outside the kernel arms the telling.
func (w *waitSet) armed(arm func()) {
	w.arms = append(w.arms, arm)
}

// left has the wait run leave when it ends.
func (w *waitSet) left(leave func()) {
	w.leaves = append(w.leaves, leave)
}

func (w *waitSet) leave() {
	for _, q := range w.queues {
		q.remove(w.by)
	}

	for _, leave := range w.leaves {
		leave()
	}
}

// waitObjects waits until an object is ready for its events, asking every
// one again whenever one wakes the process, the timeout in milliseconds
// passes, or a signal interrupts: then the error is EINTR, after the
// handlers ran when all of them restart calls and restart is set. It
// returns how many are ready.
func (p *Process) waitObjects(entries []pollEntry, timeout int, restart bool) (int, error) {
	w := waitSet{by: p}

	for i := range entries {
		entries[i].revents = 0
		entries[i].obj.watch(&w, &entries[i])
	}

	defer w.leave()
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

		if ready > 0 {
			return ready, nil
		}

		for _, arm := range w.arms {
			arm()
		}

		if p.sleepUntil(end) {
			return 0, nil
		}
	}
}

// waitObject waits until one object is ready for events.
func (p *Process) waitObject(obj File, events int16, timeout int, restart bool) (int, error) {
	return p.waitObjects([]pollEntry{{obj: obj, events: events}}, timeout, restart)
}

// waitMs is a timeout in milliseconds, rounded up and bounded.
func waitMs(d time.Duration) int {
	return int(min((d+time.Millisecond-1)/time.Millisecond, maxPollWaitMs))
}

const maxPollWaitMs = 1 << 30
