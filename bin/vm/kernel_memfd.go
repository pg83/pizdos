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

// copyWindow is copy_file_range between a memfd and another file: the
// memfd's bytes are the memory it is of, so the other file's go straight
// there or come straight from there, one copy. A file of the host is read
// into that memory or written from it by the host, a server's file
// through a READ or WRITE that brings the memfd itself; another memfd's
// bytes are moved. from and to are the offsets given, nil for the files'
// positions, which move.
func copyWindow(src, dst File, from, to *int64, n int) (int, error) {
	if w, ok := dst.(*memfdFile); ok {
		return w.fill(src, from, to, n)
	}

	return src.(*memfdFile).drain(dst, from, to, n)
}

// fill copies up to n bytes of src, at from or its position, into the
// range at to or its position: the range does not grow, past its end the
// copy is ENOSPC. No lock is held while the bytes go: a server that
// writes to the range as it answers waits for nothing.
func (f *memfdFile) fill(src File, from, to *int64, n int) (int, error) {
	if src == File(f) {
		return 0, syscall.EINVAL
	}

	if f.mode == syscall.O_RDONLY {
		return 0, syscall.EBADF
	}

	at := f.position(to)

	switch {
	case at < 0:
		return 0, syscall.EINVAL
	case n == 0:
		return 0, nil
	case at >= int64(len(f.mem)):
		return 0, syscall.ENOSPC
	}

	n = min(n, len(f.mem)-int(at))
	var count int
	var err error

	switch s := src.(type) {
	case *memfdFile:
		count, err = s.moveTo(f.mem[at:int(at)+n], from)
	case *remoteFile:
		count, err = s.readWindow(f, uint32(at), n, from)
	default:
		count, err = hostTransfer(src, f.mem[at:int(at)+n], from, false)
	}

	f.moved(to, count)

	return count, err
}

// drain copies up to n bytes of the range, at from or its position, into
// dst at to or its position.
func (f *memfdFile) drain(dst File, from, to *int64, n int) (int, error) {
	if f.mode == syscall.O_WRONLY {
		return 0, syscall.EBADF
	}

	at := f.position(from)

	switch {
	case at < 0:
		return 0, syscall.EINVAL
	case n == 0 || at >= int64(len(f.mem)):
		return 0, nil
	}

	n = min(n, len(f.mem)-int(at))
	var count int
	var err error

	switch d := dst.(type) {
	case *remoteFile:
		count, err = d.writeWindow(f, uint32(at), n, to)
	default:
		count, err = hostTransfer(dst, f.mem[at:int(at)+n], to, true)
	}

	f.moved(from, count)

	return count, err
}

// moveTo moves the range's bytes at from or its position, which moves,
// into b, another memfd's range.
func (f *memfdFile) moveTo(b []byte, from *int64) (int, error) {
	n, err := f.pread(nil, b, f.position(from))
	f.moved(from, n)

	return n, err
}

// position is where a copy of the range starts: at the offset given, or
// at the range's position.
func (f *memfdFile) position(at *int64) int64 {
	if at != nil {
		return *at
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	return f.off
}

// moved moves the offset given, or the range's position, past count bytes.
func (f *memfdFile) moved(at *int64, count int) {
	if at != nil {
		*at += int64(count)

		return
	}

	f.mu.Lock()
	f.off += int64(count)
	f.mu.Unlock()
}

// readWindow reads n bytes of the server's file at from, or at its
// position, which moves, into the memfd w from its byte woff on.
func (f *remoteFile) readWindow(w *memfdFile, woff uint32, n int, from *int64) (int, error) {
	if from != nil {
		count, err := f.readInto(w, woff, n, *from)
		*from += int64(count)

		return count, err
	}

	count, err := f.readInto(w, woff, n, atomic.LoadInt64(&f.pos))
	atomic.AddInt64(&f.pos, int64(count))

	return count, err
}

// writeWindow writes n bytes of the memfd w, from its byte woff on, into
// the server's file at to, or at its position, which moves. A file open
// to append is EBADF, as Linux has it.
func (f *remoteFile) writeWindow(w *memfdFile, woff uint32, n int, to *int64) (int, error) {
	if f.append {
		return 0, syscall.EBADF
	}

	if to != nil {
		count, _, err := f.writeFrom(w, woff, n, *to, false)
		*to += int64(count)

		return count, err
	}

	count, _, err := f.writeFrom(w, woff, n, atomic.LoadInt64(&f.pos), false)
	atomic.AddInt64(&f.pos, int64(count))

	return count, err
}

// hostTransfer reads a regular file or a block device of the host into b,
// or with write writes b into it, at off or at its position, which moves:
// the host copies between the file and the memory of b. Any other file is
// EINVAL, as copy_file_range has it.
func hostTransfer(f File, b []byte, off *int64, write bool) (int, error) {
	fd, ok := hostFdOf(f)
	var st syscall.Stat_t

	if !ok || syscall.Fstat(fd, &st) != nil || (st.Mode&syscall.S_IFMT != syscall.S_IFREG && st.Mode&syscall.S_IFMT != syscall.S_IFBLK) {
		return 0, syscall.EINVAL
	}

	for {
		var n int
		var err error

		switch {
		case write && off != nil:
			n, err = syscall.Pwrite(fd, b, *off)
		case write:
			n, err = syscall.Write(fd, b)
		case off != nil:
			n, err = syscall.Pread(fd, b, *off)
		default:
			n, err = syscall.Read(fd, b)
		}

		if err == syscall.EINTR {
			continue
		}

		if err != nil {
			return 0, err
		}

		if off != nil {
			*off += int64(n)
		}

		return n, nil
	}
}
