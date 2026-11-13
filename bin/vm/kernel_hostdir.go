//go:build linux && amd64

package main

import (
	"strings"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// A directory of the host walks a path one name at a time with the
// host's calls, as the host would: each name opened O_PATH from the
// directory before it by openat2, which follows no link and leaves no
// directory, a link read and its target walked in its place by the
// kernel, an absolute one from the root of the process, and ".." the
// host's, which goes out of a mount of the host as the host does, but
// nowhere from the root of the process. A call on a name is the host's
// call on the directory's descriptor and the name, following nothing.

const (
	oPath             = 0o10000000
	sysOpenat2        = 437
	sysFaccessat2     = 439
	sysFchmodat2      = 452
	resolveNoSymlinks = 0x04
	resolveBeneath    = 0x08
	atFdcwd           = -100
	atSymlinkNofollow = 0x100
	atRemovedir       = 0x200
	atEaccess         = 0x200
	atSymlinkFollow   = 0x400
	atEmptyPath       = 0x1000
	sIfifo            = 0o10000
)

type openHow struct {
	flags, mode, resolve uint64
}

// hostDevices are the host's devices every host root has, with its
// pseudoterminals: no other path leaves it.
var hostDevices = map[string]bool{
	"/dev/null": true, "/dev/zero": true, "/dev/full": true, "/dev/random": true, "/dev/urandom": true, "/dev/tty": true,
	"/dev/ptmx": true,
}

func hostDevice(path string) bool {
	return hostDevices[path] || strings.HasPrefix(path, "/dev/pts/") && len(path) > len("/dev/pts/") && !strings.Contains(path[len("/dev/pts/"):], "/")
}

// openAt2 opens a name of a directory of the host, or a path of the
// host's own from AT_FDCWD; the descriptor is close-on-exec.
func openAt2(dirfd int, name string, flags int, mode uint32, resolve uint64) (int, error) {
	if chaosFail("openat") {
		return -1, syscall.EIO
	}

	path := throw2(syscall.BytePtrFromString(name))
	how := openHow{flags: uint64(flags | syscall.O_CLOEXEC), mode: uint64(mode), resolve: resolve}

	for {
		r, _, e := syscall.Syscall6(sysOpenat2, uintptr(dirfd), uintptr(unsafe.Pointer(path)), uintptr(unsafe.Pointer(&how)), unsafe.Sizeof(how), 0, 0)

		if e == syscall.EINTR || e == syscall.EAGAIN {
			continue
		}

		if e != 0 {
			return -1, e
		}

		return int(r), nil
	}
}

// openNamed opens a name of a directory without following it, "" the
// directory itself.
func openNamed(dirfd int, name string, flags int, mode uint32) (int, error) {
	if name == "" {
		name = "."
	}

	return openAt2(dirfd, name, flags|syscall.O_NOFOLLOW, mode, resolveBeneath|resolveNoSymlinks)
}

// utimensat sets the times of path in dirfd, or of dirfd for a nil path.
func utimensat(dirfd int, name *byte, ts []syscall.Timespec, flags int) error {
	var times uintptr

	if ts != nil {
		times = uintptr(unsafe.Pointer(&ts[0]))
	}

	if _, _, e := syscall.Syscall6(syscall.SYS_UTIMENSAT, uintptr(dirfd), uintptr(unsafe.Pointer(name)), times, uintptr(flags), 0, 0); e != 0 {
		return e
	}

	return nil
}

func atCall(trap uintptr, dirfd int, path string, args ...uintptr) error {
	name := throw2(syscall.BytePtrFromString(path))
	a := [4]uintptr{}
	copy(a[:], args)

	if _, _, e := syscall.Syscall6(trap, uintptr(dirfd), uintptr(unsafe.Pointer(name)), a[0], a[1], a[2], a[3]); e != 0 {
		return e
	}

	return nil
}

// access checks a descriptor's file for mode, as the real ids do or, with
// AT_EACCESS, the effective ones.
func access(fd int, mode, flags uint32) error {
	return atCall(sysFaccessat2, fd, "", uintptr(mode), uintptr(atEmptyPath|flags&atEaccess))
}

func readlinkFd(fd int, out []byte, named bool) (int, error) {
	var buf uintptr

	if len(out) > 0 {
		buf = uintptr(unsafe.Pointer(&out[0]))
	}

	empty := [1]byte{}
	r, _, e := syscall.Syscall6(syscall.SYS_READLINKAT, uintptr(fd), uintptr(unsafe.Pointer(&empty[0])), buf, uintptr(len(out)), 0, 0)

	// Linux reports ENOENT for a non-symlink with an empty path. The
	// original, named request reports EINVAL instead.
	if e == syscall.ENOENT && named {
		return 0, syscall.EINVAL
	}

	if e != 0 {
		return 0, e
	}

	return int(r), nil
}

// emptyIf is AT_EMPTY_PATH for "", the directory itself.
func emptyIf(name string) uintptr {
	if name == "" {
		return atEmptyPath
	}

	return 0
}

// up is where ".." goes from a directory of the host: nowhere from the
// root of the process, of device dev and inode ino, else the host's
// parent; -1 for nowhere.
func up(cur int, dev, ino uint64) (int, error) {
	var st syscall.Stat_t

	if err := syscall.Fstat(cur, &st); err != nil {
		return -1, err
	}

	if st.Dev == dev && st.Ino == ino {
		return -1, nil
	}

	return openAt2(cur, "..", oPath|syscall.O_DIRECTORY, 0, 0)
}

// The Dir role.

// resolve walks a path from the directory to the directory of its last
// name, one name at a time; an absolute link sends the walk on from the
// root of the process.
func (d *hostDir) resolve(p *Process, path string, w *walk) (step, error) {
	names, slash := splitPath(path)
	dev, ino := identity(w.root)
	cur, owned := d.fd, false

	drop := func() {
		if owned {
			syscall.Close(cur)
		}
	}

	move := func(fd int) {
		drop()
		cur, owned = fd, true
	}

	fail := func(err error) (step, error) {
		drop()

		return step{}, err
	}

	for len(names) > 1 {
		name := names[0]
		names = names[1:]

		if name == "." {
			continue
		}

		if name == ".." {
			fd, err := up(cur, dev, ino)

			if err != nil {
				return fail(err)
			}

			if fd >= 0 {
				move(fd)
			}

			continue
		}

		fd, err := openAt2(cur, name, oPath|syscall.O_NOFOLLOW, 0, resolveBeneath|resolveNoSymlinks)

		if err != nil {
			return fail(err)
		}

		var st syscall.Stat_t

		if err := syscall.Fstat(fd, &st); err != nil {
			syscall.Close(fd)

			return fail(err)
		}

		if st.Mode&syscall.S_IFMT != syscall.S_IFLNK {
			if st.Mode&syscall.S_IFMT != syscall.S_IFDIR {
				syscall.Close(fd)

				return fail(syscall.ENOTDIR)
			}

			move(fd)

			continue
		}

		if w.links >= maxLinks {
			syscall.Close(fd)

			return fail(syscall.ELOOP)
		}

		w.links++
		buf := make([]byte, 4096)
		n, err := readlinkFd(fd, buf, true)
		syscall.Close(fd)

		if err == nil && n == 0 {
			err = syscall.ENOENT
		}

		if err != nil {
			return fail(err)
		}

		target := string(buf[:n])
		more, _ := splitPath(target)
		names = append(more, names...)

		if target[0] == '/' {
			drop()
			rest := strings.Join(names, "/")

			if slash {
				rest += "/"
			}

			w.root.ref()

			return step{dir: w.root, more: true, rest: rest}, nil
		}
	}

	s := step{kind: lastNone}

	if len(names) == 1 {
		s.last, s.kind, s.slash = lastOf(names[0], slash)
	}

	if s.kind == lastDotdot {
		fd, err := up(cur, dev, ino)

		if err != nil {
			return fail(err)
		}

		if fd >= 0 {
			move(fd)
		}
	}

	if owned {
		s.dir = newHostDir(cur)
	} else {
		d.ref()
		s.dir = d
	}

	return s, nil
}

// path is the directory's path in the process's root: none for one that
// was removed, or is not under the root.
func (d *hostDir) path(p *Process) (string, error) {
	host, err := d.hostPath()

	if err != nil {
		return "", err
	}

	var st syscall.Stat_t

	if syscall.Fstat(d.fd, &st) == nil && st.Nlink == 0 {
		return "", syscall.ENOENT
	}

	root := p.rootPath

	switch {
	case root == "":
		return "", syscall.ENOENT
	case root == "/":
		return host, nil
	case host == root:
		return "/", nil
	case strings.HasPrefix(host, root+"/"):
		return host[len(root):], nil
	}

	return "", syscall.ENOENT
}

func (d *hostDir) statName(p *Process, name string, st *syscall.Stat_t) error {
	if name == "" {
		return fileStat(d.fd, st)
	}

	if chaosFail("file-stat") {
		return syscall.EIO
	}

	n := throw2(syscall.BytePtrFromString(name))

	if _, _, e := syscall.Syscall6(syscall.SYS_NEWFSTATAT, uintptr(d.fd), uintptr(unsafe.Pointer(n)), uintptr(unsafe.Pointer(st)), atSymlinkNofollow, 0, 0); e != 0 {
		return e
	}

	return nil
}

func (d *hostDir) readlinkName(p *Process, name string) (string, error) {
	if name == "" {
		return "", syscall.EINVAL
	}

	buf := make([]byte, 4096)
	n := throw2(syscall.BytePtrFromString(name))
	r, _, e := syscall.Syscall6(syscall.SYS_READLINKAT, uintptr(d.fd), uintptr(unsafe.Pointer(n)), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)), 0, 0)

	if e != 0 {
		return "", e
	}

	return string(buf[:r]), nil
}

// openName opens a name of the directory, which is never followed: a
// link is ELOOP, or with O_PATH and O_NOFOLLOW the link itself.
func (d *hostDir) openName(p *Process, name string, flags int, mode uint32) (File, error) {
	fd, err := openNamed(d.fd, name, flags, mode)

	// With O_DIRECTORY the host says ENOTDIR of a link it does not follow:
	// a link is still the caller's to follow.
	if err == syscall.ENOTDIR && flags&syscall.O_NOFOLLOW == 0 {
		var st syscall.Stat_t

		if d.statName(p, name, &st) == nil && st.Mode&syscall.S_IFMT == syscall.S_IFLNK {
			err = syscall.ELOOP
		}
	}

	if err != nil {
		return nil, err
	}

	if flags&oPath != 0 && flags&syscall.O_NOFOLLOW == 0 {
		var st syscall.Stat_t

		if syscall.Fstat(fd, &st) == nil && st.Mode&syscall.S_IFMT == syscall.S_IFLNK {
			syscall.Close(fd)

			return nil, syscall.ELOOP
		}
	}

	return newHostObject(fd, true), nil
}

func (d *hostDir) mkdir(p *Process, name string, mode uint32) error {
	return syscall.Mkdirat(d.fd, name, mode)
}

func (d *hostDir) mknod(p *Process, name string, mode uint32, dev uint64) error {
	return syscall.Mknodat(d.fd, name, mode, int(dev))
}

func (d *hostDir) symlink(p *Process, target, name string) error {
	t := throw2(syscall.BytePtrFromString(target))
	n := throw2(syscall.BytePtrFromString(name))

	if _, _, e := syscall.Syscall(syscall.SYS_SYMLINKAT, uintptr(unsafe.Pointer(t)), uintptr(d.fd), uintptr(unsafe.Pointer(n))); e != 0 {
		return e
	}

	return nil
}

func (d *hostDir) unlink(p *Process, name string, dir bool) error {
	flags := 0

	if dir {
		flags = atRemovedir
	}

	return unlinkat(d.fd, name, flags)
}

func (d *hostDir) rename(p *Process, name string, to Dir, toName string) error {
	other, ok := to.(*hostDir)

	if !ok {
		return syscall.EXDEV
	}

	return syscall.Renameat(d.fd, name, other.fd, toName)
}

func (d *hostDir) link(p *Process, name string, to Dir, toName string) error {
	other, ok := to.(*hostDir)

	if !ok {
		return syscall.EXDEV
	}

	n := throw2(syscall.BytePtrFromString(toName))

	return atCall(syscall.SYS_LINKAT, d.fd, name, uintptr(other.fd), uintptr(unsafe.Pointer(n)), 0)
}

func (d *hostDir) accessName(p *Process, name string, mode, flags uint32) error {
	return atCall(sysFaccessat2, d.fd, name, uintptr(mode), uintptr(flags&atEaccess)|atSymlinkNofollow|emptyIf(name))
}

func (d *hostDir) chmodName(p *Process, name string, mode uint32) error {
	return atCall(sysFchmodat2, d.fd, name, uintptr(mode), atSymlinkNofollow|emptyIf(name))
}

func (d *hostDir) chownName(p *Process, name string, uid, gid int) error {
	return syscall.Fchownat(d.fd, name, uid, gid, atSymlinkNofollow|int(emptyIf(name)))
}

// utimesName sets the times of a name, a link itself; the directory's
// own through /proc, where an O_PATH descriptor's times are set.
func (d *hostDir) utimesName(p *Process, name string, ts []syscall.Timespec) error {
	if name == "" {
		return utimensat(atFdcwd, throw2(syscall.BytePtrFromString(procFd(d.fd))), ts, 0)
	}

	return utimensat(d.fd, throw2(syscall.BytePtrFromString(name)), ts, atSymlinkNofollow)
}

func (d *hostDir) truncateName(p *Process, name string, size int64) error {
	fd, err := openNamed(d.fd, name, oPath, 0)

	if err != nil {
		return err
	}

	defer syscall.Close(fd)

	return syscall.Truncate(procFd(fd), size)
}

func (d *hostDir) statfsName(p *Process, name string, st *syscall.Statfs_t) error {
	fd, err := openNamed(d.fd, name, oPath, 0)

	if err != nil {
		return err
	}

	defer syscall.Close(fd)

	return syscall.Fstatfs(fd, st)
}

// deviceDir is a directory of the host's devices a root of the host has
// (hostDevices): it holds no descriptor, and a call on a device is the
// host's call on the device's path. Its path is the guest's too.
type deviceDir struct {
	dir  string
	refs atomic.Int32
}

func newDeviceDir(dir string) *deviceDir {
	d := &deviceDir{dir: dir}
	d.refs.Store(1)

	return d
}

func (d *deviceDir) ref()   { d.refs.Add(1) }
func (d *deviceDir) unref() { d.refs.Add(-1) }

func (d *deviceDir) stat(st *syscall.Stat_t) error {
	return syscall.Stat(d.dir, st)
}

func (d *deviceDir) poll(events int16) int16 {
	return events & (pollIn | pollOut | pollRdnorm | pollWrnorm)
}

func (d *deviceDir) changes(events int16) []<-chan struct{} { return nil }
func (d *deviceDir) flags() (int, error)                    { return syscall.O_RDONLY, nil }
func (d *deviceDir) setFlags(fl int) error                  { return nil }
func (d *deviceDir) readdir(b []byte) (int, error)          { return 0, nil }

func (d *deviceDir) resolve(p *Process, path string, w *walk) (step, error) {
	return step{}, syscall.ENOTDIR
}

func (d *deviceDir) path(p *Process) (string, error) {
	return strings.TrimSuffix(d.dir, "/"), nil
}

// at is the host's path of a device of the directory.
func (d *deviceDir) at(name string) string {
	return d.dir + name
}

func (d *deviceDir) statName(p *Process, name string, st *syscall.Stat_t) error {
	if chaosFail("file-stat") {
		return syscall.EIO
	}

	return syscall.Lstat(d.at(name), st)
}

func (d *deviceDir) readlinkName(p *Process, name string) (string, error) {
	return "", syscall.EINVAL
}

func (d *deviceDir) openName(p *Process, name string, flags int, mode uint32) (File, error) {
	fd, err := openAt2(atFdcwd, d.at(name), flags|syscall.O_NOFOLLOW, mode, 0)

	if err != nil {
		return nil, err
	}

	return newHostObject(fd, true), nil
}

func (d *deviceDir) mkdir(p *Process, name string, mode uint32) error             { return syscall.EEXIST }
func (d *deviceDir) mknod(p *Process, name string, mode uint32, dev uint64) error { return syscall.EEXIST }
func (d *deviceDir) symlink(p *Process, target, name string) error                { return syscall.EEXIST }
func (d *deviceDir) unlink(p *Process, name string, dir bool) error               { return syscall.EPERM }
func (d *deviceDir) rename(p *Process, name string, to Dir, toName string) error  { return syscall.EXDEV }
func (d *deviceDir) link(p *Process, name string, to Dir, toName string) error    { return syscall.EXDEV }

func (d *deviceDir) accessName(p *Process, name string, mode, flags uint32) error {
	return atCall(sysFaccessat2, atFdcwd, d.at(name), uintptr(mode), uintptr(flags&atEaccess)|atSymlinkNofollow)
}

func (d *deviceDir) chmodName(p *Process, name string, mode uint32) error {
	return atCall(sysFchmodat2, atFdcwd, d.at(name), uintptr(mode), atSymlinkNofollow)
}

func (d *deviceDir) chownName(p *Process, name string, uid, gid int) error {
	return syscall.Fchownat(atFdcwd, d.at(name), uid, gid, atSymlinkNofollow)
}

func (d *deviceDir) utimesName(p *Process, name string, ts []syscall.Timespec) error {
	return utimensat(atFdcwd, throw2(syscall.BytePtrFromString(d.at(name))), ts, atSymlinkNofollow)
}

func (d *deviceDir) truncateName(p *Process, name string, size int64) error {
	return syscall.Truncate(d.at(name), size)
}

func (d *deviceDir) statfsName(p *Process, name string, st *syscall.Statfs_t) error {
	return syscall.Statfs(d.at(name), st)
}
