//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"math/rand/v2"
	"strings"
	"syscall"
)

// Paths are the guest's: absolute ones start at the process's root,
// relative ones at the working directory, and go to the root whole,
// absolute: the root is a Dir and resolves them (kernel_object.go). A
// call relative to a directory descriptor starts at the directory's
// name. The working directory is a name the root gave.

// full is the path from the root.
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

// atPath is the path a call relative to a guest directory descriptor names.
func (p *Process) atPath(dirfd int32, path string) (string, error) {
	if dirfd == atFdcwd || strings.HasPrefix(path, "/") {
		return path, nil
	}

	d, err := p.dirAt(uint32(dirfd))

	if err != nil {
		return "", err
	}

	dir, err := d.name(p)

	if err != nil {
		return "", err
	}

	return dir + "/" + path, nil
}

// onAt runs a call on what an *at call names: byPath with the full path
// and whether a last symbolic link is followed, or byFile on the
// descriptor's own file when the path is empty with AT_EMPTY_PATH.
func (p *Process) onAt(dirfd int32, path string, flags uint32, byPath func(full string, follow bool) error, byFile func(f File) error) error {
	if path == "" && flags&atEmptyPath != 0 {
		if dirfd == atFdcwd {
			return byPath(p.cwd, true)
		}

		obj, err := p.fd(uint32(dirfd))

		if err != nil {
			return err
		}

		return byFile(obj)
	}

	rel, err := p.atPath(dirfd, path)

	if err != nil {
		return err
	}

	full, err := p.full(rel)

	if err != nil {
		return err
	}

	return byPath(full, flags&atSymlinkNofollow == 0)
}

// nodeCall is a call on a descriptor's node; another file has no such
// call: EINVAL.
func nodeCall(call func(n Node) error) func(f File) error {
	return func(f File) error {
		if n, ok := f.(Node); ok {
			return call(n)
		}

		return syscall.EINVAL
	}
}

// open opens a guest path and installs the file at the lowest descriptor.
func (p *Process) open(path string, flags, va uint32) (int, error) {
	mode := uint32(0)

	if flags&(syscall.O_CREAT|0o20000000) != 0 {
		mode = p.varArg(va, 0) & 0o7777 &^ p.umask
	}

	f, err := p.openPath(path, int(flags&^syscall.O_CLOEXEC), mode)

	if err != nil {
		return 0, err
	}

	return p.install(f, 0, flags&syscall.O_CLOEXEC != 0)
}

// openPath opens a guest path through the root.
func (p *Process) openPath(path string, flags int, mode uint32) (File, error) {
	full, err := p.full(path)

	if err != nil {
		return nil, err
	}

	return p.root.open(p, full, flags, mode)
}

// readPath reads a file of the root whole, up to max bytes: the kernel's
// own reads of the databases and tables under the root. The error is the
// open's; what cannot be read, a directory, is read as empty, as musl
// reads it through fopen.
func (p *Process) readPath(path string, max int) ([]byte, error) {
	f, err := p.openPath(path, syscall.O_RDONLY, 0)

	if err != nil {
		return nil, err
	}

	defer f.unref()
	t, ok := f.(Transfer)

	if !ok {
		return nil, nil
	}

	var data []byte
	buf := make([]byte, 65536)

	for len(data) < max {
		n, err := t.read(p, buf, ioMode{})

		if err != nil || n == 0 {
			break
		}

		data = append(data, buf[:n]...)
	}

	return data, nil
}

// onFull runs a call on the full path of a guest path.
func (p *Process) onFull(path string, call func(full string) error) error {
	full, err := p.full(path)

	if err != nil {
		return err
	}

	return call(full)
}

func (p *Process) mkdir(path string, mode uint32) error {
	return p.onFull(path, func(full string) error {
		return p.root.mkdir(p, full, mode&0o7777&^p.umask)
	})
}

// unlink removes a name, or with dir an empty directory.
func (p *Process) unlink(path string, dir bool) error {
	return p.onFull(path, func(full string) error {
		return p.root.unlink(p, full, dir)
	})
}

func (p *Process) rename(old, new string) error {
	return p.onFull(old, func(from string) error {
		return p.onFull(new, func(to string) error {
			return p.root.rename(p, from, to)
		})
	})
}

func (p *Process) link(old, new string, follow bool) error {
	return p.onFull(old, func(from string) error {
		return p.onFull(new, func(to string) error {
			return p.root.link(p, from, to, follow)
		})
	})
}

func (p *Process) symlink(target, path string) error {
	return p.onFull(path, func(full string) error {
		return p.root.symlink(p, target, full)
	})
}

func (p *Process) mknod(path string, mode uint32, dev uint64) error {
	return p.onFull(path, func(full string) error {
		return p.root.mknod(p, full, mode&^(p.umask&0o777), dev)
	})
}

func (p *Process) accessPath(path string, mode, flags uint32) error {
	return p.onFull(path, func(full string) error {
		return p.root.accessPath(p, full, mode, flags)
	})
}

// statPath is the metadata of what a path names, or of a last symbolic
// link itself.
func (p *Process) statPath(path string, follow bool, st *syscall.Stat_t) error {
	return p.onFull(path, func(full string) error {
		return p.root.statPath(p, full, follow, st)
	})
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
	err := p.onAt(int32(dirfd), p.str(path), flags, func(full string, follow bool) error {
		return p.root.statPath(p, full, follow, &st)
	}, func(f File) error {
		return statFile(f, &st)
	})

	if err == nil {
		p.putStat(at, &st)
	}

	return p.result(0, err)
}

func (p *Process) libcAccess(path, mode uint32) uint32 {
	return p.result(0, p.accessPath(p.str(path), mode, 0))
}

// faccessat of a descriptor's own file: a file of the kernel is the
// process's to use.
func (p *Process) libcFaccessat(dirfd, path, mode, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(full string, follow bool) error {
		return p.root.accessPath(p, full, mode, flags)
	}, func(f File) error {
		if n, ok := f.(Node); ok {
			return n.access(mode, flags)
		}

		return nil
	}))
}

func (p *Process) libcTruncate(path uint32, size uint64) uint32 {
	return p.result(0, p.onFull(p.str(path), func(full string) error {
		return p.root.truncatePath(p, full, int64(size))
	}))
}

func (p *Process) libcStatvfs(path, at uint32) uint32 {
	var st syscall.Statfs_t
	err := p.onFull(p.str(path), func(full string) error {
		return p.root.statfsPath(p, full, &st)
	})

	if err == nil {
		p.putStatvfs(at, &st)
	}

	return p.result(0, err)
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

// setCwdDir keeps a directory as the working directory's, or none.
func (p *Process) setCwdDir(d Dir) {
	if p.cwdDir != nil {
		p.cwdDir.unref()
	}

	p.cwdDir = d
}

// fchdir makes a directory descriptor's directory the working directory,
// by its name; a connection to a server is a directory of that server,
// its root, which has no name here but "/": chroot(".") takes it.
func (p *Process) libcFchdir(fd uint32) uint32 {
	obj, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	d, named := asRoot(obj)

	if d == nil {
		return p.result(0, syscall.ENOTDIR)
	}

	if named {
		name, err := d.name(p)

		if err == nil {
			err = p.chdir(name)
		}

		if err != nil {
			return p.result(0, err)
		}

		d.ref()
	} else {
		p.cwd = "/"
	}

	p.setCwdDir(d)

	return p.result(0, nil)
}

// chroot makes a directory the process's root: its paths resolve there
// from then on. In a root of the host the working directory is named
// there, or is the new root itself when it lies outside; in a server's
// it is the root.
func (p *Process) libcChroot(path uint32) uint32 {
	var dir Dir

	if name := p.str(path); name == "." && p.cwdDir != nil {
		dir = p.cwdDir
		dir.ref()
	} else {
		full, err := p.full(name)

		if err != nil {
			return p.result(0, err)
		}

		// A directory first, as Linux has it, then one the process may
		// search.
		f, err := p.root.open(p, full, oPath|syscall.O_DIRECTORY, 0)

		if err != nil {
			return p.result(0, err)
		}

		d, ok := f.(Dir)

		if !ok {
			f.unref()

			return p.result(0, syscall.ENOTDIR)
		}

		if err := p.root.accessPath(p, full, 1, 0); err != nil {
			f.unref()

			return p.result(0, err)
		}

		dir = d
	}

	f := File(dir)
	cwd, rootPath := "/", ""

	if pathed, ok := f.(Pathed); ok {
		host, err := pathed.hostPath()

		if err != nil {
			f.unref()

			return p.result(0, err)
		}

		rootPath = host
		cwd = guestPathIn(host, p.cwdHostPath())
	}

	p.root.unref()
	p.root, p.rootPath, p.cwd = dir, rootPath, cwd
	p.setCwdDir(nil)

	return p.result(0, nil)
}

// cwdHostPath is the host path of the working directory in a root of the
// host.
func (p *Process) cwdHostPath() string {
	if p.rootPath == "/" {
		return p.cwd
	}

	return strings.TrimSuffix(p.rootPath, "/") + strings.TrimSuffix(p.cwd, "/")
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
	return p.result(0, p.unlink(p.str(path), true))
}

func (p *Process) libcUnlink(path uint32) uint32 {
	return p.result(0, p.unlink(p.str(path), false))
}

func (p *Process) libcUnlinkat(dirfd, path, flags uint32) uint32 {
	if flags&^atRemovedir != 0 {
		return p.result(0, syscall.EINVAL)
	}

	full, err := p.atPath(int32(dirfd), p.str(path))

	if err == nil {
		err = p.unlink(full, flags&atRemovedir != 0)
	}

	return p.result(0, err)
}

// remove unlinks a file or empties a directory, whichever it is.
func (p *Process) libcRemove(path uint32) uint32 {
	err := p.unlink(p.str(path), false)

	if err == syscall.EISDIR || err == syscall.EPERM {
		err = p.unlink(p.str(path), true)
	}

	return p.result(0, err)
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

// readlink writes a link's target, as much as the buffer holds.
func (p *Process) readlink(path string, buf, size uint32) (int, error) {
	var target string
	err := p.onFull(path, func(full string) error {
		var e error
		target, e = p.root.readlinkPath(p, full)

		return e
	})

	if err != nil {
		return 0, err
	}

	return copy(p.span(buf, size), target), nil
}

func (p *Process) libcReadlink(path, buf, size uint32) uint32 {
	return p.result(p.readlink(p.str(path), buf, size))
}

// readlinkat with an empty path reads the link a descriptor is: what is
// no link is ENOENT then, as Linux has it, not the named path's EINVAL.
func (p *Process) libcReadlinkat(dirfd, path, buf, size uint32) uint32 {
	n := 0
	named := p.u8(path) != 0
	err := p.onAt(int32(dirfd), p.str(path), atSymlinkNofollow|atEmptyPath, func(full string, follow bool) error {
		var e error
		n, e = p.readlink(full, buf, size)

		if e == syscall.EINVAL && !named {
			e = syscall.ENOENT
		}

		return e
	}, func(f File) error {
		var e error
		n, e = 0, syscall.EINVAL

		if node, ok := f.(Node); ok {
			n, e = node.readlink(p.span(buf, size))
		}

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
	f, err := p.openPath(p.str(path), syscall.O_WRONLY|syscall.O_CREAT|syscall.O_TRUNC, mode&0o7777&^p.umask)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(p.install(f, 0, false))
}

func (p *Process) libcEaccess(path, mode uint32) uint32 {
	return p.result(0, p.accessPath(p.str(path), mode, atEaccess))
}

func (p *Process) libcEuidaccess(path, mode uint32) uint32 {
	return p.result(0, p.accessPath(p.str(path), mode, atEaccess))
}

func (p *Process) libcStatfs(path, at uint32) uint32 {
	var st syscall.Statfs_t
	err := p.onFull(p.str(path), func(full string) error {
		return p.root.statfsPath(p, full, &st)
	})

	if err == nil {
		p.putStatfsOf(at, &st)
	}

	return p.result(0, err)
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

	// A host root's realpath reads /proc/self/fd: Linux bounds that result
	// below PATH_MAX in fs/proc/base.c. Removing the root prefix can only
	// shorten it.
	p.putString(resolved, guest)

	return p.pointer(resolved, nil)
}

func (p *Process) libcChmod(path, mode uint32) uint32 {
	return p.result(0, p.onFull(p.str(path), func(full string) error {
		return p.root.chmodPath(p, full, mode&0o7777)
	}))
}

func (p *Process) libcFchmodat(dirfd, path, mode, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(full string, follow bool) error {
		return p.root.chmodPath(p, full, mode&0o7777)
	}, nodeCall(func(n Node) error {
		return n.chmod(mode & 0o7777)
	})))
}

func (p *Process) libcFchownat(dirfd, path, uid, gid, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(full string, follow bool) error {
		return p.root.chownPath(p, full, int(int32(uid)), int(int32(gid)), follow)
	}, nodeCall(func(n Node) error {
		return n.chown(int(int32(uid)), int(int32(gid)))
	})))
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
	return p.result(0, p.utimens(p.str(path), p.timevals(times), true))
}

func (p *Process) libcLutimes(path, times uint32) uint32 {
	return p.result(0, p.utimens(p.str(path), p.timevals(times), false))
}

func (p *Process) libcUtime(path, times uint32) uint32 {
	var ts []syscall.Timespec

	if times != 0 {
		ts = []syscall.Timespec{{Sec: int64(p.u64(times))}, {Sec: int64(p.u64(times + 8))}}
	}

	return p.result(0, p.utimens(p.str(path), ts, true))
}

func (p *Process) libcUtimensat(dirfd, path, times, flags uint32) uint32 {
	full, err := p.atPath(int32(dirfd), p.str(path))

	if err == nil {
		err = p.utimens(full, p.timespecs(times), flags&atSymlinkNofollow == 0)
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
		return p.onFull(path, func(full string) error {
			return p.root.mkdir(p, full, 0o700)
		})
	})

	return p.pointer(template, err)
}

func (p *Process) libcMktemp(template uint32) uint32 {
	err := p.temporary(template, 0, func(path string) error {
		var st syscall.Stat_t
		err := p.statPath(path, false, &st)

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
	err := p.statPath(p.str(path), follow, &st)

	if err == nil {
		p.putStat(at, &st)
	}

	return p.result(0, err)
}

func (p *Process) chown(path, uid, gid uint32, follow bool) uint32 {
	return p.result(0, p.onFull(p.str(path), func(full string) error {
		return p.root.chownPath(p, full, int(int32(uid)), int(int32(gid)), follow)
	}))
}

// utimens sets the times of what a path names, or of a last symbolic
// link itself.
func (p *Process) utimens(path string, ts []syscall.Timespec, follow bool) error {
	return p.onFull(path, func(full string) error {
		return p.root.utimesPath(p, full, ts, follow)
	})
}

// chdir makes a directory the working directory: one the process may
// search, by the name the root gives it.
func (p *Process) chdir(path string) error {
	full, err := p.full(path)

	if err != nil {
		return err
	}

	var st syscall.Stat_t

	if err := p.root.statPath(p, full, true, &st); err != nil {
		return err
	}

	if st.Mode&syscall.S_IFMT != syscall.S_IFDIR {
		return syscall.ENOTDIR
	}

	if err := p.root.accessPath(p, full, 1, 0); err != nil {
		return err
	}

	name, err := p.root.realpath(p, full)

	if err != nil {
		return err
	}

	p.cwd = name
	p.setCwdDir(nil)

	return nil
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
		f, err := p.openPath(path, syscall.O_RDWR|syscall.O_CREAT|syscall.O_EXCL|int(flags&^syscall.O_CLOEXEC), 0o600)

		if err == nil {
			fd, err = p.install(f, 0, flags&syscall.O_CLOEXEC != 0)
		}

		return err
	})

	return p.result(fd, err)
}

// realpath is the guest path a path names.
func (p *Process) realpath(path string) (string, error) {
	full, err := p.full(path)

	if err != nil {
		return "", err
	}

	return p.root.realpath(p, full)
}

// A DIR is a block of the heap: the descriptor, the position and end of
// the entries in the buffer, how many entries readdir returned since the
// start, then the buffer, which the directory fills with entries of the
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
	f, err := p.openPath(p.str(path), syscall.O_RDONLY|syscall.O_DIRECTORY, 0)

	if err != nil {
		return p.pointer(0, err)
	}

	fd, err := p.install(f, 0, true)

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

	if err := statFile(obj, &st); err != nil {
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
