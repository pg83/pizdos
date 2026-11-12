//go:build linux && amd64

package main

import (
	"encoding/binary"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// An epoll is a list of files of the kernel. epoll_wait asks each member
// whether it is ready and, when none is, selects on the changes of them
// all, as every wait of the kernel does: a member is a file like any
// other, with no subscription of its own. A level-triggered member is
// reported while it is ready, an edge-triggered one once a change of its
// file, a one-shot one once until EPOLL_CTL_MOD; the next epoll_wait goes
// on from where the last stopped, so every member gets its turn. A file
// that is ready whenever asked and never changes, a regular file, cannot
// be a member: EPERM, as on Linux. A set can be a member of a set: its
// changes are its members'. The epoll keeps a reference to a member's
// file, so a member is removed before its last close, or stays until the
// epoll's.
//
// The guest's struct epoll_event is 16 bytes: events u32, data u64 at 8.

const (
	epollCtlAdd    = 1
	epollCtlDel    = 2
	epollCtlMod    = 3
	epollOneshot   = 1 << 30
	epollEt        = 1 << 31
	epollEventSize = 16
	epollMaxEvents = 1 << 20
)

// An Epoll is the role epoll_ctl and epoll_wait ask of their descriptor.
type Epoll interface {
	File
	ctl(op uint32, key epollKey, events uint32, data uint64) error
	take(buf []byte, max int) int
}

type epoll struct {
	mu      sync.Mutex
	members map[epollKey]*epollMember
	// order is the members as added, next where the next epoll_wait
	// starts among them.
	order []*epollMember
	next  int
	refs  atomic.Int32
	ino   uint64
	made  time.Time
}

// An epollKey names a member as Linux does: by the descriptor it was
// added with and the file.
type epollKey struct {
	fd  uint32
	obj File
}

type epollMember struct {
	key    epollKey
	events uint32
	data   uint64
	// last is the changes of the file when it was last reported: an
	// edge-triggered member with the same ones, unclosed, has not changed
	// and is not reported again. disabled is a one-shot member reported.
	last     []<-chan struct{}
	disabled bool
}

var (
	_ Epoll    = (*epoll)(nil)
	_ Transfer = (*epoll)(nil)
)

var epollInodes atomic.Uint64

func newEpoll() *epoll {
	ep := &epoll{members: map[epollKey]*epollMember{}, ino: epollInodes.Add(1), made: time.Now()}
	ep.refs.Store(1)

	return ep
}

func (ep *epoll) ref() {
	ep.refs.Add(1)
}

// unref on the last reference lets every member's file go.
func (ep *epoll) unref() {
	if ep.refs.Add(-1) != 0 {
		return
	}

	ep.mu.Lock()
	members := ep.order
	ep.members, ep.order = map[epollKey]*epollMember{}, nil
	ep.mu.Unlock()

	for _, m := range members {
		m.key.obj.unref()
	}
}

// stat is an anonymous inode's: no type, as Linux has it.
func (ep *epoll) stat(st *syscall.Stat_t) error {
	*st = syscall.Stat_t{Ino: ep.ino, Mode: 0o600, Nlink: 1, Uid: uint32(syscall.Getuid()), Gid: uint32(syscall.Getgid()), Blksize: 4096}
	st.Atim = syscall.NsecToTimespec(ep.made.UnixNano())
	st.Mtim, st.Ctim = st.Atim, st.Atim

	return nil
}

// poll: the epoll is readable when a member would be reported.
func (ep *epoll) poll(events int16) int16 {
	if ep.take(nil, 1) > 0 {
		return (pollIn | pollRdnorm) & events
	}

	return 0
}

// changes are the members'.
func (ep *epoll) changes(events int16) []<-chan struct{} {
	var all []<-chan struct{}

	for _, m := range ep.snapshot() {
		if !m.disabled {
			all = append(all, m.key.obj.changes(int16(m.events))...)
		}
	}

	return all
}

func (ep *epoll) flags() (int, error) {
	return syscall.O_RDWR, nil
}

func (ep *epoll) setFlags(fl int) error {
	return nil
}

// The Transfer role, to answer EINVAL as Linux does: an epoll has no data.
func (ep *epoll) read(p *Process, b []byte, mode ioMode) (int, error) {
	return 0, syscall.EINVAL
}

func (ep *epoll) write(p *Process, b []byte, mode ioMode) (int, error) {
	return 0, syscall.EINVAL
}

func (ep *epoll) pread(p *Process, b []byte, off int64) (int, error) {
	return 0, syscall.EINVAL
}

func (ep *epoll) pwrite(p *Process, b []byte, off int64) (int, error) {
	return 0, syscall.EINVAL
}

// snapshot is the members in order, starting from next.
func (ep *epoll) snapshot() []*epollMember {
	ep.mu.Lock()
	defer ep.mu.Unlock()

	if len(ep.order) == 0 {
		return nil
	}

	at := ep.next % len(ep.order)

	return append(append([]*epollMember(nil), ep.order[at:]...), ep.order[:at]...)
}

// contains tells whether an epoll is a member of the epoll, at any depth:
// a loop would wake forever.
func (ep *epoll) contains(inner *epoll) bool {
	for _, m := range ep.snapshot() {
		if e, ok := m.key.obj.(*epoll); ok && (e == inner || e.contains(inner)) {
			return true
		}
	}

	return false
}

func (ep *epoll) ctl(op uint32, key epollKey, events uint32, data uint64) error {
	switch op {
	case epollCtlAdd:
		return ep.add(key, events, data)
	case epollCtlMod:
		ep.mu.Lock()
		defer ep.mu.Unlock()
		m := ep.members[key]

		if m == nil {
			return syscall.ENOENT
		}

		m.events, m.data, m.disabled, m.last = events, data, false, nil

		return nil
	case epollCtlDel:
		ep.mu.Lock()
		m := ep.members[key]

		if m == nil {
			ep.mu.Unlock()

			return syscall.ENOENT
		}

		delete(ep.members, key)

		for i, o := range ep.order {
			if o == m {
				ep.order = append(ep.order[:i], ep.order[i+1:]...)

				break
			}
		}

		ep.mu.Unlock()
		key.obj.unref()

		return nil
	}

	return syscall.EINVAL
}

func (ep *epoll) add(key epollKey, events uint32, data uint64) error {
	if inner, ok := key.obj.(*epoll); ok && (inner == ep || inner.contains(ep)) {
		return syscall.ELOOP
	}

	if len(key.obj.changes(int16(events))) == 0 {
		return syscall.EPERM
	}

	ep.mu.Lock()
	defer ep.mu.Unlock()

	if _, dup := ep.members[key]; dup {
		return syscall.EEXIST
	}

	key.obj.ref()
	m := &epollMember{key: key, events: events, data: data}
	ep.members[key] = m
	ep.order = append(ep.order, m)

	return nil
}

// changedSince tells whether a file changed since it was reported with
// its changes last: other channels, or a closed one.
func changedSince(last, now []<-chan struct{}) bool {
	if len(last) != len(now) {
		return true
	}

	for i := range now {
		if now[i] != last[i] || closed(now[i]) {
			return true
		}
	}

	return false
}

// take reports up to max members that are ready into a guest's
// epoll_event array, or only counts them without an array, and tells how
// many; the next take starts after the last reported.
func (ep *epoll) take(buf []byte, max int) int {
	members := ep.snapshot()
	n := 0

	for i, m := range members {
		ep.mu.Lock()
		events, disabled, last := m.events, m.disabled, m.last
		ep.mu.Unlock()

		if disabled {
			continue
		}

		revents := m.key.obj.poll(int16(events))

		if revents == 0 {
			continue
		}

		var now []<-chan struct{}

		if events&epollEt != 0 {
			now = m.key.obj.changes(int16(events))

			if !changedSince(last, now) {
				continue
			}
		}

		if buf == nil {
			return 1
		}

		binary.LittleEndian.PutUint32(buf[epollEventSize*n:], uint32(uint16(revents)))
		binary.LittleEndian.PutUint64(buf[epollEventSize*n+8:], m.data)
		n++
		ep.mu.Lock()
		m.last = now
		m.disabled = events&epollOneshot != 0
		ep.mu.Unlock()

		if n == max {
			ep.mu.Lock()
			ep.next += i + 1
			ep.mu.Unlock()

			break
		}
	}

	return n
}

// epollAt is the Epoll of a descriptor: another file is EINVAL.
func (p *Process) epollAt(fd uint32) (Epoll, error) {
	obj, err := p.fd(fd)

	if err != nil {
		return nil, err
	}

	if ep, ok := obj.(Epoll); ok {
		return ep, nil
	}

	return nil, syscall.EINVAL
}

func (p *Process) libcEpoll_create(size uint32) uint32 {
	if int32(size) <= 0 {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(p.install(newEpoll(), 0, false))
}

func (p *Process) libcEpoll_create1(flags uint32) uint32 {
	if flags&^syscall.O_CLOEXEC != 0 {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(p.install(newEpoll(), 0, flags&syscall.O_CLOEXEC != 0))
}

func (p *Process) libcEpoll_ctl(epfd, op, fd, at uint32) uint32 {
	ep, err := p.epollAt(epfd)

	if err != nil {
		return p.result(0, err)
	}

	obj, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	if fd == epfd {
		return p.result(0, syscall.EINVAL)
	}

	var events uint32
	var data uint64

	if op != epollCtlDel {
		if at == 0 {
			return p.result(0, syscall.EFAULT)
		}

		b := p.span(at, epollEventSize)
		events, data = binary.LittleEndian.Uint32(b), binary.LittleEndian.Uint64(b[8:])
	}

	return p.result(0, ep.ctl(op, epollKey{fd, obj}, events, data))
}

// epollWait waits for up to max ready members, for timeout milliseconds,
// forever when negative; a signal interrupts it, restarted or not.
func (p *Process) epollWait(epfd, at, max uint32, timeout int) (int, error) {
	if int32(max) <= 0 || max > epollMaxEvents {
		return 0, syscall.EINVAL
	}

	ep, err := p.epollAt(epfd)

	if err != nil {
		return 0, err
	}

	end := deadline(timeout)

	for {
		if n := ep.take(p.span(at, max*epollEventSize), int(max)); n > 0 {
			return n, nil
		}

		wait := -1

		if !end.IsZero() {
			left := time.Until(end)

			if left <= 0 {
				return 0, nil
			}

			wait = waitMs(left)
		}

		ready, err := p.waitObject(ep, pollIn, wait, false)

		if err != nil {
			return 0, err
		}

		if ready == 0 {
			return 0, nil
		}
	}
}

func (p *Process) libcEpoll_wait(epfd, at, max, timeout uint32) uint32 {
	return p.result(p.epollWait(epfd, at, max, int(int32(timeout))))
}

func (p *Process) libcEpoll_pwait(epfd, at, max, timeout, mask uint32) uint32 {
	return p.result(p.withMask(mask, func() (int, error) {
		return p.epollWait(epfd, at, max, int(int32(timeout)))
	}))
}
