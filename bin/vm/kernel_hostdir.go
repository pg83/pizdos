//go:build linux && amd64

package main

import (
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

// A directory of the host resolves paths on the host: openat2 keeps them
// inside it, symbolic links and ".." included. A call on what a path
// names opens it, O_PATH, and works on that descriptor; a call on the name
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

// hostDevices are the host's devices every host root has, with its
// pseudoterminals: no other path leaves it.
var hostDevices = map[string]bool{
	"/dev/null": true, "/dev/zero": true, "/dev/full": true, "/dev/random": true, "/dev/urandom": true, "/dev/tty": true,
	"/dev/ptmx": true,
}

func hostDevice(path string) bool {
	return hostDevices[path] || strings.HasPrefix(path, "/dev/pts/") && len(path) > len("/dev/pts/") && !strings.Contains(path[len("/dev/pts/"):], "/")
}

// openat2 opens a path from the directory; the host descriptor is
// close-on-exec.
func (d *hostDir) openat2(path string, flags int, mode uint32) (int, error) {
	if chaosFail("openat") {
		return -1, syscall.EIO
	}

	if clean := filepath.Clean(path); hostDevice(clean) {
		for {
			fd, err := syscall.Open(clean, flags|syscall.O_CLOEXEC, mode)

			if err != syscall.EINTR {
				return fd, err
			}
		}
	}

	name := throw2(syscall.BytePtrFromString(path))
	how := openHow{flags: uint64(flags | syscall.O_CLOEXEC), mode: uint64(mode), resolve: resolveInRoot}

	for {
		r, _, e := syscall.Syscall6(sysOpenat2, uintptr(d.fd), uintptr(unsafe.Pointer(name)), uintptr(unsafe.Pointer(&how)), unsafe.Sizeof(how), 0, 0)

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
func (d *hostDir) opath(path string, follow bool) (int, error) {
	flags := oPath

	if !follow {
		flags |= syscall.O_NOFOLLOW
	}

	return d.openat2(path, flags, 0)
}

// parent opens the directory a path's last component is in and returns
// that name. A path naming the root, "." or ".." has no name in a parent:
// the name is "" and the descriptor is the path's own.
func (d *hostDir) parent(path string) (int, string, error) {
	for len(path) > 1 && path[len(path)-1] == '/' {
		path = path[:len(path)-1]
	}

	at := strings.LastIndexByte(path, '/')
	dir, name := path[:at], path[at+1:]

	if dir == "" {
		dir = "/"
	}

	if name == "" || name == "." || name == ".." {
		fd, err := d.openat2(path, oPath, 0)

		return fd, "", err
	}

	fd, err := d.openat2(dir, oPath|syscall.O_DIRECTORY, 0)

	return fd, name, err
}

// onName runs a call on a path's name in its parent directory; on a path
// without one it fails with noName.
func (d *hostDir) onName(path string, noName syscall.Errno, call func(dirfd int, name string) error) error {
	dirfd, name, err := d.parent(path)

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
func (d *hostDir) onPath(path string, follow bool, call func(fd int) error) error {
	fd, err := d.opath(path, follow)

	if err != nil {
		return err
	}

	defer syscall.Close(fd)

	return call(fd)
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

// The Dir role: the paths are absolute, from this directory.

// name is the directory's guest path in the process's root.
func (d *hostDir) name(p *Process) (string, error) {
	host, err := d.hostPath()

	if err != nil {
		return "", err
	}

	return p.guestPath(host), nil
}

func (d *hostDir) open(p *Process, path string, flags int, mode uint32) (File, error) {
	fd, err := d.openat2(path, flags, mode)

	if err != nil {
		return nil, err
	}

	return newHostObject(fd, true), nil
}

func (d *hostDir) statPath(p *Process, path string, follow bool, st *syscall.Stat_t) error {
	return d.onPath(path, follow, func(fd int) error {
		return fileStat(fd, st)
	})
}

func (d *hostDir) readlinkPath(p *Process, path string) (string, error) {
	buf := make([]byte, 4096)
	n := 0
	err := d.onPath(path, false, func(fd int) error {
		var e error
		n, e = readlinkFd(fd, buf, true)

		return e
	})

	return string(buf[:n]), err
}

func (d *hostDir) mkdir(p *Process, path string, mode uint32) error {
	return d.onName(path, syscall.EEXIST, func(dirfd int, name string) error {
		return syscall.Mkdirat(dirfd, name, mode)
	})
}

// unlink removes a name, or with dir an empty directory.
func (d *hostDir) unlink(p *Process, path string, dir bool) error {
	noName, flags := syscall.EISDIR, 0

	if dir {
		noName, flags = syscall.EBUSY, atRemovedir
	}

	return d.onName(path, noName, func(dirfd int, name string) error {
		return unlinkat(dirfd, name, flags)
	})
}

func (d *hostDir) rename(p *Process, old, new string) error {
	return d.onName(old, syscall.EBUSY, func(olddir int, oldName string) error {
		return d.onName(new, syscall.EBUSY, func(newdir int, newName string) error {
			return syscall.Renameat(olddir, oldName, newdir, newName)
		})
	})
}

// link makes a new name of old, or with follow of what a symbolic link old
// names.
func (d *hostDir) link(p *Process, old, new string, follow bool) error {
	if follow {
		return d.onPath(old, true, func(fd int) error {
			return d.onName(new, syscall.EEXIST, func(newdir int, newName string) error {
				name := throw2(syscall.BytePtrFromString(newName))

				return atCall(syscall.SYS_LINKAT, atFdcwd, procFd(fd), uintptr(newdir), uintptr(unsafe.Pointer(name)), atSymlinkFollow)
			})
		})
	}

	return d.onName(old, syscall.EPERM, func(olddir int, oldName string) error {
		return d.onName(new, syscall.EEXIST, func(newdir int, newName string) error {
			name := throw2(syscall.BytePtrFromString(newName))

			return atCall(syscall.SYS_LINKAT, olddir, oldName, uintptr(newdir), uintptr(unsafe.Pointer(name)), 0)
		})
	})
}

func (d *hostDir) symlink(p *Process, target, path string) error {
	t := throw2(syscall.BytePtrFromString(target))

	return d.onName(path, syscall.EEXIST, func(dirfd int, name string) error {
		n := throw2(syscall.BytePtrFromString(name))

		if _, _, e := syscall.Syscall(syscall.SYS_SYMLINKAT, uintptr(unsafe.Pointer(t)), uintptr(dirfd), uintptr(unsafe.Pointer(n))); e != 0 {
			return e
		}

		return nil
	})
}

func (d *hostDir) mknod(p *Process, path string, mode uint32, dev uint64) error {
	return d.onName(path, syscall.EEXIST, func(dirfd int, name string) error {
		return syscall.Mknodat(dirfd, name, mode, int(dev))
	})
}

func (d *hostDir) accessPath(p *Process, path string, mode, flags uint32) error {
	return d.onPath(path, true, func(fd int) error {
		return access(fd, mode, flags)
	})
}

func (d *hostDir) chmodPath(p *Process, path string, mode uint32) error {
	return d.onPath(path, true, func(fd int) error {
		return atCall(sysFchmodat2, fd, "", uintptr(mode), atEmptyPath)
	})
}

func (d *hostDir) chownPath(p *Process, path string, uid, gid int, follow bool) error {
	return d.onPath(path, follow, func(fd int) error {
		return syscall.Fchownat(fd, "", uid, gid, atEmptyPath)
	})
}

// utimesPath sets the times of a path: what it names through /proc, a
// last symbolic link itself in its parent.
func (d *hostDir) utimesPath(p *Process, path string, ts []syscall.Timespec, follow bool) error {
	if !follow {
		return d.onName(path, syscall.EINVAL, func(dirfd int, name string) error {
			return utimensat(dirfd, throw2(syscall.BytePtrFromString(name)), ts, atSymlinkNofollow)
		})
	}

	return d.onPath(path, true, func(fd int) error {
		return utimensat(atFdcwd, throw2(syscall.BytePtrFromString(procFd(fd))), ts, 0)
	})
}

func (d *hostDir) truncatePath(p *Process, path string, size int64) error {
	return d.onPath(path, true, func(fd int) error {
		return syscall.Truncate(procFd(fd), size)
	})
}

func (d *hostDir) statfsPath(p *Process, path string, st *syscall.Statfs_t) error {
	return d.onPath(path, true, func(fd int) error {
		return syscall.Fstatfs(fd, st)
	})
}

// realpath is the guest path a path names.
func (d *hostDir) realpath(p *Process, path string) (string, error) {
	var guest string
	err := d.onPath(path, true, func(fd int) error {
		host, err := hostPath(fd)
		guest = p.guestPath(host)

		return err
	})

	return guest, err
}
