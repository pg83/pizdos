//go:build linux && amd64

package main

import (
	"encoding/binary"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// An epoll is a file of the kernel that watches files of the kernel. It
// is a waker in the queue of every member, as a process in a blocking
// call is: a change of a member notes the member changed and wakes the
// epoll's own waiters, the processes in epoll_wait and whatever polls the
// epoll. epoll_wait looks at the changed members only, never at them all:
// a level-triggered member that is ready stays changed until it is not,
// an edge-triggered one is reported once a change, a one-shot one once
// until EPOLL_CTL_MOD. A file whose readiness the host must be asked for,
// or that is ready whenever asked, cannot be a member: EPERM, as a regular
// file is on Linux. The epoll keeps a reference to a member's file, so a
// member is removed before its last close, or stays until the epoll's.
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
	// changed is the members whose files woke the epoll, in order.
	changed []*epollMember
	waiters waitQueue
	refs    atomic.Int32
	ino     uint64
	made    time.Time
}

// An epollKey names a member as Linux does: by the descriptor it was
// added with and the file.
type epollKey struct {
	fd  uint32
	obj File
}

type epollMember struct {
	ep     *epoll
	key    epollKey
	events uint32
	data   uint64
	entry  pollEntry
	// watch is the member's wait on its file, listed whether it is in
	// changed, disabled whether a one-shot report or its removal ended it.
	watch    waitSet
	listed   bool
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

// unref on the last reference leaves every member's file.
func (ep *epoll) unref() {
	if ep.refs.Add(-1) != 0 {
		return
	}

	ep.mu.Lock()
	members := ep.members
	ep.members = map[epollKey]*epollMember{}
	ep.changed = nil
	ep.mu.Unlock()

	for _, m := range members {
		m.watch.leave()
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

func (ep *epoll) poll(events int16) int16 {
	if ep.scan(0, nil) {
		return (pollIn | pollRdnorm) & events
	}

	return 0
}

func (ep *epoll) watch(w *waitSet, e *pollEntry) {
	w.queued(e, &ep.waiters)
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

// wakeUp is a change of the member's file: the member is changed, and the
// epoll's waiters look.
func (m *epollMember) wakeUp() {
	ep := m.ep
	ep.mu.Lock()

	if !m.listed && !m.disabled {
		m.listed = true
		ep.changed = append(ep.changed, m)
	}

	ep.mu.Unlock()
	ep.waiters.wake()
}

// contains tells whether an epoll is a member of the epoll, at any depth:
// a loop would wake forever.
func (ep *epoll) contains(inner *epoll) bool {
	ep.mu.Lock()
	defer ep.mu.Unlock()

	for key := range ep.members {
		if e, ok := key.obj.(*epoll); ok && (e == inner || e.contains(inner)) {
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
		m := ep.members[key]

		if m == nil {
			ep.mu.Unlock()

			return syscall.ENOENT
		}

		m.events, m.data, m.disabled = events, data, false
		ep.mu.Unlock()
		ep.check(m)

		return nil
	case epollCtlDel:
		ep.mu.Lock()
		m := ep.members[key]

		if m == nil {
			ep.mu.Unlock()

			return syscall.ENOENT
		}

		delete(ep.members, key)
		m.disabled = true
		ep.unlist(m)
		ep.mu.Unlock()
		m.watch.leave()
		key.obj.unref()

		return nil
	}

	return syscall.EINVAL
}

func (ep *epoll) add(key epollKey, events uint32, data uint64) error {
	if inner, ok := key.obj.(*epoll); ok && (inner == ep || inner.contains(ep)) {
		return syscall.ELOOP
	}

	ep.mu.Lock()
	_, dup := ep.members[key]
	ep.mu.Unlock()

	if dup {
		return syscall.EEXIST
	}

	m := &epollMember{ep: ep, key: key, events: events, data: data, entry: pollEntry{obj: key.obj, events: int16(events)}}
	m.watch.by = m
	key.obj.watch(&m.watch, &m.entry)

	if len(m.watch.queues) == 0 || len(m.watch.arms) > 0 {
		m.watch.leave()

		return syscall.EPERM
	}

	ep.mu.Lock()

	if _, dup := ep.members[key]; dup {
		ep.mu.Unlock()
		m.watch.leave()

		return syscall.EEXIST
	}

	key.obj.ref()
	ep.members[key] = m
	ep.mu.Unlock()
	ep.check(m)

	return nil
}

// check notes a member that is ready now, as its file would.
func (ep *epoll) check(m *epollMember) {
	if m.key.obj.poll(int16(m.events)) != 0 {
		m.wakeUp()
	}
}

// unlist takes a member out of changed, under the lock.
func (ep *epoll) unlist(m *epollMember) {
	if !m.listed {
		return
	}

	m.listed = false
	kept := ep.changed[:0]

	for _, c := range ep.changed {
		if c != m {
			kept = append(kept, c)
		}
	}

	ep.changed = kept
}

// take reports up to max ready members into a guest's epoll_event array
// and tells how many.
func (ep *epoll) take(buf []byte, max int) int {
	n := 0
	ep.scan(max, func(m *epollMember, revents int16) {
		binary.LittleEndian.PutUint32(buf[epollEventSize*n:], uint32(uint16(revents)))
		binary.LittleEndian.PutUint64(buf[epollEventSize*n+8:], m.data)
		n++
	})

	return n
}

// scan looks at the changed members, asking their files outside the lock:
// one that is ready goes to report, up to max of them, and tells true;
// one that is not is no longer changed. Without report it only tells.
func (ep *epoll) scan(max int, report func(m *epollMember, revents int16)) bool {
	type look struct {
		m      *epollMember
		events uint32
	}

	ep.mu.Lock()
	list := make([]look, 0, len(ep.changed))

	for _, m := range ep.changed {
		list = append(list, look{m, m.events})

		if report != nil {
			m.listed = false
		}
	}

	if report != nil {
		ep.changed = nil
	}

	ep.mu.Unlock()
	found, fired := false, 0
	// rest is what the call did not look at, again what it reported and
	// stays ready: changed in that order, as Linux keeps it fair.
	var rest, again, once []*epollMember

	for i, l := range list {
		revents := l.m.key.obj.poll(int16(l.events))

		if revents == 0 {
			continue
		}

		found = true

		if report == nil {
			break
		}

		report(l.m, revents)
		fired++

		switch {
		case l.events&epollOneshot != 0:
			once = append(once, l.m)
		case l.events&epollEt != 0:
		default:
			again = append(again, l.m)
		}

		if fired == max {
			for _, l := range list[i+1:] {
				rest = append(rest, l.m)
			}

			break
		}
	}

	if report == nil {
		return found
	}

	ep.mu.Lock()

	for _, m := range once {
		m.disabled = true
		ep.unlist(m)
	}

	for _, m := range append(rest, again...) {
		if !m.listed && !m.disabled && ep.members[m.key] == m {
			m.listed = true
			ep.changed = append(ep.changed, m)
		}
	}

	ep.mu.Unlock()

	return found
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
