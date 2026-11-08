//go:build linux && amd64

package main

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"os"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

// WASI preview1 over Linux, enough for Go's wasip1 port. Descriptors 0-2 are
// the host's, 3 is the preopened root directory, the rest are opened by
// path_open; the environment is the host's with PWD its working directory,
// so paths resolve as on the host. A function returns an errno and finds
// its memory through the instance.

const (
	wasiOK           = 0
	wasiE2big        = 1
	wasiEacces       = 2
	wasiEbadf        = 8
	wasiEexist       = 20
	wasiEfault       = 21
	wasiEinval       = 28
	wasiEio          = 29
	wasiEisdir       = 31
	wasiEloop        = 32
	wasiEnametoolong = 37
	wasiEnoent       = 44
	wasiEnomem       = 48
	wasiEnospc       = 51
	wasiEnosys       = 52
	wasiEnotdir      = 54
	wasiEnotempty    = 55
	wasiEperm        = 63
	wasiEspipe       = 70
)

var wasiErrnos = map[syscall.Errno]uint32{
	syscall.E2BIG: wasiE2big, syscall.EACCES: wasiEacces, syscall.EBADF: wasiEbadf, syscall.EEXIST: wasiEexist,
	syscall.EFAULT: wasiEfault, syscall.EINVAL: wasiEinval, syscall.EISDIR: wasiEisdir, syscall.ELOOP: wasiEloop,
	syscall.ENAMETOOLONG: wasiEnametoolong, syscall.ENOENT: wasiEnoent, syscall.ENOMEM: wasiEnomem,
	syscall.ENOSPC: wasiEnospc, syscall.ENOSYS: wasiEnosys, syscall.ENOTDIR: wasiEnotdir,
	syscall.ENOTEMPTY: wasiEnotempty, syscall.EPERM: wasiEperm, syscall.ESPIPE: wasiEspipe,
}

func wasiErrno(err error) uint32 {
	var e syscall.Errno

	if err == nil {
		return wasiOK
	}

	if errors.As(err, &e) {
		if w, ok := wasiErrnos[e]; ok {
			return w
		}
	}

	return wasiEio
}

// wasiExit is the panic of proc_exit.
type wasiExit struct {
	code uint32
}

type WASI struct {
	args []string
	env  []string
	fds  map[uint32]int
	next uint32
}

const wasiRoot = 3

func newWASI(args []string) *WASI {
	root := throw2(syscall.Open("/", syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_CLOEXEC, 0))
	w := &WASI{args: args, env: []string{"PWD=" + throw2(os.Getwd())}, fds: map[uint32]int{0: 0, 1: 1, 2: 2, wasiRoot: root}, next: wasiRoot + 1}

	for _, e := range os.Environ() {
		if len(e) < 4 || e[:4] != "PWD=" {
			w.env = append(w.env, e)
		}
	}

	return w
}

func (w *WASI) function(imp *ImportMeta) any {
	if imp.module != "wasi_snapshot_preview1" {
		return nil
	}

	functions := map[string]any{
		"args_get": w.argsGet, "args_sizes_get": w.argsSizesGet, "environ_get": w.environGet,
		"environ_sizes_get": w.environSizesGet, "clock_time_get": w.clockTimeGet, "clock_res_get": w.clockResGet,
		"fd_write": w.fdWrite, "fd_read": w.fdRead, "fd_pwrite": w.fdPwrite, "fd_pread": w.fdPread,
		"fd_close": w.fdClose, "fd_seek": w.fdSeek, "fd_sync": w.fdSync, "fd_fdstat_get": w.fdFdstatGet,
		"fd_fdstat_set_flags": w.fdFdstatSetFlags, "fd_filestat_get": w.fdFilestatGet,
		"fd_filestat_set_size": w.fdFilestatSetSize, "fd_prestat_get": w.fdPrestatGet,
		"fd_prestat_dir_name": w.fdPrestatDirName, "fd_readdir": w.fdReaddir,
		"path_filestat_get": w.pathFilestatGet, "path_open": w.pathOpen,
		"path_create_directory": w.pathCreateDirectory, "path_remove_directory": w.pathRemoveDirectory,
		"path_unlink_file": w.pathUnlinkFile, "path_rename": w.pathRename, "path_readlink": w.pathReadlink,
		"poll_oneoff": w.pollOneoff, "random_get": w.randomGet, "sched_yield": w.schedYield, "proc_exit": w.procExit,
	}

	return functions[imp.name]
}

func (w *WASI) global(*ImportMeta) uintptr {
	return 0
}

func (w *WASI) memory(*ImportMeta) *LinearMemory {
	return nil
}

func (w *WASI) table(*ImportMeta) uintptr {
	return 0
}

func guest(ctx uintptr) []byte {
	return instanceAt(ctx).memoryBytes()
}

// span is n bytes of memory at at, or nil when they do not fit.
func span(m []byte, at uint32, n uint64) []byte {
	if uint64(at)+n > uint64(len(m)) {
		return nil
	}

	return m[at : uint64(at)+n]
}

func put32(m []byte, at uint32, v uint32) bool {
	b := span(m, at, 4)

	if b != nil {
		binary.LittleEndian.PutUint32(b, v)
	}

	return b != nil
}

func put64(m []byte, at uint32, v uint64) bool {
	b := span(m, at, 8)

	if b != nil {
		binary.LittleEndian.PutUint64(b, v)
	}

	return b != nil
}

func (w *WASI) fd(fd uint32) (int, bool) {
	host, ok := w.fds[fd]

	return host, ok
}

func stringsSizes(m []byte, list []string, count, size uint32) uint32 {
	n := 0

	for _, s := range list {
		n += len(s) + 1
	}

	if !put32(m, count, uint32(len(list))) || !put32(m, size, uint32(n)) {
		return wasiEfault
	}

	return wasiOK
}

func stringsGet(m []byte, list []string, ptrs, buf uint32) uint32 {
	for i, s := range list {
		b := span(m, buf, uint64(len(s)+1))

		if b == nil || !put32(m, ptrs+4*uint32(i), buf) {
			return wasiEfault
		}

		copy(b, s)
		b[len(s)] = 0
		buf += uint32(len(s) + 1)
	}

	return wasiOK
}

func (w *WASI) argsGet(ctx uintptr, argv, buf uint32) uint32 {
	return stringsGet(guest(ctx), w.args, argv, buf)
}

func (w *WASI) argsSizesGet(ctx uintptr, count, size uint32) uint32 {
	return stringsSizes(guest(ctx), w.args, count, size)
}

func (w *WASI) environGet(ctx uintptr, env, buf uint32) uint32 {
	return stringsGet(guest(ctx), w.env, env, buf)
}

func (w *WASI) environSizesGet(ctx uintptr, count, size uint32) uint32 {
	return stringsSizes(guest(ctx), w.env, count, size)
}

// clocks: realtime, monotonic, process and thread CPU time
var wasiClocks = []uintptr{0, 1, 2, 3}

func clock(id uint32, res bool) (uint64, uint32) {
	if id >= uint32(len(wasiClocks)) {
		return 0, wasiEinval
	}

	var ts syscall.Timespec
	call := uintptr(syscall.SYS_CLOCK_GETTIME)

	if res {
		call = syscall.SYS_CLOCK_GETRES
	}

	if _, _, e := syscall.Syscall(call, wasiClocks[id], uintptr(unsafe.Pointer(&ts)), 0); e != 0 {
		return 0, wasiErrno(e)
	}

	return uint64(ts.Sec)*1e9 + uint64(ts.Nsec), wasiOK
}

func (w *WASI) clockTimeGet(ctx uintptr, id uint32, precision uint64, at uint32) uint32 {
	t, e := clock(id, false)

	if e == wasiOK && !put64(guest(ctx), at, t) {
		return wasiEfault
	}

	return e
}

func (w *WASI) clockResGet(ctx uintptr, id uint32, at uint32) uint32 {
	t, e := clock(id, true)

	if e == wasiOK && !put64(guest(ctx), at, t) {
		return wasiEfault
	}

	return e
}

// iovecs reads a guest iovec array as host iovecs into its memory, which
// stays where it is during the call.
func iovecs(m []byte, iovs, count uint32) ([]syscall.Iovec, uint32) {
	if count > 1024 {
		return nil, wasiEinval
	}

	table := span(m, iovs, 8*uint64(count))

	if table == nil {
		return nil, wasiEfault
	}

	out := make([]syscall.Iovec, 0, count)

	for i := uint32(0); i < count; i++ {
		base, n := binary.LittleEndian.Uint32(table[8*i:]), binary.LittleEndian.Uint32(table[8*i+4:])
		b := span(m, base, uint64(n))

		if b == nil {
			return nil, wasiEfault
		}

		if n > 0 {
			out = append(out, syscall.Iovec{Base: &b[0], Len: uint64(n)})
		}
	}

	return out, wasiOK
}

// vectored runs readv, writev, preadv or pwritev and stores the count.
func (w *WASI) vectored(ctx uintptr, call uintptr, fd, iovs, count uint32, offset uint64, positioned bool, done uint32) uint32 {
	m := guest(ctx)
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	v, e := iovecs(m, iovs, count)

	if e != wasiOK {
		return e
	}

	if span(m, done, 4) == nil {
		return wasiEfault
	}

	n := uintptr(0)

	if len(v) > 0 {
		var errno syscall.Errno

		if positioned {
			n, _, errno = syscall.Syscall6(call, uintptr(host), uintptr(unsafe.Pointer(&v[0])), uintptr(len(v)), uintptr(offset), 0, 0)
		} else {
			n, _, errno = syscall.Syscall(call, uintptr(host), uintptr(unsafe.Pointer(&v[0])), uintptr(len(v)))
		}

		if errno != 0 {
			return wasiErrno(errno)
		}
	}

	put32(m, done, uint32(n))

	return wasiOK
}

func (w *WASI) fdWrite(ctx uintptr, fd, iovs, count, written uint32) uint32 {
	return w.vectored(ctx, syscall.SYS_WRITEV, fd, iovs, count, 0, false, written)
}

func (w *WASI) fdRead(ctx uintptr, fd, iovs, count, read uint32) uint32 {
	return w.vectored(ctx, syscall.SYS_READV, fd, iovs, count, 0, false, read)
}

func (w *WASI) fdPwrite(ctx uintptr, fd, iovs, count uint32, offset uint64, written uint32) uint32 {
	return w.vectored(ctx, syscall.SYS_PWRITEV, fd, iovs, count, offset, true, written)
}

func (w *WASI) fdPread(ctx uintptr, fd, iovs, count uint32, offset uint64, read uint32) uint32 {
	return w.vectored(ctx, syscall.SYS_PREADV, fd, iovs, count, offset, true, read)
}

func (w *WASI) fdClose(ctx uintptr, fd uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	if fd <= wasiRoot {
		return wasiOK
	}

	delete(w.fds, fd)

	return wasiErrno(syscall.Close(host))
}

func (w *WASI) fdSeek(ctx uintptr, fd uint32, offset uint64, whence uint32, result uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	if whence > 2 {
		return wasiEinval
	}

	at, err := syscall.Seek(host, int64(offset), int(whence))

	if err != nil {
		return wasiErrno(err)
	}

	if !put64(guest(ctx), result, uint64(at)) {
		return wasiEfault
	}

	return wasiOK
}

func (w *WASI) fdSync(ctx uintptr, fd uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	return wasiErrno(syscall.Fsync(host))
}

func filetype(mode uint32) byte {
	switch mode & syscall.S_IFMT {
	case syscall.S_IFBLK:
		return 1
	case syscall.S_IFCHR:
		return 2
	case syscall.S_IFDIR:
		return 3
	case syscall.S_IFREG:
		return 4
	case syscall.S_IFSOCK:
		return 6
	case syscall.S_IFLNK:
		return 7
	}

	return 0
}

func fcntl(fd int, cmd, arg int) (int, error) {
	r, _, e := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fd), uintptr(cmd), uintptr(arg))

	if e != 0 {
		return 0, e
	}

	return int(r), nil
}

func (w *WASI) fdFdstatGet(ctx uintptr, fd, at uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	b := span(guest(ctx), at, 24)

	if b == nil {
		return wasiEfault
	}

	var st syscall.Stat_t

	if err := syscall.Fstat(host, &st); err != nil {
		return wasiErrno(err)
	}

	fl, err := fcntl(host, syscall.F_GETFL, 0)

	if err != nil {
		return wasiErrno(err)
	}

	flags := uint16(0)

	if fl&syscall.O_APPEND != 0 {
		flags |= 1
	}

	if fl&syscall.O_NONBLOCK != 0 {
		flags |= 4
	}

	clear(b)
	b[0] = filetype(st.Mode)
	binary.LittleEndian.PutUint16(b[2:], flags)
	binary.LittleEndian.PutUint64(b[8:], ^uint64(0))
	binary.LittleEndian.PutUint64(b[16:], ^uint64(0))

	return wasiOK
}

func (w *WASI) fdFdstatSetFlags(ctx uintptr, fd, flags uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	fl, err := fcntl(host, syscall.F_GETFL, 0)

	if err != nil {
		return wasiErrno(err)
	}

	fl &^= syscall.O_APPEND | syscall.O_NONBLOCK

	if flags&1 != 0 {
		fl |= syscall.O_APPEND
	}

	if flags&4 != 0 {
		fl |= syscall.O_NONBLOCK
	}

	_, err = fcntl(host, syscall.F_SETFL, fl)

	return wasiErrno(err)
}

func nanos(t syscall.Timespec) uint64 {
	return uint64(t.Sec)*1e9 + uint64(t.Nsec)
}

func filestat(b []byte, st *syscall.Stat_t) {
	clear(b)
	binary.LittleEndian.PutUint64(b, st.Dev)
	binary.LittleEndian.PutUint64(b[8:], st.Ino)
	b[16] = filetype(st.Mode)
	binary.LittleEndian.PutUint64(b[24:], st.Nlink)
	binary.LittleEndian.PutUint64(b[32:], uint64(st.Size))
	binary.LittleEndian.PutUint64(b[40:], nanos(st.Atim))
	binary.LittleEndian.PutUint64(b[48:], nanos(st.Mtim))
	binary.LittleEndian.PutUint64(b[56:], nanos(st.Ctim))
}

func (w *WASI) fdFilestatGet(ctx uintptr, fd, at uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	b := span(guest(ctx), at, 64)

	if b == nil {
		return wasiEfault
	}

	var st syscall.Stat_t

	if err := syscall.Fstat(host, &st); err != nil {
		return wasiErrno(err)
	}

	filestat(b, &st)

	return wasiOK
}

func (w *WASI) fdFilestatSetSize(ctx uintptr, fd uint32, size uint64) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	return wasiErrno(syscall.Ftruncate(host, int64(size)))
}

func (w *WASI) fdPrestatGet(ctx uintptr, fd, at uint32) uint32 {
	if fd != wasiRoot {
		return wasiEbadf
	}

	m := guest(ctx)

	if !put32(m, at, 0) || !put32(m, at+4, 1) {
		return wasiEfault
	}

	return wasiOK
}

func (w *WASI) fdPrestatDirName(ctx uintptr, fd, path, n uint32) uint32 {
	if fd != wasiRoot {
		return wasiEbadf
	}

	b := span(guest(ctx), path, uint64(n))

	if b == nil {
		return wasiEfault
	}

	if n < 1 {
		return wasiEinval
	}

	b[0] = '/'

	return wasiOK
}

func direntType(t byte) byte {
	switch t {
	case syscall.DT_BLK:
		return 1
	case syscall.DT_CHR:
		return 2
	case syscall.DT_DIR:
		return 3
	case syscall.DT_REG:
		return 4
	case syscall.DT_LNK:
		return 7
	}

	return 0
}

// fdReaddir reads the directory from its start and writes the entries from
// cookie on, each a 24-byte header and the name, until the buffer is full.
func (w *WASI) fdReaddir(ctx uintptr, fd, buf, n uint32, cookie uint64, used uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	m := guest(ctx)
	out := span(m, buf, uint64(n))

	if out == nil || span(m, used, 4) == nil {
		return wasiEfault
	}

	if _, err := syscall.Seek(host, 0, 0); err != nil {
		return wasiErrno(err)
	}

	index := uint64(0)
	written := 0
	raw := make([]byte, 64<<10)

	for written < len(out) {
		got, err := syscall.Getdents(host, raw)

		if err != nil {
			return wasiErrno(err)
		}

		if got == 0 {
			break
		}

		for at := 0; at < got && written < len(out); {
			reclen := int(binary.LittleEndian.Uint16(raw[at+16:]))
			name := raw[at+19 : at+reclen]

			for k, c := range name {
				if c == 0 {
					name = name[:k]

					break
				}
			}

			if index >= cookie {
				var head [24]byte
				binary.LittleEndian.PutUint64(head[:], index+1)
				binary.LittleEndian.PutUint64(head[8:], binary.LittleEndian.Uint64(raw[at:]))
				binary.LittleEndian.PutUint32(head[16:], uint32(len(name)))
				head[20] = direntType(raw[at+18])
				written += copy(out[written:], head[:])
				written += copy(out[written:], name)
			}

			index++
			at += reclen
		}
	}

	put32(m, used, uint32(written))

	return wasiOK
}

func guestPath(m []byte, path, n uint32) (string, uint32) {
	b := span(m, path, uint64(n))

	if b == nil {
		return "", wasiEfault
	}

	if n >= 4096 {
		return "", wasiEnametoolong
	}

	if n == 0 {
		return ".", wasiOK
	}

	return string(b), wasiOK
}

func (w *WASI) pathFilestatGet(ctx uintptr, fd, flags, path, n, at uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	m := guest(ctx)
	p, e := guestPath(m, path, n)

	if e != wasiOK {
		return e
	}

	b := span(m, at, 64)

	if b == nil {
		return wasiEfault
	}

	follow := 0x100 // AT_SYMLINK_NOFOLLOW

	if flags&1 != 0 {
		follow = 0
	}

	var st syscall.Stat_t

	if err := fstatat(host, p, &st, follow); err != nil {
		return wasiErrno(err)
	}

	filestat(b, &st)

	return wasiOK
}

func (w *WASI) pathOpen(ctx uintptr, fd, dirflags, path, n, oflags uint32, rights, inheriting uint64, fdflags, opened uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	m := guest(ctx)
	p, e := guestPath(m, path, n)

	if e != wasiOK {
		return e
	}

	if span(m, opened, 4) == nil {
		return wasiEfault
	}

	read := rights&(1<<1) != 0 || rights&(1<<14) != 0
	write := rights&(1<<6) != 0 || rights&(1<<22) != 0
	flags := syscall.O_CLOEXEC

	switch {
	case read && write:
		flags |= syscall.O_RDWR
	case write:
		flags |= syscall.O_WRONLY
	default:
		flags |= syscall.O_RDONLY
	}

	for bit, f := range map[uint32]int{1: syscall.O_CREAT, 2: syscall.O_DIRECTORY, 4: syscall.O_EXCL, 8: syscall.O_TRUNC} {
		if oflags&bit != 0 {
			flags |= f
		}
	}

	if fdflags&1 != 0 {
		flags |= syscall.O_APPEND
	}

	if fdflags&4 != 0 {
		flags |= syscall.O_NONBLOCK
	}

	if dirflags&1 == 0 {
		flags |= syscall.O_NOFOLLOW
	}

	r, err := syscall.Openat(host, p, flags, 0o666)

	if err == syscall.EISDIR && write && oflags&1 == 0 {
		r, err = syscall.Openat(host, p, flags&^(syscall.O_WRONLY|syscall.O_RDWR)|syscall.O_RDONLY, 0o666)
	}

	if err != nil {
		return wasiErrno(err)
	}

	guestFd := w.next
	w.next++
	w.fds[guestFd] = r
	put32(m, opened, guestFd)

	return wasiOK
}

func (w *WASI) pathCreateDirectory(ctx uintptr, fd, path, n uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	p, e := guestPath(guest(ctx), path, n)

	if e != wasiOK {
		return e
	}

	return wasiErrno(syscall.Mkdirat(host, p, 0o777))
}

func fstatat(dirfd int, path string, st *syscall.Stat_t, flags int) error {
	p := throw2(syscall.BytePtrFromString(path))

	if _, _, e := syscall.Syscall6(syscall.SYS_NEWFSTATAT, uintptr(dirfd), uintptr(unsafe.Pointer(p)), uintptr(unsafe.Pointer(st)), uintptr(flags), 0, 0); e != 0 {
		return e
	}

	return nil
}

func unlinkat(dirfd int, path string, flags int) error {
	p := throw2(syscall.BytePtrFromString(path))

	if _, _, e := syscall.Syscall(syscall.SYS_UNLINKAT, uintptr(dirfd), uintptr(unsafe.Pointer(p)), uintptr(flags)); e != 0 {
		return e
	}

	return nil
}

func (w *WASI) pathRemoveDirectory(ctx uintptr, fd, path, n uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	p, e := guestPath(guest(ctx), path, n)

	if e != wasiOK {
		return e
	}

	return wasiErrno(unlinkat(host, p, 0x200))
}

func (w *WASI) pathUnlinkFile(ctx uintptr, fd, path, n uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	p, e := guestPath(guest(ctx), path, n)

	if e != wasiOK {
		return e
	}

	return wasiErrno(unlinkat(host, p, 0))
}

func (w *WASI) pathRename(ctx uintptr, fd, path, n, fd2, path2, n2 uint32) uint32 {
	host, ok := w.fd(fd)
	host2, ok2 := w.fd(fd2)

	if !ok || !ok2 {
		return wasiEbadf
	}

	m := guest(ctx)
	p, e := guestPath(m, path, n)

	if e != wasiOK {
		return e
	}

	q, e := guestPath(m, path2, n2)

	if e != wasiOK {
		return e
	}

	return wasiErrno(syscall.Renameat(host, p, host2, q))
}

func (w *WASI) pathReadlink(ctx uintptr, fd, path, n, buf, size, used uint32) uint32 {
	host, ok := w.fd(fd)

	if !ok {
		return wasiEbadf
	}

	m := guest(ctx)
	p, e := guestPath(m, path, n)

	if e != wasiOK {
		return e
	}

	out := span(m, buf, uint64(size))

	if out == nil || span(m, used, 4) == nil {
		return wasiEfault
	}

	name := throw2(syscall.BytePtrFromString(p))
	var target uintptr

	if len(out) > 0 {
		target = uintptr(unsafe.Pointer(&out[0]))
	}

	r, _, errno := syscall.Syscall6(syscall.SYS_READLINKAT, uintptr(host), uintptr(unsafe.Pointer(name)), target, uintptr(len(out)), 0, 0)

	if errno != 0 {
		return wasiErrno(errno)
	}

	put32(m, used, uint32(r))

	return wasiOK
}

// pollOneoff sleeps until the earliest clock subscription; descriptor
// subscriptions are reported ready at once.
func (w *WASI) pollOneoff(ctx uintptr, in, out, count, events uint32) uint32 {
	if count == 0 {
		return wasiEinval
	}

	m := guest(ctx)
	subs := span(m, in, 48*uint64(count))
	evs := span(m, out, 32*uint64(count))

	if subs == nil || evs == nil || span(m, events, 4) == nil {
		return wasiEfault
	}

	ready := uint32(0)
	sleep := int64(-1)
	earliest := uint32(0)

	for i := uint32(0); i < count; i++ {
		s := subs[48*i:]
		tag := s[8]

		if tag != 0 {
			e := evs[32*ready : 32*ready+32]
			clear(e)
			copy(e, s[:8])
			e[10] = tag
			ready++

			continue
		}

		timeout := binary.LittleEndian.Uint64(s[24:])

		if binary.LittleEndian.Uint16(s[40:])&1 != 0 {
			now, errno := clock(binary.LittleEndian.Uint32(s[16:]), false)

			if errno != wasiOK {
				return errno
			}

			timeout = max(timeout, now) - now
		}

		if sleep < 0 || int64(timeout) < sleep {
			sleep = int64(timeout)
			earliest = i
		}
	}

	if ready == 0 && sleep >= 0 {
		time.Sleep(time.Duration(sleep))
		e := evs[:32]
		clear(e)
		copy(e, subs[48*earliest:48*earliest+8])
		ready = 1
	}

	put32(m, events, ready)

	return wasiOK
}

func (w *WASI) randomGet(ctx uintptr, buf, n uint32) uint32 {
	b := span(guest(ctx), buf, uint64(n))

	if b == nil {
		return wasiEfault
	}

	if _, err := rand.Read(b); err != nil {
		return wasiEio
	}

	return wasiOK
}

func (w *WASI) schedYield(ctx uintptr) uint32 {
	runtime.Gosched()

	return wasiOK
}

func (w *WASI) procExit(ctx uintptr, code uint32) {
	panic(&wasiExit{code})
}
