//go:build linux && amd64

package main

import (
	"reflect"
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
	// poll is the events ready now of those asked; changes are the
	// channels that close when the file may become ready for events, a
	// wait selects on them (changed): none for a file ready whenever asked.
	poll(events int16) int16
	changes(events int16) []<-chan struct{}
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

// A changed is the one wake a change gives everyone waiting on an object:
// a channel the waiters select on, which the change closes and the next
// wait replaces. Nobody waiting, nothing is made.
type changed struct {
	mu sync.Mutex
	ch chan struct{}
}

// wait is the channel the next change closes.
func (c *changed) wait() <-chan struct{} {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.ch == nil {
		c.ch = make(chan struct{})
	}

	return c.ch
}

// bump is a change: the waiters wake.
func (c *changed) bump() {
	c.mu.Lock()

	if c.ch != nil {
		close(c.ch)
		c.ch = nil
	}

	c.mu.Unlock()
}

// closed tells whether a channel of wait has closed since.
func closed(ch <-chan struct{}) bool {
	select {
	case <-ch:
		return true
	default:
		return false
	}
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

// waitObjects waits until an object is ready for its events: it asks them
// all, and when none is, takes their changes, asks once more, as a change
// before the wait was armed is not waited for, then selects on the
// changes, the process's wake and the deadline of timeout milliseconds,
// and asks again. A signal interrupts it: then the error is EINTR, after
// the handlers ran when all of them restart calls and restart is set. It
// returns how many are ready.
func (p *Process) waitObjects(entries []pollEntry, timeout int, restart bool) (int, error) {
	end := deadline(timeout)

	for {
		if p.wakeSignal() {
			if restart && p.interrupted() {
				continue
			}

			return 0, syscall.EINTR
		}

		if ready := pollEntries(entries); ready > 0 {
			return ready, nil
		}

		cases := []reflect.SelectCase{recvCase(p.wake)}
		var timer *time.Timer

		if !end.IsZero() {
			d := time.Until(end)

			if d <= 0 {
				return 0, nil
			}

			timer = time.NewTimer(d)
			cases = append(cases, recvCase(timer.C))
		}

		for i := range entries {
			for _, ch := range entries[i].obj.changes(entries[i].events) {
				cases = append(cases, recvCase(ch))
			}
		}

		if ready := pollEntries(entries); ready > 0 {
			if timer != nil {
				timer.Stop()
			}

			return ready, nil
		}

		chosen, _, _ := reflect.Select(cases)

		if timer != nil {
			timer.Stop()

			if chosen == 1 {
				return 0, nil
			}
		}
	}
}

// pollEntries asks every entry's object for its events and counts the
// ready ones.
func pollEntries(entries []pollEntry) int {
	ready := 0

	for i := range entries {
		entries[i].revents = entries[i].obj.poll(entries[i].events)

		if entries[i].revents != 0 {
			ready++
		}
	}

	return ready
}

// recvCase is a receive from any channel, for a select over a list.
func recvCase[T any](ch <-chan T) reflect.SelectCase {
	return reflect.SelectCase{Dir: reflect.SelectRecv, Chan: reflect.ValueOf(ch)}
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
