//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math/rand/v2"
	"path/filepath"
	"slices"
	"strings"
	"syscall"
)

// Paths are the guest's, walked as Linux's namei walks them. A path
// starts at the process's root when it is absolute, else at the working
// directory or at the directory of a directory descriptor: directories
// the process holds, not names. resolve walks it from there to the
// directory of its last name (walkFrom); the directories walk
// (Dir.resolve), and a walk that leaves one goes on where it says. Each call makes the last
// step itself, as only it knows how:
//   - a call that looks follows a last symbolic link when it means to, or
//     when a slash ends the path, which then wants a directory;
//   - open with O_CREAT makes the name, or what a link there points to,
//     and with O_EXCL follows nothing; O_NOFOLLOW on a link is ELOOP;
//   - the calls that make and remove names take the last name as it is,
//     refuse "." and ".." as Linux refuses them, and a slash on a name
//     that is no directory.

// walk is what one resolution carries from directory to directory: the
// root of the process, where ".." stops and absolute links start, and the
// symbolic links it went through, no more than maxLinks.
type walk struct {
	root  Dir
	links int
}

// maxLinks is how many symbolic links a path goes through, Linux's.
const maxLinks = 40

// The kinds of a path's last name.
const (
	lastName = iota
	lastDot
	lastDotdot
	// lastNone is the kind of the root alone, "/".
	lastNone
)

// step is where a resolution stops: the directory of the last name, the
// name, its kind and whether a slash ends the path. For ".", ".." and the
// root alone the directory is what the path names. With more, the walk
// goes on in dir with rest.
type step struct {
	dir   Dir
	last  string
	kind  int
	slash bool
	more  bool
	rest  string
}

// splitPath is a path's names, the empty ones dropped, and whether a
// slash ends it.
func splitPath(path string) ([]string, bool) {
	var names []string

	for _, name := range strings.Split(path, "/") {
		if name != "" {
			names = append(names, name)
		}
	}

	return names, strings.HasSuffix(path, "/")
}

// lastOf is a step's last name, its kind and the slash after it, of the
// last name of a path a slash may end.
func lastOf(name string, slash bool) (string, int, bool) {
	switch name {
	case ".":
		return "", lastDot, false
	case "..":
		return "", lastDotdot, false
	}

	return name, lastName, slash
}

// checked is a path a call takes: none is ENOENT, one of PATH_MAX or more
// ENAMETOOLONG.
func checked(path string) error {
	switch {
	case path == "":
		return syscall.ENOENT
	case len(path) >= 4096:
		return syscall.ENAMETOOLONG
	}

	return nil
}

// start is the directory a path starts at, which the caller holds a
// reference to: the root for an absolute one, else the working directory
// or the directory of dirfd.
func (p *Process) start(dirfd int32, path string) (Dir, error) {
	d := p.root

	if !strings.HasPrefix(path, "/") {
		d = p.cwd

		if dirfd != atFdcwd {
			var err error

			if d, err = p.dirAt(uint32(dirfd)); err != nil {
				return nil, err
			}
		}
	}

	d.ref()

	return d, nil
}

// walkFrom walks path from d, whose reference it takes, to the step of
// its last name.
func (p *Process) walkFrom(d Dir, path string, w *walk) (step, error) {
	// A path of "." alone names where it starts, which no directory is
	// asked about, as Linux asks no file system.
	if names, _ := splitPath(path); !slices.ContainsFunc(names, func(name string) bool { return name != "." }) {
		kind := lastDot

		if len(names) == 0 {
			kind = lastNone
		}

		return step{dir: d, kind: kind}, nil
	}

	for {
		s, err := d.resolve(p, path, w)
		d.unref()

		if err != nil || !s.more {
			return s, err
		}

		d, path = s.dir, s.rest
	}
}

// walkTo is the step of the last name of a path relative to dirfd.
func (p *Process) walkTo(dirfd int32, path string) (step, error) {
	if err := checked(path); err != nil {
		return step{}, err
	}

	d, err := p.start(dirfd, path)

	if err != nil {
		return step{}, err
	}

	return p.walkFrom(d, path, &walk{root: p.root})
}

// through is where a last symbolic link leads, with a reference: the
// link's directory, or the root for an absolute target, and the target,
// which keeps a slash that ended the path. The step's directory goes with
// it.
func (p *Process) through(s step, w *walk) (Dir, string, error) {
	if w.links >= maxLinks {
		s.dir.unref()

		return nil, "", syscall.ELOOP
	}

	w.links++
	target, err := s.dir.readlinkName(p, s.last)

	if err == nil && target == "" {
		err = syscall.ENOENT
	}

	if err != nil {
		s.dir.unref()

		return nil, "", err
	}

	if s.slash {
		target += "/"
	}

	if target[0] == '/' {
		s.dir.unref()
		p.root.ref()

		return p.root, target, nil
	}

	return s.dir, target, nil
}

// hostDevice is the directory, with a reference, and the name of a device
// of the host that a path names in a root of the host (kernel_hostdir.go
// hostDevices); ok is false for any other path. A relative path is one
// from d, the directory it starts at: one without a path, removed,
// outside the root or too long, names no device, but EIO finding it fails
// the call.
func (p *Process) hostDevice(d Dir, path string) (dev Dir, name string, ok bool, err error) {
	if _, host := p.root.(*hostDir); !host {
		return nil, "", false, nil
	}

	full := path

	if !strings.HasPrefix(path, "/") {
		base, err := d.path(p)

		if err == syscall.EIO {
			return nil, "", false, err
		}

		if err != nil {
			return nil, "", false, nil
		}

		full = base + "/" + path
	}

	clean := filepath.Clean(full)

	if !hostDevice(clean) {
		return nil, "", false, nil
	}

	dir, name := filepath.Split(clean)

	return newDeviceDir(dir), name, true, nil
}

// lookup is what a path relative to dirfd names to a call that looks at
// it: the directory of the last name, the name, "" when the directory is
// itself what the path names, and its stat. A last symbolic link is
// followed when follow says or a slash ends the path, which then wants a
// directory. The directory's reference is the caller's.
func (p *Process) lookup(dirfd int32, path string, follow bool, st *syscall.Stat_t) (Dir, string, error) {
	if err := checked(path); err != nil {
		return nil, "", err
	}

	d, err := p.start(dirfd, path)

	if err != nil {
		return nil, "", err
	}

	if dev, name, ok, err := p.hostDevice(d, path); ok || err != nil {
		d.unref()

		if err == nil {
			if err = dev.statName(p, name, st); err != nil {
				dev.unref()
			}
		}

		return dev, name, err
	}

	w := &walk{root: p.root}

	for {
		s, err := p.walkFrom(d, path, w)

		if err != nil {
			return nil, "", err
		}

		name := s.last

		if s.kind != lastName {
			name = ""
		}

		if err := s.dir.statName(p, name, st); err != nil {
			s.dir.unref()

			return nil, "", err
		}

		if name != "" && st.Mode&syscall.S_IFMT == syscall.S_IFLNK && (follow || s.slash) {
			next, target, err := p.through(s, w)

			if err != nil {
				return nil, "", err
			}

			d, path = next, target

			continue
		}

		if s.slash && st.Mode&syscall.S_IFMT != syscall.S_IFDIR {
			s.dir.unref()

			return nil, "", syscall.ENOTDIR
		}

		return s.dir, name, nil
	}
}

// onName runs a call on what a path relative to dirfd names, a last link
// followed when follow says: its directory, its name and its stat.
func (p *Process) onName(dirfd int32, path string, follow bool, call func(d Dir, name string, st *syscall.Stat_t) error) error {
	var st syscall.Stat_t
	d, name, err := p.lookup(dirfd, path, follow, &st)

	if err != nil {
		return err
	}

	defer d.unref()

	return call(d, name, &st)
}

// onAt runs a call on what an *at call names: byName on the path, a last
// link followed unless AT_SYMLINK_NOFOLLOW, or byFile on the descriptor's
// own file when the path is empty with AT_EMPTY_PATH.
func (p *Process) onAt(dirfd int32, path string, flags uint32, byName func(d Dir, name string, st *syscall.Stat_t) error, byFile func(f File) error) error {
	if path == "" && flags&atEmptyPath != 0 {
		if dirfd == atFdcwd {
			return byFile(p.cwd)
		}

		obj, err := p.fd(uint32(dirfd))

		if err != nil {
			return err
		}

		return byFile(obj)
	}

	return p.onName(dirfd, path, flags&atSymlinkNofollow == 0, byName)
}

// attrsCall is a call on a descriptor's attributes; a file without any
// has no such call: EINVAL.
func attrsCall(call func(a Attrs) error) func(f File) error {
	return func(f File) error {
		if a, ok := f.(Attrs); ok {
			return call(a)
		}

		return syscall.EINVAL
	}
}

// openAt opens what a path relative to dirfd names, as open(2) does. A
// last link is followed unless O_NOFOLLOW says not, or O_CREAT with
// O_EXCL asks for a name of its own; a slash at the end wants a
// directory, and with O_CREAT is EISDIR, as ".", ".." and "/" are.
func (p *Process) openAt(dirfd int32, path string, flags int, mode uint32) (File, error) {
	if err := checked(path); err != nil {
		return nil, err
	}

	d, err := p.start(dirfd, path)

	if err != nil {
		return nil, err
	}

	if dev, name, ok, err := p.hostDevice(d, path); ok || err != nil {
		d.unref()

		if err != nil {
			return nil, err
		}

		defer dev.unref()

		return dev.openName(p, name, flags, mode)
	}

	follow := flags&syscall.O_NOFOLLOW == 0 && flags&(syscall.O_CREAT|syscall.O_EXCL) != syscall.O_CREAT|syscall.O_EXCL
	w := &walk{root: p.root}

	for {
		s, err := p.walkFrom(d, path, w)

		if err != nil {
			return nil, err
		}

		if s.kind != lastName {
			if flags&syscall.O_CREAT != 0 {
				s.dir.unref()

				return nil, syscall.EISDIR
			}

			f, err := s.dir.openName(p, "", flags, mode)
			s.dir.unref()

			return f, err
		}

		fl := flags

		if s.slash {
			if flags&syscall.O_CREAT != 0 {
				s.dir.unref()

				return nil, syscall.EISDIR
			}

			fl |= syscall.O_DIRECTORY
		}

		// The name is never followed where it is: O_NOFOLLOW tells the
		// directory the call does not follow it either.
		follows := follow || s.slash

		if follows {
			fl &^= syscall.O_NOFOLLOW
		} else {
			fl |= syscall.O_NOFOLLOW
		}

		f, err := s.dir.openName(p, s.last, fl, mode)

		if err == syscall.ELOOP && follows {
			next, target, err := p.through(s, w)

			if err != nil {
				return nil, err
			}

			d, path = next, target

			continue
		}

		s.dir.unref()

		if err == syscall.ELOOP && flags&syscall.O_DIRECTORY != 0 {
			err = syscall.ENOTDIR
		}

		return f, err
	}
}

// open opens a guest path relative to dirfd and installs the file at the
// lowest descriptor.
func (p *Process) open(dirfd int32, path string, flags, va uint32) (int, error) {
	mode := uint32(0)

	if flags&(syscall.O_CREAT|0o20000000) != 0 {
		mode = p.varArg(va, 0) & 0o7777 &^ p.umask
	}

	f, err := p.openAt(dirfd, path, int(flags&^syscall.O_CLOEXEC), mode)

	if err != nil {
		return 0, err
	}

	return p.install(f, 0, flags&syscall.O_CLOEXEC != 0)
}

// openPath opens a guest path.
func (p *Process) openPath(path string, flags int, mode uint32) (File, error) {
	return p.openAt(atFdcwd, path, flags, mode)
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

// makeAt makes the last name of a path relative to dirfd: one that is
// there, ".", ".." and the root alone are EEXIST, and a slash after a
// name is for a directory only: EEXIST for one that is there, else
// ENOENT.
func (p *Process) makeAt(dirfd int32, path string, dir bool, make func(d Dir, name string) error) error {
	s, err := p.walkTo(dirfd, path)

	if err != nil {
		return err
	}

	defer s.dir.unref()

	if s.kind != lastName {
		return syscall.EEXIST
	}

	if s.slash && !dir {
		var st syscall.Stat_t

		if s.dir.statName(p, s.last, &st) == nil {
			return syscall.EEXIST
		}

		return syscall.ENOENT
	}

	return make(s.dir, s.last)
}

func (p *Process) mkdir(dirfd int32, path string, mode uint32) error {
	return p.makeAt(dirfd, path, true, func(d Dir, name string) error {
		return d.mkdir(p, name, mode&0o7777&^p.umask)
	})
}

func (p *Process) mknod(dirfd int32, path string, mode uint32, dev uint64) error {
	return p.makeAt(dirfd, path, false, func(d Dir, name string) error {
		return d.mknod(p, name, mode&^(p.umask&0o777), dev)
	})
}

func (p *Process) symlink(target string, dirfd int32, path string) error {
	if target == "" {
		return syscall.ENOENT
	}

	return p.makeAt(dirfd, path, false, func(d Dir, name string) error {
		return d.symlink(p, target, name)
	})
}

// remove takes the last name of a path relative to dirfd away, a
// directory's with dir. "." is EINVAL to rmdir, ".." ENOTEMPTY, the root
// alone EBUSY, and all three EISDIR to unlink; a slash after a name that
// is no directory is ENOTDIR to unlink, EISDIR after one that is.
func (p *Process) remove(dirfd int32, path string, dir bool) error {
	s, err := p.walkTo(dirfd, path)

	if err != nil {
		return err
	}

	defer s.dir.unref()

	switch {
	case s.kind == lastName:
	case !dir:
		return syscall.EISDIR
	case s.kind == lastDot:
		return syscall.EINVAL
	case s.kind == lastDotdot:
		return syscall.ENOTEMPTY
	default:
		return syscall.EBUSY
	}

	if s.slash && !dir {
		var st syscall.Stat_t

		if err := s.dir.statName(p, s.last, &st); err != nil {
			return err
		}

		if st.Mode&syscall.S_IFMT == syscall.S_IFDIR {
			return syscall.EISDIR
		}

		return syscall.ENOTDIR
	}

	return s.dir.unlink(p, s.last, dir)
}

// rename moves the last name of one path to the last name of another:
// ".", ".." and the root alone are EBUSY, a slash after either is for a
// directory only.
func (p *Process) rename(olddir int32, old string, newdir int32, new string) error {
	a, err := p.walkTo(olddir, old)

	if err != nil {
		return err
	}

	defer a.dir.unref()
	b, err := p.walkTo(newdir, new)

	if err != nil {
		return err
	}

	defer b.dir.unref()

	if a.kind != lastName || b.kind != lastName {
		return syscall.EBUSY
	}

	if a.slash || b.slash {
		var st syscall.Stat_t

		if err := a.dir.statName(p, a.last, &st); err != nil {
			return err
		}

		if st.Mode&syscall.S_IFMT != syscall.S_IFDIR {
			return syscall.ENOTDIR
		}
	}

	return a.dir.rename(p, a.last, b.dir, b.last)
}

// link makes the last name of a new path a name of what an old one
// names, a last link of it followed when follow says: a directory is
// EPERM, and the new name is made as makeAt makes one.
func (p *Process) link(olddir int32, old string, newdir int32, new string, follow bool) error {
	return p.onName(olddir, old, follow, func(d Dir, name string, st *syscall.Stat_t) error {
		if name == "" || st.Mode&syscall.S_IFMT == syscall.S_IFDIR {
			return syscall.EPERM
		}

		return p.makeAt(newdir, new, false, func(to Dir, toName string) error {
			return d.link(p, name, to, toName)
		})
	})
}

// statPath is the metadata of what a path names, or of a last symbolic
// link itself.
func (p *Process) statPath(path string, follow bool, st *syscall.Stat_t) error {
	return p.onName(atFdcwd, path, follow, func(d Dir, name string, found *syscall.Stat_t) error {
		*st = *found

		return nil
	})
}

// accessAt checks what a path names for mode, as access does, or with
// AT_SYMLINK_NOFOLLOW a last link itself.
func (p *Process) accessAt(dirfd int32, path string, mode, flags uint32) error {
	return p.onName(dirfd, path, flags&atSymlinkNofollow == 0, func(d Dir, name string, st *syscall.Stat_t) error {
		return d.accessName(p, name, mode, flags)
	})
}

// chmodName changes the mode of a name: a symbolic link has none to
// change, EOPNOTSUPP as Linux's fchmodat says.
func (p *Process) chmodName(d Dir, name string, st *syscall.Stat_t, mode uint32) error {
	if st.Mode&syscall.S_IFMT == syscall.S_IFLNK {
		return syscall.EOPNOTSUPP
	}

	return d.chmodName(p, name, mode&0o7777)
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

// statfsPath is the file system of what a path names.
func (p *Process) statfsPath(path string, st *syscall.Statfs_t) error {
	return p.onName(atFdcwd, path, true, func(d Dir, name string, _ *syscall.Stat_t) error {
		return d.statfsName(p, name, st)
	})
}

func (p *Process) libcOpen(path, flags, va uint32) uint32 {
	return p.result(p.open(atFdcwd, p.str(path), flags, va))
}

func (p *Process) libcOpenat(dirfd, path, flags, va uint32) uint32 {
	return p.result(p.open(int32(dirfd), p.str(path), flags, va))
}

func (p *Process) libcFstatat(dirfd, path, at, flags uint32) uint32 {
	var st syscall.Stat_t
	err := p.onAt(int32(dirfd), p.str(path), flags, func(d Dir, name string, found *syscall.Stat_t) error {
		st = *found

		return nil
	}, func(f File) error {
		return statFile(f, &st)
	})

	if err == nil {
		p.putStat(at, &st)
	}

	return p.result(0, err)
}

func (p *Process) libcAccess(path, mode uint32) uint32 {
	return p.result(0, p.accessAt(atFdcwd, p.str(path), mode, 0))
}

// faccessat of a descriptor's own file: a file of the kernel is the
// process's to use.
func (p *Process) libcFaccessat(dirfd, path, mode, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(d Dir, name string, st *syscall.Stat_t) error {
		return d.accessName(p, name, mode, flags)
	}, func(f File) error {
		if n, ok := f.(Node); ok {
			return n.access(mode, flags)
		}

		return nil
	}))
}

func (p *Process) libcTruncate(path uint32, size uint64) uint32 {
	return p.result(0, p.onName(atFdcwd, p.str(path), true, func(d Dir, name string, st *syscall.Stat_t) error {
		return d.truncateName(p, name, int64(size))
	}))
}

func (p *Process) libcStatvfs(path, at uint32) uint32 {
	var st syscall.Statfs_t
	err := p.statfsPath(p.str(path), &st)

	if err == nil {
		p.putStatvfs(at, &st)
	}

	return p.result(0, err)
}

func (p *Process) libcFstatvfs(fd, at uint32) uint32 {
	n, err := p.attrsAt(fd)

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

// setCwd makes d, whose reference it takes, the working directory.
func (p *Process) setCwd(d Dir) {
	old := p.cwd
	p.cwd = d

	if old != nil {
		old.unref()
	}
}

// dirOf is the directory a name of d is, held: for a working directory
// or a root. "" is d itself.
func (p *Process) dirOf(d Dir, name string) (Dir, error) {
	if name == "" {
		d.ref()

		return d, nil
	}

	f, err := d.openName(p, name, oPath|syscall.O_DIRECTORY|syscall.O_NOFOLLOW, 0)

	if err != nil {
		return nil, err
	}

	nd, ok := f.(Dir)

	if !ok {
		f.unref()

		return nil, syscall.ENOTDIR
	}

	return nd, nil
}

// directory is the directory a path names, one the process may search,
// held: for a working directory or a root.
func (p *Process) directory(path string) (Dir, error) {
	var nd Dir
	err := p.onName(atFdcwd, path, true, func(d Dir, name string, st *syscall.Stat_t) error {
		if st.Mode&syscall.S_IFMT != syscall.S_IFDIR {
			return syscall.ENOTDIR
		}

		if err := d.accessName(p, name, 1, 0); err != nil {
			return err
		}

		var err error
		nd, err = p.dirOf(d, name)

		return err
	})

	return nd, err
}

// chdir makes a directory the working directory: the directory itself,
// which a rename does not change and a removal leaves without a path.
func (p *Process) chdir(path string) error {
	d, err := p.directory(path)

	if err != nil {
		return err
	}

	p.setCwd(d)

	return nil
}

func (p *Process) libcChdir(path uint32) uint32 {
	return p.result(0, p.chdir(p.str(path)))
}

// fchdir makes a directory descriptor's directory the working directory;
// a connection to a server is a directory of that server, its root,
// which chroot(".") then takes.
func (p *Process) libcFchdir(fd uint32) uint32 {
	obj, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	d := asRoot(obj)

	if d == nil {
		return p.result(0, syscall.ENOTDIR)
	}

	if err := d.accessName(p, "", 1, 0); err != nil {
		d.unref()

		return p.result(0, err)
	}

	p.setCwd(d)

	return p.result(0, nil)
}

// chroot makes a directory the process's root: ".." stops there and
// absolute paths start there from then on. The working directory stays
// what it is, as Linux leaves it.
func (p *Process) libcChroot(path uint32) uint32 {
	d, err := p.directory(p.str(path))

	if err != nil {
		return p.result(0, err)
	}

	rootPath := ""

	if pathed, ok := d.(Pathed); ok {
		host, err := pathed.hostPath()

		if err != nil {
			d.unref()

			return p.result(0, err)
		}

		rootPath = host
	}

	p.root.unref()
	p.root, p.rootPath = d, rootPath

	return p.result(0, nil)
}

// cwdPath is the working directory's path: none past PATH_MAX, as
// Linux's getcwd has none.
func (p *Process) cwdPath() (string, error) {
	cwd, err := p.cwd.path(p)

	if err == nil && len(cwd) >= 4096 {
		err = syscall.ENAMETOOLONG
	}

	return cwd, err
}

func (p *Process) libcGetcwd(buf, size uint32) uint32 {
	cwd, err := p.cwdPath()

	if err != nil {
		return p.pointer(0, err)
	}

	if buf == 0 {
		at := p.newString(cwd)

		if at == 0 {
			return p.pointer(0, syscall.ENOMEM)
		}

		return p.pointer(at, nil)
	}

	if uint32(len(cwd)) >= size {
		return p.pointer(0, syscall.ERANGE)
	}

	p.putString(buf, cwd)

	return p.pointer(buf, nil)
}

func (p *Process) libcMkdir(path, mode uint32) uint32 {
	return p.result(0, p.mkdir(atFdcwd, p.str(path), mode))
}

func (p *Process) libcMkdirat(dirfd, path, mode uint32) uint32 {
	return p.result(0, p.mkdir(int32(dirfd), p.str(path), mode))
}

func (p *Process) libcRmdir(path uint32) uint32 {
	return p.result(0, p.remove(atFdcwd, p.str(path), true))
}

func (p *Process) libcUnlink(path uint32) uint32 {
	return p.result(0, p.remove(atFdcwd, p.str(path), false))
}

func (p *Process) libcUnlinkat(dirfd, path, flags uint32) uint32 {
	if flags&^atRemovedir != 0 {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(0, p.remove(int32(dirfd), p.str(path), flags&atRemovedir != 0))
}

// remove unlinks a file or empties a directory, whichever it is.
func (p *Process) libcRemove(path uint32) uint32 {
	err := p.remove(atFdcwd, p.str(path), false)

	if err == syscall.EISDIR || err == syscall.EPERM {
		err = p.remove(atFdcwd, p.str(path), true)
	}

	return p.result(0, err)
}

func (p *Process) libcRename(old, new uint32) uint32 {
	return p.result(0, p.rename(atFdcwd, p.str(old), atFdcwd, p.str(new)))
}

func (p *Process) libcRenameat(olddir, old, newdir, new uint32) uint32 {
	return p.result(0, p.rename(int32(olddir), p.str(old), int32(newdir), p.str(new)))
}

func (p *Process) libcLink(old, new uint32) uint32 {
	return p.result(0, p.link(atFdcwd, p.str(old), atFdcwd, p.str(new), false))
}

func (p *Process) libcLinkat(olddir, old, newdir, new, flags uint32) uint32 {
	if flags&^atSymlinkFollow != 0 {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(0, p.link(int32(olddir), p.str(old), int32(newdir), p.str(new), flags&atSymlinkFollow != 0))
}

func (p *Process) libcSymlink(target, path uint32) uint32 {
	return p.result(0, p.symlink(p.str(target), atFdcwd, p.str(path)))
}

func (p *Process) libcSymlinkat(target, dirfd, path uint32) uint32 {
	return p.result(0, p.symlink(p.str(target), int32(dirfd), p.str(path)))
}

// readlinkAt writes a link's target, as much as the buffer holds: what is
// no link is EINVAL, a slash after one follows it.
func (p *Process) readlinkAt(dirfd int32, path string, buf, size uint32) (int, error) {
	var target string
	err := p.onName(dirfd, path, false, func(d Dir, name string, st *syscall.Stat_t) error {
		if name == "" || st.Mode&syscall.S_IFMT != syscall.S_IFLNK {
			return syscall.EINVAL
		}

		var err error
		target, err = d.readlinkName(p, name)

		return err
	})

	if err != nil {
		return 0, err
	}

	return copy(p.span(buf, size), target), nil
}

func (p *Process) libcReadlink(path, buf, size uint32) uint32 {
	return p.result(p.readlinkAt(atFdcwd, p.str(path), buf, size))
}

// readlinkat with an empty path reads the link a descriptor is: what is
// no link is ENOENT then, as Linux has it, not the named path's EINVAL.
func (p *Process) libcReadlinkat(dirfd, path, buf, size uint32) uint32 {
	if p.u8(path) == 0 {
		obj, err := p.fd(dirfd)

		if dirfd == uint32(atFdcwd&0xffffffff) {
			obj, err = p.cwd, nil
		}

		if err != nil {
			return p.result(0, err)
		}

		n, err := 0, error(syscall.ENOENT)

		if node, ok := obj.(Node); ok {
			if n, err = node.readlink(p.span(buf, size)); err == syscall.EINVAL {
				err = syscall.ENOENT
			}
		}

		return p.result(n, err)
	}

	return p.result(p.readlinkAt(int32(dirfd), p.str(path), buf, size))
}

// realpath is the path from the root of what a path names, with no ".",
// ".." or link in it: its directory's path and its name, or a
// directory's own.
func (p *Process) realpath(path string) (string, error) {
	var out string
	err := p.onName(atFdcwd, path, true, func(d Dir, name string, st *syscall.Stat_t) error {
		if name == "" || st.Mode&syscall.S_IFMT == syscall.S_IFDIR {
			nd, err := p.dirOf(d, name)

			if err != nil {
				return err
			}

			defer nd.unref()
			out, err = nd.path(p)

			return err
		}

		base, err := d.path(p)
		out = strings.TrimSuffix(base, "/") + "/" + name

		return err
	})

	if err == nil && len(out) >= 4096 {
		err = syscall.ENAMETOOLONG
	}

	return out, err
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
	cwd, err := p.cwdPath()

	if err != nil {
		return p.pointer(0, err)
	}

	at := p.newString(cwd)

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
	return p.result(0, p.accessAt(atFdcwd, p.str(path), mode, atEaccess))
}

func (p *Process) libcEuidaccess(path, mode uint32) uint32 {
	return p.result(0, p.accessAt(atFdcwd, p.str(path), mode, atEaccess))
}

func (p *Process) libcStatfs(path, at uint32) uint32 {
	var st syscall.Statfs_t
	err := p.statfsPath(p.str(path), &st)

	if err == nil {
		p.putStatfsOf(at, &st)
	}

	return p.result(0, err)
}

func (p *Process) libcFstatfs(fd, at uint32) uint32 {
	n, err := p.attrsAt(fd)

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

	p.putString(resolved, guest)

	return p.pointer(resolved, nil)
}

func (p *Process) libcChmod(path, mode uint32) uint32 {
	return p.result(0, p.onName(atFdcwd, p.str(path), true, func(d Dir, name string, st *syscall.Stat_t) error {
		return p.chmodName(d, name, st, mode)
	}))
}

func (p *Process) libcFchmodat(dirfd, path, mode, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(d Dir, name string, st *syscall.Stat_t) error {
		return p.chmodName(d, name, st, mode)
	}, attrsCall(func(a Attrs) error {
		return a.chmod(mode & 0o7777)
	})))
}

func (p *Process) libcFchownat(dirfd, path, uid, gid, flags uint32) uint32 {
	return p.result(0, p.onAt(int32(dirfd), p.str(path), flags, func(d Dir, name string, st *syscall.Stat_t) error {
		return d.chownName(p, name, int(int32(uid)), int(int32(gid)))
	}, attrsCall(func(a Attrs) error {
		return a.chown(int(int32(uid)), int(int32(gid)))
	})))
}

func (p *Process) libcMknod(path, mode uint32, dev uint64) uint32 {
	return p.result(0, p.mknod(atFdcwd, p.str(path), mode, dev))
}

func (p *Process) libcMknodat(dirfd, path, mode uint32, dev uint64) uint32 {
	return p.result(0, p.mknod(int32(dirfd), p.str(path), mode, dev))
}

func (p *Process) libcMkfifo(path, mode uint32) uint32 {
	return p.result(0, p.mknod(atFdcwd, p.str(path), sIfifo|mode&0o7777, 0))
}

func (p *Process) libcMkfifoat(dirfd, path, mode uint32) uint32 {
	return p.result(0, p.mknod(int32(dirfd), p.str(path), sIfifo|mode&0o7777, 0))
}

func (p *Process) libcUtimes(path, times uint32) uint32 {
	return p.result(0, p.utimens(atFdcwd, p.str(path), p.timevals(times), true))
}

func (p *Process) libcLutimes(path, times uint32) uint32 {
	return p.result(0, p.utimens(atFdcwd, p.str(path), p.timevals(times), false))
}

func (p *Process) libcUtime(path, times uint32) uint32 {
	var ts []syscall.Timespec

	if times != 0 {
		ts = []syscall.Timespec{{Sec: int64(p.u64(times))}, {Sec: int64(p.u64(times + 8))}}
	}

	return p.result(0, p.utimens(atFdcwd, p.str(path), ts, true))
}

// utimensat of a null path sets the times of the descriptor's own file,
// as Linux's does.
func (p *Process) libcUtimensat(dirfd, path, times, flags uint32) uint32 {
	if path == 0 {
		n, err := p.attrsAt(dirfd)

		if err == nil {
			err = n.utimes(p.timespecs(times))
		}

		return p.result(0, err)
	}

	return p.result(0, p.utimens(int32(dirfd), p.str(path), p.timespecs(times), flags&atSymlinkNofollow == 0))
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
		return p.makeAt(atFdcwd, path, true, func(d Dir, name string) error {
			return d.mkdir(p, name, 0o700)
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
	return p.result(0, p.onName(atFdcwd, p.str(path), follow, func(d Dir, name string, st *syscall.Stat_t) error {
		return d.chownName(p, name, int(int32(uid)), int(int32(gid)))
	}))
}

// utimens sets the times of what a path names, or of a last symbolic
// link itself.
func (p *Process) utimens(dirfd int32, path string, ts []syscall.Timespec, follow bool) error {
	return p.onName(dirfd, path, follow, func(d Dir, name string, st *syscall.Stat_t) error {
		return d.utimesName(p, name, ts)
	})
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

// unixPath is the host's address of a guest path of a socket: a socket of
// a root of the host, by the descriptor of its directory; bind makes the
// last name, connect finds what the path names. A path of a server names
// no socket of the host: EOPNOTSUPP.
func (p *Process) unixPath(guest string, bind bool, call func(host string) error) error {
	if bind {
		s, err := p.walkTo(atFdcwd, guest)

		if err != nil {
			return err
		}

		defer s.dir.unref()
		d, ok := s.dir.(*hostDir)

		if !ok {
			return syscall.EOPNOTSUPP
		}

		if s.kind != lastName {
			return syscall.EADDRINUSE
		}

		return call(fmt.Sprintf("/proc/self/fd/%d/%s", d.fd, s.last))
	}

	return p.onName(atFdcwd, guest, true, func(d Dir, name string, st *syscall.Stat_t) error {
		if _, ok := d.(*hostDir); !ok {
			return syscall.EOPNOTSUPP
		}

		f, err := d.openName(p, name, oPath|syscall.O_NOFOLLOW, 0)

		if err != nil {
			return err
		}

		defer f.unref()
		fd, _ := hostFdOf(f)

		return call(procFd(fd))
	})
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
