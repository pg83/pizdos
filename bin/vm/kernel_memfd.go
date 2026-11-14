//go:build linux && amd64

package main

import (
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// memfdFile is the file memfd_create makes of a range of a process's
// memory: its bytes are that memory's. Whatever process the descriptor
// reaches reads and writes them, a read straight into its own memory and
// a write straight from it, with no copy between. The access mode is the
// creator's grant. The file holds its instance's region, which stays
// mapped while the file lives (Instance.unhold).
type memfdFile struct {
	inst *Instance
	mem  []byte
	mode int
	refs atomic.Int32
	// mu guards off, the position read, write and seek share.
	mu   sync.Mutex
	off  int64
	ino  uint64
	made time.Time
}

var (
	_ Transfer = (*memfdFile)(nil)
	_ Seeker   = (*memfdFile)(nil)
)

var memfdInodes atomic.Uint64

// memfd_create makes a file of the n bytes of the process's memory at at,
// open in the access mode of flags, close-on-exec with O_CLOEXEC.
func (p *Process) libcMemfd_create(at, n, flags uint32) uint32 {
	mode := int(flags & syscall.O_ACCMODE)

	if flags&^(syscall.O_ACCMODE|syscall.O_CLOEXEC) != 0 || mode == syscall.O_ACCMODE {
		return p.result(0, syscall.EINVAL)
	}

	i := p.inst

	if i.memory == nil || uint64(at)+uint64(n) > i.memory.pages*pageSize {
		return p.result(0, syscall.EFAULT)
	}

	f := newMemfd(i, i.memory.data[at:at+n:at+n], mode)
	fd, err := p.install(f, 0, flags&syscall.O_CLOEXEC != 0)

	if err != nil {
		f.unref()

		return p.result(0, err)
	}

	return p.result(fd, nil)
}

// newMemfd is the memfd of mem in the access mode mode: of a range of
// inst's memory, which it holds, or with inst nil of the kernel's.
func newMemfd(inst *Instance, mem []byte, mode int) *memfdFile {
	f := &memfdFile{inst: inst, mem: mem, mode: mode, ino: memfdInodes.Add(1), made: time.Now()}
	f.refs.Store(1)

	if inst != nil {
		inst.holds.Add(1)
	}

	return f
}

// window is the memfd a READ or WRITE of the file protocol brings
// (kernel_remote.go): b, where the process's read puts the bytes or its
// write takes them, for the server to write or read itself.
func window(p *Process, b []byte, mode int) *memfdFile {
	var inst *Instance

	if p != nil {
		inst = p.inst
	}

	return newMemfd(inst, b, mode)
}

func (f *memfdFile) ref() {
	f.refs.Add(1)
}

func (f *memfdFile) unref() {
	if f.refs.Add(-1) == 0 && f.inst != nil {
		f.inst.unhold()
	}
}

func (f *memfdFile) stat(st *syscall.Stat_t) error {
	*st = syscall.Stat_t{Ino: f.ino, Mode: syscall.S_IFREG | 0o600, Nlink: 1, Size: int64(len(f.mem)),
		Uid: uint32(syscall.Getuid()), Gid: uint32(syscall.Getgid()), Blksize: 4096}
	st.Atim = syscall.NsecToTimespec(f.made.UnixNano())
	st.Mtim, st.Ctim = st.Atim, st.Atim

	return nil
}

// poll: memory is always ready, as a regular file is.
func (f *memfdFile) poll(events int16) int16 {
	return events & (pollIn | pollRdnorm | pollOut | pollWrnorm)
}

func (f *memfdFile) changes(events int16) []<-chan struct{} {
	return nil
}

func (f *memfdFile) flags() (int, error) {
	return f.mode, nil
}

func (f *memfdFile) setFlags(fl int) error {
	return nil
}

func (f *memfdFile) read(p *Process, b []byte, mode ioMode) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()

	n, err := f.pread(p, b, f.off)
	f.off += int64(n)

	return n, err
}

func (f *memfdFile) write(p *Process, b []byte, mode ioMode) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()

	n, err := f.pwrite(p, b, f.off)
	f.off += int64(n)

	return n, err
}

// pread copies the range's bytes at off into b, the reader's memory, with
// the memmove a program's memcpy runs.
func (f *memfdFile) pread(p *Process, b []byte, off int64) (int, error) {
	switch {
	case f.mode == syscall.O_WRONLY:
		return 0, syscall.EBADF
	case off < 0:
		return 0, syscall.EINVAL
	case off >= int64(len(f.mem)):
		return 0, nil
	}

	n := min(len(b), len(f.mem)-int(off))
	moveMemory(b, f.mem[off:int(off)+n])

	return n, nil
}

// pwrite copies b, the writer's memory, into the range at off, as pread
// does; the range does not grow.
func (f *memfdFile) pwrite(p *Process, b []byte, off int64) (int, error) {
	switch {
	case f.mode == syscall.O_RDONLY:
		return 0, syscall.EBADF
	case off < 0:
		return 0, syscall.EINVAL
	case len(b) == 0:
		return 0, nil
	case off >= int64(len(f.mem)):
		return 0, syscall.ENOSPC
	}

	n := min(len(b), len(f.mem)-int(off))
	moveMemory(f.mem[off:], b[:n])

	return n, nil
}

func (f *memfdFile) seek(off int64, whence int) (int64, error) {
	f.mu.Lock()
	defer f.mu.Unlock()

	switch whence {
	case 0:
	case 1:
		off += f.off
	case 2:
		off += int64(len(f.mem))
	default:
		return 0, syscall.EINVAL
	}

	if off < 0 {
		return 0, syscall.EINVAL
	}

	f.off = off

	return off, nil
}
