//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"math/rand/v2"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

// Paths are the guest's: absolute ones start at the process's root,
// relative ones at the working directory, and openat2 keeps both inside
// the root, symbolic links and ".." included. A call on what a path names
// opens it, O_PATH, and works on that descriptor; a call on the name
// itself (unlink, mkdir, rename) opens the parent directory and works on
// the name in it.

const (
	oPath             = 0o10000000
	sysOpenat2        = 437
	sysFaccessat2     = 439
	sysFchmodat2      = 452
	resolveInRoot     = 0x10
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

// full is the path from the root, which openat2 takes relative to it.
func (p *Process) full(path string) (string, error) {
	switch {
	case path == "":
		return "", syscall.ENOENT
	case len(path) >= 4096:
		return "", syscall.ENAMETOOLONG
	case path[0] == '/':
		return path, nil
	}

	return p.cwd + "/" + path, nil
}

// hostDevices are the host's devices every root has, with its
// pseudoterminals: no other path leaves it.
var hostDevices = map[string]bool{
	"/dev/null": true, "/dev/zero": true, "/dev/full": true, "/dev/random": true, "/dev/urandom": true, "/dev/tty": true,
	"/dev/ptmx": true,
}

func hostDevice(path string) bool {
	return hostDevices[path] || strings.HasPrefix(path, "/dev/pts/") && len(path) > len("/dev/pts/") && !strings.Contains(path[len("/dev/pts/"):], "/")
}

// openat2 opens a guest path; the host descriptor is close-on-exec.
func (p *Process) openat2(path string, flags int, mode uint32) (int, error) {
	if chaosFail("openat") {
		return -1, syscall.EIO
	}

	full, err := p.full(path)

	if err != nil {
		return -1, err
	}

	if clean := filepath.Clean(full); hostDevice(clean) {
		for {
			fd, err := syscall.Open(clean, flags|syscall.O_CLOEXEC, mode)

			if err != syscall.EINTR {
				return fd, err
			}
		}
	}

	name := throw2(syscall.BytePtrFromString(full))
	how := openHow{flags: uint64(flags | syscall.O_CLOEXEC), mode: uint64(mode), resolve: resolveInRoot}

	for {
		r, _, e := syscall.Syscall6(sysOpenat2, uintptr(p.root.host()), uintptr(unsafe.Pointer(name)), uintptr(unsafe.Pointer(&how)), unsafe.Sizeof(how), 0, 0)

		if e == syscall.EINTR || e == syscall.EAGAIN {
			continue
		}

		if e != 0 {
			return -1, e
		}

		return int(r), nil
	}
}

// opath opens what a path names, O_PATH: following a last symbolic link,
// or the link itself.
func (p *Process) opath(path string, follow bool) (int, error) {
	flags := oPath

	if !follow {
		flags |= syscall.O_NOFOLLOW
	}

	return p.openat2(path, flags, 0)
}

// parent opens the directory a path's last component is in and returns
// that name. A path naming the root, "." or ".." has no name in a parent:
// the name is "" and the descriptor is the path's own.
func (p *Process) parent(path string) (int, string, error) {
	full, err := p.full(path)

	if err != nil {
		return -1, "", err
	}

	for len(full) > 1 && full[len(full)-1] == '/' {
		full = full[:len(full)-1]
	}

	at := strings.LastIndexByte(full, '/')
	dir, name := full[:at], full[at+1:]

	if dir == "" {
		dir = "/"
	}

	if name == "" || name == "." || name == ".." {
		fd, err := p.openat2(full, oPath, 0)

		return fd, "", err
	}

	fd, err := p.openat2(dir, oPath|syscall.O_DIRECTORY, 0)

	return fd, name, err
}

// onName runs a call on a path's name in its parent directory; on a path
// without one it fails with noName.
func (p *Process) onName(path string, noName syscall.Errno, call func(dirfd int, name string) error) error {
	dirfd, name, err := p.parent(path)

	if err != nil {
		return err
	}

	defer syscall.Close(dirfd)

	if name == "" {
		return noName
	}

	return call(dirfd, name)
}

// onPath runs a call on the O_PATH descriptor of what a path names.
func (p *Process) onPath(path string, follow bool, call func(fd int) error) error {
	fd, err := p.opath(path, follow)

	if err != nil {
		return err
	}

	defer syscall.Close(fd)

	return call(fd)
}

// atPath is the path a call relative to a guest directory descriptor names.
func (p *Process) atPath(dirfd int32, path string) (string, error) {
	if dirfd == atFdcwd || strings.HasPrefix(path, "/") {
		return path, nil
	}

	d, err := p.dirAt(uint32(dirfd))

	if err != nil {
		return "", err
	}

	dir, err := d.hostPath()

	if err != nil {
		return "", err
	}

	return p.guestPath(dir) + "/" + path, nil
}

// atFile is the host descriptor an *at call with AT_EMPTY_PATH and an
// empty path works on: the directory descriptor's own. An object of the
// kernel is no file of the host: EBADF.
func (p *Process) atFile(dirfd int32, path string, flags uint32) (int, bool, error) {
	if path != "" || flags&atEmptyPath == 0 {
		return -1, false, nil
	}

	if dirfd == atFdcwd {
		fd, err := p.openat2(".", oPath, 0)

		return fd, true, err
	}

	obj, err := p.fd(uint32(dirfd))

	if err != nil {
		return -1, true, err
	}

	if obj.host() < 0 {
		return -1, true, syscall.EBADF
	}

	fd, err := dupHigh(obj.host())

	return fd, true, err
}

// onAt runs a call on the O_PATH descriptor of what an *at call names.
func (p *Process) onAt(dirfd int32, path string, flags uint32, call func(fd int) error) error {
	fd, empty, err := p.atFile(dirfd, path, flags)

	if empty {
		if err != nil {
			return err
		}

		defer syscall.Close(fd)

		return call(fd)
	}

	full, err := p.atPath(dirfd, path)

	if err != nil {
		return err
	}

	return p.onPath(full, flags&atSymlinkNofollow == 0, call)
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

// open opens a guest path and installs the file at the lowest descriptor.
func (p *Process) open(path string, flags, va uint32) (int, error) {
	mode := uint32(0)

	if flags&(syscall.O_CREAT|0o20000000) != 0 {
		mode = p.varArg(va, 0) & 0o7777 &^ p.umask
	}

	fd, err := p.openat2(path, int(flags&^syscall.O_CLOEXEC), mode)

	if err != nil {
		return 0, err
	}

	return p.install(newHostObject(fd, true), 0, flags&syscall.O_CLOEXEC != 0)
}

func (p *Process) mkdir(path string, mode uint32) error {
	return p.onName(path, syscall.EEXIST, func(dirfd int, name string) error {
		return syscall.Mkdirat(dirfd, name, mode&0o7777&^p.umask)
	})
}

// unlink removes a name, or with atRemovedir an empty directory.
func (p *Process) unlink(path string, flags int) error {
	noName := syscall.EISDIR

	if flags&atRemovedir != 0 {
		noName = syscall.EBUSY
	}

	return p.onName(path, noName, func(dirfd int, name string) error {
		return unlinkat(dirfd, name, flags)
	})
}

func (p *Process) rename(old, new string) error {
	return p.onName(old, syscall.EBUSY, func(olddir int, oldName string) error {
		return p.onName(new, syscall.EBUSY, func(newdir int, newName string) error {
			return syscall.Renameat(olddir, oldName, newdir, newName)
		})
	})
}

// link makes a new name of old, or with follow of what a symbolic link old
// names.
func (p *Process) link(old, new string, follow bool) error {
	if follow {
		return p.onPath(old, true, func(fd int) error {
			return p.onName(new, syscall.EEXIST, func(newdir int, newName string) error {
				name := throw2(syscall.BytePtrFromString(newName))

				return atCall(syscall.SYS_LINKAT, atFdcwd, procFd(fd), uintptr(newdir), uintptr(unsafe.Pointer(name)), atSymlinkFollow)
			})
		})
	}

	return p.onName(old, syscall.EPERM, func(olddir int, oldName string) error {
		return p.onName(new, syscall.EEXIST, func(newdir int, newName string) error {
			name := throw2(syscall.BytePtrFromString(newName))

			return atCall(syscall.SYS_LINKAT, olddir, oldName, uintptr(newdir), uintptr(unsafe.Pointer(name)), 0)
		})
	})
}

func (p *Process) symlink(target, path string) error {
	t := throw2(syscall.BytePtrFromString(target))

	return p.onName(path, syscall.EEXIST, func(dirfd int, name string) error {
		n := throw2(syscall.BytePtrFromString(name))

		if _, _, e := syscall.Syscall(syscall.SYS_SYMLINKAT, uintptr(unsafe.Pointer(t)), uintptr(dirfd), uintptr(unsafe.Pointer(n))); e != 0 {
			return e
		}

		return nil
	})
}

func (p *Process) mknod(path string, mode uint32, dev uint64) error {
	return p.onName(path, syscall.EEXIST, func(dirfd int, name string) error {
		return syscall.Mknodat(dirfd, name, mode&^(p.umask&0o777), int(dev))
	})
}

// access checks a descriptor's file for mode, as the real ids do or, with
// AT_EACCESS, the effective ones.
func access(fd int, mode, flags uint32) error {
	return atCall(sysFaccessat2, fd, "", uintptr(mode), uintptr(atEmptyPath|flags&atEaccess))
}

// statvfs writes the guest's struct statvfs of a host descriptor's file
// system.
func (p *Process) statvfs(fd int, at uint32) error {
	var st syscall.Statfs_t

	if err := syscall.Fstatfs(fd, &st); err != nil {
		return err
	}

	p.putStatvfs(at, &st)

	return nil
}

// putStatvfs writes a guest struct statvfs.
func (p *Process) putStatvfs(at uint32, st *syscall.Statfs_t) {
	b := p.span(at, 72)
	clear(b)
	le := binary.LittleEndian
	// Linux fs/statfs.c already substitutes f_bsize for a zero f_frsize.
	le.PutUint32(b, uint32(st.Bsize))
	le.PutUint32(b[4:], uint32(st.Frsize))
	le.PutUint64(b[8:], st.Blocks)
	le.PutUint64(b[16:], st.Bfree)
	le.PutUint64(b[24:], st.Bavail)
	le.PutUint64(b[32:], st.Files)
	le.PutUint64(b[40:], st.Ffree)
	le.PutUint64(b[48:], st.Ffree)
	le.PutUint32(b[56:], uint32(st.Fsid.X__val[0]))
	le.PutUint32(b[60:], uint32(st.Flags))
	le.PutUint32(b[64:], uint32(st.Namelen))
}

func (p *Process) libcOpen(path, flags, va uint32) uint32 {
	return p.result(p.open(p.str(path), flags, va))
}

func (p *Process) libcOpenat(dirfd, path, flags, va uint32) uint32 {
	full, err := p.atPath(int32(dirfd), p.str(path))

	if err != nil {
		return p.result(0, err)
	}

	return p.result(p.open(full, flags, va))
}

func (p *Process) libcFstatat(dirfd, path, at, flags uint32) uint32 {
	var st syscall.Stat_t
	err := p.onAt(int32(dirfd), p.str(path), flags, func(fd int) error {
		return fileStat(fd, &st)
	})

	if err == nil {
		p.putStat(at, &st)
	}

	return p.result(0, err)
}

func (p *Process) libcAccess(path, mode uint32) uint32 {
	return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
		return access(fd, mode, 0)
	}))
}

func (p *Process) libcFaccessat(dirfd, path, mode, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(fd int) error {
		return access(fd, mode, flags)
	}))
}

func (p *Process) libcTruncate(path uint32, size uint64) uint32 {
	return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
		return syscall.Truncate(procFd(fd), int64(size))
	}))
}

func (p *Process) libcStatvfs(path, at uint32) uint32 {
	return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
		return p.statvfs(fd, at)
	}))
}

func (p *Process) libcFstatvfs(fd, at uint32) uint32 {
	n, err := p.nodeAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	var st syscall.Statfs_t

	if err := n.statfs(&st); err != nil {
		return p.result(0, err)
	}

	p.putStatvfs(at, &st)

	return p.result(0, nil)
}

func (p *Process) libcFchdir(fd uint32) uint32 {
	d, err := p.dirAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	host, err := d.hostPath()

	if err != nil {
		return p.result(0, err)
	}

	return p.result(0, p.chdir(p.guestPath(host)))
}

// chroot makes a directory the process's root: its paths resolve there
// from then on, and the working directory is named there, or is the new
// root itself when it lies outside.
func (p *Process) libcChroot(path uint32) uint32 {
	fd, err := p.openat2(p.str(path), oPath|syscall.O_DIRECTORY, 0)

	if err != nil {
		return p.result(0, err)
	}

	if err := atCall(sysFaccessat2, fd, "", 1, atEmptyPath); err != nil {
		syscall.Close(fd)

		return p.result(0, err)
	}

	dir := newHostDir(fd)
	host, err := dir.hostPath()

	if err != nil {
		dir.unref()

		return p.result(0, err)
	}

	cwd := filepath.Join(p.rootPath, p.cwd)
	p.root.unref()
	p.root, p.rootPath = dir, host
	p.cwd = p.guestPath(cwd)

	return p.result(0, nil)
}

func (p *Process) libcChdir(path uint32) uint32 {
	return p.result(0, p.chdir(p.str(path)))
}

func (p *Process) libcGetcwd(buf, size uint32) uint32 {
	if buf == 0 {
		at := p.newString(p.cwd)

		if at == 0 {
			return p.pointer(0, syscall.ENOMEM)
		}

		return p.pointer(at, nil)
	}

	if uint32(len(p.cwd)) >= size {
		return p.pointer(0, syscall.ERANGE)
	}

	p.putString(buf, p.cwd)

	return p.pointer(buf, nil)
}

func (p *Process) libcMkdir(path, mode uint32) uint32 {
	return p.result(0, p.mkdir(p.str(path), mode))
}

func (p *Process) libcMkdirat(dirfd, path, mode uint32) uint32 {
	full, err := p.atPath(int32(dirfd), p.str(path))

	if err == nil {
		err = p.mkdir(full, mode)
	}

	return p.result(0, err)
}

func (p *Process) libcRmdir(path uint32) uint32 {
	return p.result(0, p.unlink(p.str(path), atRemovedir))
}

func (p *Process) libcUnlink(path uint32) uint32 {
	return p.result(0, p.unlink(p.str(path), 0))
}

func (p *Process) libcUnlinkat(dirfd, path, flags uint32) uint32 {
	if flags&^atRemovedir != 0 {
		return p.result(0, syscall.EINVAL)
	}

	full, err := p.atPath(int32(dirfd), p.str(path))

	if err == nil {
		err = p.unlink(full, int(flags))
	}

	return p.result(0, err)
}

// remove unlinks a file or empties a directory, whichever it is.
func (p *Process) libcRemove(path uint32) uint32 {
	take := func(dirfd int, name string) error {
		err := unlinkat(dirfd, name, 0)

		if err == syscall.EISDIR || err == syscall.EPERM {
			return unlinkat(dirfd, name, atRemovedir)
		}

		return err
	}

	return p.result(0, p.onName(p.str(path), syscall.EBUSY, take))
}

func (p *Process) libcRename(old, new uint32) uint32 {
	return p.result(0, p.rename(p.str(old), p.str(new)))
}

func (p *Process) libcRenameat(olddir, old, newdir, new uint32) uint32 {
	from, err := p.atPath(int32(olddir), p.str(old))

	if err != nil {
		return p.result(0, err)
	}

	to, err := p.atPath(int32(newdir), p.str(new))

	if err != nil {
		return p.result(0, err)
	}

	return p.result(0, p.rename(from, to))
}

func (p *Process) libcLink(old, new uint32) uint32 {
	return p.result(0, p.link(p.str(old), p.str(new), false))
}

func (p *Process) libcLinkat(olddir, old, newdir, new, flags uint32) uint32 {
	if flags&^atSymlinkFollow != 0 {
		return p.result(0, syscall.EINVAL)
	}

	from, err := p.atPath(int32(olddir), p.str(old))

	if err != nil {
		return p.result(0, err)
	}

	to, err := p.atPath(int32(newdir), p.str(new))

	if err != nil {
		return p.result(0, err)
	}

	return p.result(0, p.link(from, to, flags&atSymlinkFollow != 0))
}

func (p *Process) libcSymlink(target, path uint32) uint32 {
	return p.result(0, p.symlink(p.str(target), p.str(path)))
}

func (p *Process) libcSymlinkat(target, dirfd, path uint32) uint32 {
	full, err := p.atPath(int32(dirfd), p.str(path))

	if err == nil {
		err = p.symlink(p.str(target), full)
	}

	return p.result(0, err)
}

func (p *Process) libcReadlink(path, buf, size uint32) uint32 {
	out := p.span(buf, size)
	n := 0
	err := p.onPath(p.str(path), false, func(fd int) error {
		var e error
		n, e = readlinkFd(fd, out, true)

		return e
	})

	return p.result(n, err)
}

func (p *Process) libcReadlinkat(dirfd, path, buf, size uint32) uint32 {
	out := p.span(buf, size)
	n := 0
	err := p.onAt(int32(dirfd), p.str(path), atSymlinkNofollow|atEmptyPath, func(fd int) error {
		var e error
		n, e = readlinkFd(fd, out, p.u8(path) != 0)

		return e
	})

	return p.result(n, err)
}

func (p *Process) libcCanonicalize_file_name(path uint32) uint32 {
	guest, err := p.realpath(p.str(path))

	if err != nil {
		return p.pointer(0, err)
	}

	at := p.newString(guest)

	if at == 0 {
		return p.pointer(0, syscall.ENOMEM)
	}

	return p.pointer(at, nil)
}

func (p *Process) libcGet_current_dir_name() uint32 {
	at := p.newString(p.cwd)

	if at == 0 {
		return p.pointer(0, syscall.ENOMEM)
	}

	return p.pointer(at, nil)
}

func (p *Process) libcCreat(path, mode uint32) uint32 {
	fd, err := p.openat2(p.str(path), syscall.O_WRONLY|syscall.O_CREAT|syscall.O_TRUNC, mode&0o7777&^p.umask)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(p.install(newHostObject(fd, true), 0, false))
}

func (p *Process) libcEaccess(path, mode uint32) uint32 {
	return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
		return access(fd, mode, atEaccess)
	}))
}

func (p *Process) libcEuidaccess(path, mode uint32) uint32 {
	return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
		return access(fd, mode, atEaccess)
	}))
}

func (p *Process) libcStatfs(path, at uint32) uint32 {
	return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
		return p.putStatfs(fd, at)
	}))
}

func (p *Process) libcFstatfs(fd, at uint32) uint32 {
	n, err := p.nodeAt(fd)

	if err != nil {
		return p.result(0, err)
	}

	var st syscall.Statfs_t

	if err := n.statfs(&st); err != nil {
		return p.result(0, err)
	}

	p.putStatfsOf(at, &st)

	return p.result(0, nil)
}

func (p *Process) libcRealpath(path, resolved uint32) uint32 {
	guest, err := p.realpath(p.str(path))

	if err != nil {
		return p.pointer(0, err)
	}

	if resolved == 0 {
		resolved = p.newString(guest)

		if resolved == 0 {
			return p.pointer(0, syscall.ENOMEM)
		}

		return p.pointer(resolved, nil)
	}

	// realpath reads /proc/self/fd: Linux bounds that result below PATH_MAX
	// in fs/proc/base.c. Removing the root prefix can only shorten it.
	p.putString(resolved, guest)

	return p.pointer(resolved, nil)
}

func (p *Process) libcChmod(path, mode uint32) uint32 {
	return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
		return atCall(sysFchmodat2, fd, "", uintptr(mode&0o7777), atEmptyPath)
	}))
}

func (p *Process) libcFchmodat(dirfd, path, mode, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(fd int) error {
		return atCall(sysFchmodat2, fd, "", uintptr(mode&0o7777), atEmptyPath)
	}))
}

func (p *Process) libcFchownat(dirfd, path, uid, gid, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(fd int) error {
		return syscall.Fchownat(fd, "", int(int32(uid)), int(int32(gid)), atEmptyPath)
	}))
}

func (p *Process) libcMknod(path, mode uint32, dev uint64) uint32 {
	return p.result(0, p.mknod(p.str(path), mode, dev))
}

func (p *Process) libcMknodat(dirfd, path, mode uint32, dev uint64) uint32 {
	full, err := p.atPath(int32(dirfd), p.str(path))

	if err == nil {
		err = p.mknod(full, mode, dev)
	}

	return p.result(0, err)
}

func (p *Process) libcMkfifo(path, mode uint32) uint32 {
	return p.result(0, p.mknod(p.str(path), sIfifo|mode&0o7777, 0))
}

func (p *Process) libcMkfifoat(dirfd, path, mode uint32) uint32 {
	full, err := p.atPath(int32(dirfd), p.str(path))

	if err == nil {
		err = p.mknod(full, sIfifo|mode&0o7777, 0)
	}

	return p.result(0, err)
}

func (p *Process) libcUtimes(path, times uint32) uint32 {
	return p.result(0, p.utimens(p.str(path), p.timevals(times), 0))
}

func (p *Process) libcLutimes(path, times uint32) uint32 {
	return p.result(0, p.utimens(p.str(path), p.timevals(times), atSymlinkNofollow))
}

func (p *Process) libcUtime(path, times uint32) uint32 {
	var ts []syscall.Timespec

	if times != 0 {
		ts = []syscall.Timespec{{Sec: int64(p.u64(times))}, {Sec: int64(p.u64(times + 8))}}
	}

	return p.result(0, p.utimens(p.str(path), ts, 0))
}

func (p *Process) libcUtimensat(dirfd, path, times, flags uint32) uint32 {
	full, err := p.atPath(int32(dirfd), p.str(path))

	if err == nil {
		err = p.utimens(full, p.timespecs(times), int(flags))
	}

	return p.result(0, err)
}

// readdir_r returns an error number.
func (p *Process) libcReaddir_r(dir, entry, result uint32) uint32 {
	e, err := p.readdir(dir)
	p.put32(result, 0)

	if err != nil {
		return uint32(errnoOf(err))
	}

	if e != 0 {
		n := 19 + uint32(len(p.cstr(e+19))) + 1
		copy(p.span(entry, n), p.span(e, n))
		p.put32(result, entry)
	}

	return 0
}

func (p *Process) libcSeekdir(dir, at uint32) {
	p.rewinddir(dir)

	for p.u32(dir+dirIndex) < at {
		if e, _ := p.readdir(dir); e == 0 {
			return
		}
	}
}

func (p *Process) libcMkdtemp(template uint32) uint32 {
	err := p.temporary(template, 0, func(path string) error {
		return p.onName(path, syscall.EEXIST, func(dirfd int, name string) error {
			return syscall.Mkdirat(dirfd, name, 0o700)
		})
	})

	return p.pointer(template, err)
}

func (p *Process) libcMktemp(template uint32) uint32 {
	err := p.temporary(template, 0, func(path string) error {
		err := p.onPath(path, false, func(int) error {
			return nil
		})

		if err == syscall.ENOENT {
			return nil
		}

		if err == nil {
			return syscall.EEXIST
		}

		return err
	})

	if err != nil {
		p.span(template, 1)[0] = 0
		p.setErrno(errnoOf(err))
	}

	return p.pointer(template, nil)
}

// timevals reads a guest struct timeval[2] as timespecs, or nil for a
// null pointer.
func (p *Process) timevals(at uint32) []syscall.Timespec {
	if at == 0 {
		return nil
	}

	b := p.span(at, 32)
	le := binary.LittleEndian
	var ts []syscall.Timespec

	for i := range 2 {
		ts = append(ts, syscall.Timespec{Sec: int64(le.Uint64(b[16*i:])), Nsec: 1000 * int64(le.Uint64(b[16*i+8:]))})
	}

	return ts
}

func (p *Process) stat(path, at uint32, follow bool) uint32 {
	var st syscall.Stat_t
	err := p.onPath(p.str(path), follow, func(fd int) error {
		return fileStat(fd, &st)
	})

	if err == nil {
		p.putStat(at, &st)
	}

	return p.result(0, err)
}

func (p *Process) chown(path, uid, gid uint32, follow bool) uint32 {
	return p.result(0, p.onPath(p.str(path), follow, func(fd int) error {
		return syscall.Fchownat(fd, "", int(int32(uid)), int(int32(gid)), atEmptyPath)
	}))
}

// utimens sets the times of a path: what it names through /proc, a last
// symbolic link itself in its parent.
func (p *Process) utimens(path string, ts []syscall.Timespec, flags int) error {
	if flags&atSymlinkNofollow != 0 {
		return p.onName(path, syscall.EINVAL, func(dirfd int, name string) error {
			return utimensat(dirfd, throw2(syscall.BytePtrFromString(name)), ts, atSymlinkNofollow)
		})
	}

	return p.onPath(path, true, func(fd int) error {
		return utimensat(atFdcwd, throw2(syscall.BytePtrFromString(procFd(fd))), ts, 0)
	})
}

func (p *Process) chdir(path string) error {
	fd, err := p.openat2(path, oPath|syscall.O_DIRECTORY, 0)

	if err != nil {
		return err
	}

	defer syscall.Close(fd)

	if err := atCall(sysFaccessat2, fd, "", 1, atEmptyPath); err != nil {
		return err
	}

	host, err := hostPath(fd)

	if err != nil {
		return err
	}

	p.cwd = p.guestPath(host)

	return nil
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

const tempLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

// temporary fills the XXXXXX that ends a template, or comes before a
// suffix of its last bytes, with random letters until create makes what
// the path names, EEXIST meaning another try.
func (p *Process) temporary(template, suffix uint32, create func(path string) error) error {
	t := p.cstr(template)
	end := len(t) - int(suffix)

	if int(suffix) > len(t) || end < 6 || string(t[end-6:end]) != "XXXXXX" {
		return syscall.EINVAL
	}

	for range 100 {
		for i := end - 6; i < end; i++ {
			t[i] = tempLetters[rand.IntN(len(tempLetters))]
		}

		if err := create(string(t)); err != syscall.EEXIST {
			return err
		}
	}

	return syscall.EEXIST
}

// mkstemp makes a file of a template, open with flags.
func (p *Process) mkstemp(template, suffix, flags uint32) uint32 {
	fd := -1
	err := p.temporary(template, suffix, func(path string) error {
		host, err := p.openat2(path, syscall.O_RDWR|syscall.O_CREAT|syscall.O_EXCL|int(flags&^syscall.O_CLOEXEC), 0o600)

		if err == nil {
			fd, err = p.install(newHostObject(host, true), 0, flags&syscall.O_CLOEXEC != 0)
		}

		return err
	})

	return p.result(fd, err)
}

// realpath is the guest path a path names.
func (p *Process) realpath(path string) (string, error) {
	var guest string
	err := p.onPath(path, true, func(fd int) error {
		host, err := hostPath(fd)
		guest = p.guestPath(host)

		return err
	})

	return guest, err
}

// putStatfs writes the guest's struct statfs of a host descriptor's file
// system.
func (p *Process) putStatfs(fd int, at uint32) error {
	var st syscall.Statfs_t

	if err := syscall.Fstatfs(fd, &st); err != nil {
		return err
	}

	p.putStatfsOf(at, &st)

	return nil
}

// putStatfsOf writes a guest struct statfs.
func (p *Process) putStatfsOf(at uint32, st *syscall.Statfs_t) {
	b := p.span(at, 88)
	clear(b)
	le := binary.LittleEndian
	le.PutUint32(b, uint32(st.Type))
	le.PutUint32(b[4:], uint32(st.Bsize))
	le.PutUint64(b[8:], st.Blocks)
	le.PutUint64(b[16:], st.Bfree)
	le.PutUint64(b[24:], st.Bavail)
	le.PutUint64(b[32:], st.Files)
	le.PutUint64(b[40:], st.Ffree)
	le.PutUint32(b[48:], uint32(st.Fsid.X__val[0]))
	le.PutUint32(b[52:], uint32(st.Fsid.X__val[1]))
	le.PutUint32(b[56:], uint32(st.Namelen))
	le.PutUint32(b[60:], uint32(st.Frsize))
	le.PutUint32(b[64:], uint32(st.Flags))
}

// A DIR is a block of the heap: the descriptor, the position and end of
// the entries in the buffer, how many entries readdir returned since the
// start, then the buffer, which getdents64 fills with entries of the
// guest's struct dirent layout. telldir is that count, which seekdir reads
// again from the start: a host's offset may not fit a long.
const (
	dirFd     = 0
	dirPos    = 4
	dirEnd    = 8
	dirIndex  = 12
	dirBuf    = 16
	dirBufLen = 4096
)

func (p *Process) libcOpendir(path uint32) uint32 {
	host, err := p.openat2(p.str(path), syscall.O_RDONLY|syscall.O_DIRECTORY, 0)

	if err != nil {
		return p.pointer(0, err)
	}

	fd, err := p.install(newHostObject(host, true), 0, true)

	if err != nil {
		return p.pointer(0, err)
	}

	dir := p.newDir(fd)

	if dir == 0 {
		p.closeFd(fd)

		return p.pointer(0, syscall.ENOMEM)
	}

	return p.pointer(dir, nil)
}

// newDir makes the DIR of a descriptor, or returns 0.
func (p *Process) newDir(fd int) uint32 {
	dir := p.malloc(dirBuf + dirBufLen)

	if dir != 0 {
		p.put32(dir+dirFd, uint32(fd))
		p.put32(dir+dirPos, 0)
		p.put32(dir+dirEnd, 0)
		p.put32(dir+dirIndex, 0)
	}

	return dir
}

// fdopendir makes a DIR of a directory's descriptor, which becomes
// close-on-exec, as musl's.
func (p *Process) libcFdopendir(fd uint32) uint32 {
	obj, err := p.fd(fd)

	if err != nil {
		return p.pointer(0, err)
	}

	var st syscall.Stat_t

	if err := obj.stat(&st); err != nil {
		return p.pointer(0, err)
	}

	if _, ok := obj.(Dir); !ok || st.Mode&syscall.S_IFMT != syscall.S_IFDIR {
		return p.pointer(0, syscall.ENOTDIR)
	}

	dir := p.newDir(int(fd))

	if dir == 0 {
		return p.pointer(0, syscall.ENOMEM)
	}

	p.files[fd].cloexec = true

	return p.pointer(dir, nil)
}

func (p *Process) rewinddir(dir uint32) {
	if s, err := p.seekerAt(p.u32(dir + dirFd)); err == nil {
		s.seek(0, 0)
	}

	p.put32(dir+dirPos, 0)
	p.put32(dir+dirEnd, 0)
	p.put32(dir+dirIndex, 0)
}

// direntCompare compares the names of two struct dirent** of scandir.
func (p *Process) direntCompare(a, b uint32, compare func(x, y []byte) int) uint32 {
	x, y := p.cstr(p.u32(a)+19), p.cstr(p.u32(b)+19)

	return uint32(int32(compare(x, y)))
}

func (p *Process) libcReaddir(dir uint32) uint32 {
	entry, err := p.readdir(dir)

	return p.pointer(entry, err)
}

// readdir returns the next entry of a DIR, or 0 at its end.
func (p *Process) readdir(dir uint32) (uint32, error) {
	pos, end := p.u32(dir+dirPos), p.u32(dir+dirEnd)

	if pos >= end {
		d, err := p.dirAt(p.u32(dir + dirFd))

		if err != nil {
			return 0, err
		}

		n, err := d.readdir(p.span(dir+dirBuf, dirBufLen))

		if err != nil || n == 0 {
			return 0, err
		}

		pos, end = 0, uint32(n)
		p.put32(dir+dirEnd, end)
	}

	entry := dir + dirBuf + pos
	p.put32(dir+dirPos, pos+uint32(p.u16(entry+16)))
	p.put32(dir+dirIndex, p.u32(dir+dirIndex)+1)

	return entry, nil
}

func (p *Process) libcClosedir(dir uint32) uint32 {
	fd := p.u32(dir + dirFd)
	p.free(dir)

	if !p.closeFd(int(fd)) {
		return p.result(0, syscall.EBADF)
	}

	return p.result(0, nil)
}

func (p *Process) libcStat(path, at uint32) uint32 {
	return p.stat(path, at, true)
}

func (p *Process) libcLstat(path, at uint32) uint32 {
	return p.stat(path, at, false)
}

// Process accounting needs privilege.
func (p *Process) libcAcct(path uint32) uint32 {
	return p.result(0, syscall.EPERM)
}

func (p *Process) libcChown(path, uid, gid uint32) uint32 {
	return p.chown(path, uid, gid, true)
}

func (p *Process) libcLchown(path, uid, gid uint32) uint32 {
	return p.chown(path, uid, gid, false)
}

func (p *Process) libcDirfd(dir uint32) uint32 {
	return p.u32(dir + dirFd)
}

func (p *Process) libcTelldir(dir uint32) uint32 {
	return p.u32(dir + dirIndex)
}

func (p *Process) libcAlphasort(a, b uint32) uint32 {
	return p.direntCompare(a, b, bytes.Compare)
}

func (p *Process) libcVersionsort(a, b uint32) uint32 {
	return p.direntCompare(a, b, strverscmp)
}

func (p *Process) libcMkstemp(template uint32) uint32 {
	return p.mkstemp(template, 0, 0)
}

func (p *Process) libcMkostemp(template, flags uint32) uint32 {
	return p.mkstemp(template, 0, flags)
}

func (p *Process) libcMkstemps(template, suffix uint32) uint32 {
	return p.mkstemp(template, suffix, 0)
}

func (p *Process) libcRewinddir(dir uint32) {
	p.rewinddir(dir)
}
