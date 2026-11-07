//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"math/rand/v2"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

// Paths are the guest's: absolute ones start at the root, relative ones at
// the working directory, and openat2 keeps both inside the root, symbolic
// links and ".." included. A call on what a path names opens it, O_PATH,
// and works on that descriptor; a call on the name itself (unlink, mkdir,
// rename) opens the parent directory and works on the name in it.

const (
	oPath             = 0o10000000
	sysOpenat2        = 437
	sysFaccessat2     = 439
	sysFchmodat2      = 452
	resolveInRoot     = 0x10
	atFdcwd           = -100
	atSymlinkNofollow = 0x100
	atRemovedir       = 0x200
	atEmptyPath       = 0x1000
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

// hostDevices are the host's devices every root has: no other path leaves
// it.
var hostDevices = map[string]bool{
	"/dev/null": true, "/dev/zero": true, "/dev/full": true, "/dev/random": true, "/dev/urandom": true, "/dev/tty": true,
}

// openat2 opens a guest path; the host descriptor is close-on-exec.
func (p *Process) openat2(path string, flags int, mode uint32) (int, error) {
	full, err := p.full(path)

	if err != nil {
		return -1, err
	}

	if clean := filepath.Clean(full); hostDevices[clean] {
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
		r, _, e := syscall.Syscall6(sysOpenat2, uintptr(p.k.root), uintptr(unsafe.Pointer(name)), uintptr(unsafe.Pointer(&how)), unsafe.Sizeof(how), 0, 0)

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

	f, err := p.fd(uint32(dirfd))

	if err != nil {
		return "", err
	}

	dir, err := p.k.hostPath(f.fd)

	if err != nil {
		return "", err
	}

	return p.k.guestPath(dir) + "/" + path, nil
}

// hostPath is the host path of a host descriptor.
func (k *Kernel) hostPath(fd int) (string, error) {
	return os.Readlink(fmt.Sprintf("/proc/self/fd/%d", fd))
}

func procFd(fd int) string {
	return fmt.Sprintf("/proc/self/fd/%d", fd)
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

func (k *Kernel) pathFunctions() map[string]any {
	return map[string]any{
		"open": func(ctx uintptr, path, flags, va uint32) uint32 {
			p := proc(ctx)
			mode := uint32(0)

			if flags&(syscall.O_CREAT|0o20000000) != 0 {
				mode = p.varArg(va, 0) & 0o7777 &^ p.umask
			}

			fd, err := p.openat2(p.str(path), int(flags&^syscall.O_CLOEXEC), mode)

			if err != nil {
				return p.result(0, err)
			}

			return p.result(p.install(newFile(fd, true), 0, flags&syscall.O_CLOEXEC != 0))
		},
		"stat": func(ctx uintptr, path, at uint32) uint32 {
			return proc(ctx).stat(path, at, true)
		},
		"lstat": func(ctx uintptr, path, at uint32) uint32 {
			return proc(ctx).stat(path, at, false)
		},
		"access": func(ctx uintptr, path, mode uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
				return atCall(sysFaccessat2, fd, "", uintptr(mode), atEmptyPath)
			}))
		},
		"chdir": func(ctx uintptr, path uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.chdir(p.str(path)))
		},
		"getcwd": func(ctx uintptr, buf, size uint32) uint32 {
			p := proc(ctx)

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
		},
		"mkdir": func(ctx uintptr, path, mode uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.onName(p.str(path), syscall.EEXIST, func(dirfd int, name string) error {
				return syscall.Mkdirat(dirfd, name, mode&0o7777&^p.umask)
			}))
		},
		"rmdir": func(ctx uintptr, path uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.onName(p.str(path), syscall.EBUSY, func(dirfd int, name string) error {
				return unlinkat(dirfd, name, atRemovedir)
			}))
		},
		"unlink": func(ctx uintptr, path uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.onName(p.str(path), syscall.EISDIR, func(dirfd int, name string) error {
				return unlinkat(dirfd, name, 0)
			}))
		},
		"rename": func(ctx uintptr, old, new uint32) uint32 {
			p := proc(ctx)
			newPath := p.str(new)

			return p.result(0, p.onName(p.str(old), syscall.EBUSY, func(olddir int, oldName string) error {
				return p.onName(newPath, syscall.EBUSY, func(newdir int, newName string) error {
					return syscall.Renameat(olddir, oldName, newdir, newName)
				})
			}))
		},
		"link": func(ctx uintptr, old, new uint32) uint32 {
			p := proc(ctx)
			newPath := p.str(new)

			return p.result(0, p.onName(p.str(old), syscall.EPERM, func(olddir int, oldName string) error {
				return p.onName(newPath, syscall.EEXIST, func(newdir int, newName string) error {
					name := throw2(syscall.BytePtrFromString(newName))

					return atCall(syscall.SYS_LINKAT, olddir, oldName, uintptr(newdir), uintptr(unsafe.Pointer(name)), 0)
				})
			}))
		},
		"symlink": func(ctx uintptr, target, path uint32) uint32 {
			p := proc(ctx)
			t := throw2(syscall.BytePtrFromString(p.str(target)))

			return p.result(0, p.onName(p.str(path), syscall.EEXIST, func(dirfd int, name string) error {
				n := throw2(syscall.BytePtrFromString(name))

				if _, _, e := syscall.Syscall(syscall.SYS_SYMLINKAT, uintptr(unsafe.Pointer(t)), uintptr(dirfd), uintptr(unsafe.Pointer(n))); e != 0 {
					return e
				}

				return nil
			}))
		},
		"readlink": func(ctx uintptr, path, buf, size uint32) uint32 {
			p := proc(ctx)
			out := p.span(buf, size)
			n := 0
			err := p.onPath(p.str(path), false, func(fd int) error {
				var e error
				n, e = readlinkFd(fd, out)

				return e
			})

			return p.result(n, err)
		},
		"realpath": func(ctx uintptr, path, resolved uint32) uint32 {
			p := proc(ctx)
			var guest string
			err := p.onPath(p.str(path), true, func(fd int) error {
				host, err := p.k.hostPath(fd)
				guest = p.k.guestPath(host)

				return err
			})

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

			if len(guest) >= 4096 {
				return p.pointer(0, syscall.ENAMETOOLONG)
			}

			p.putString(resolved, guest)

			return p.pointer(resolved, nil)
		},
		"chmod": func(ctx uintptr, path, mode uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.onPath(p.str(path), true, func(fd int) error {
				return atCall(sysFchmodat2, fd, "", uintptr(mode&0o7777), atEmptyPath)
			}))
		},
		"chown": func(ctx uintptr, path, uid, gid uint32) uint32 {
			return proc(ctx).chown(path, uid, gid, true)
		},
		"lchown": func(ctx uintptr, path, uid, gid uint32) uint32 {
			return proc(ctx).chown(path, uid, gid, false)
		},
		"mknod": func(ctx uintptr, path, mode uint32, dev uint64) uint32 {
			p := proc(ctx)

			return p.result(0, p.onName(p.str(path), syscall.EEXIST, func(dirfd int, name string) error {
				return syscall.Mknodat(dirfd, name, mode&^(p.umask&0o777), int(dev))
			}))
		},
		"utimes": func(ctx uintptr, path, times uint32) uint32 {
			p := proc(ctx)
			var ts []syscall.Timespec

			if times != 0 {
				b := p.span(times, 32)
				le := binary.LittleEndian

				for i := range 2 {
					ts = append(ts, syscall.Timespec{Sec: int64(le.Uint64(b[16*i:])), Nsec: 1000 * int64(le.Uint64(b[16*i+8:]))})
				}
			}

			return p.result(0, p.utimens(p.str(path), ts, 0))
		},
		"utimensat": func(ctx uintptr, dirfd, path, times, flags uint32) uint32 {
			p := proc(ctx)
			full, err := p.atPath(int32(dirfd), p.str(path))

			if err == nil {
				err = p.utimens(full, p.timespecs(times), int(flags))
			}

			return p.result(0, err)
		},
		"opendir":  k.opendir,
		"readdir":  k.readdir,
		"closedir": k.closedir,
		"mkstemp": func(ctx uintptr, template uint32) uint32 {
			p := proc(ctx)
			fd := -1
			err := p.temporary(template, func(path string) error {
				host, err := p.openat2(path, syscall.O_RDWR|syscall.O_CREAT|syscall.O_EXCL, 0o600)

				if err == nil {
					fd, err = p.install(newFile(host, true), 0, false)
				}

				return err
			})

			return p.result(fd, err)
		},
		"mkdtemp": func(ctx uintptr, template uint32) uint32 {
			p := proc(ctx)
			err := p.temporary(template, func(path string) error {
				return p.onName(path, syscall.EEXIST, func(dirfd int, name string) error {
					return syscall.Mkdirat(dirfd, name, 0o700)
				})
			})

			return p.pointer(template, err)
		},
		"mktemp": func(ctx uintptr, template uint32) uint32 {
			p := proc(ctx)
			err := p.temporary(template, func(path string) error {
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
			}

			return p.pointer(template, nil)
		},
	}
}

func (p *Process) stat(path, at uint32, follow bool) uint32 {
	var st syscall.Stat_t
	err := p.onPath(p.str(path), follow, func(fd int) error {
		return syscall.Fstat(fd, &st)
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

	host, err := p.k.hostPath(fd)

	if err != nil {
		return err
	}

	p.cwd = p.k.guestPath(host)

	return nil
}

func readlinkFd(fd int, out []byte) (int, error) {
	var buf uintptr

	if len(out) > 0 {
		buf = uintptr(unsafe.Pointer(&out[0]))
	}

	empty := [1]byte{}
	r, _, e := syscall.Syscall6(syscall.SYS_READLINKAT, uintptr(fd), uintptr(unsafe.Pointer(&empty[0])), buf, uintptr(len(out)), 0, 0)

	if e != 0 {
		return 0, e
	}

	return int(r), nil
}

const tempLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

// temporary fills the XXXXXX that ends a template with random letters
// until create makes what the path names, EEXIST meaning another try.
func (p *Process) temporary(template uint32, create func(path string) error) error {
	t := p.cstr(template)

	if len(t) < 6 || string(t[len(t)-6:]) != "XXXXXX" {
		return syscall.EINVAL
	}

	for range 100 {
		for i := len(t) - 6; i < len(t); i++ {
			t[i] = tempLetters[rand.IntN(len(tempLetters))]
		}

		if err := create(string(t)); err != syscall.EEXIST {
			return err
		}
	}

	return syscall.EEXIST
}

// A DIR is a block of the heap: the descriptor, the position and end of
// the entries in the buffer, then the buffer, which getdents64 fills with
// entries of the guest's struct dirent layout.
const (
	dirFd     = 0
	dirPos    = 4
	dirEnd    = 8
	dirBuf    = 16
	dirBufLen = 4096
)

func (k *Kernel) opendir(ctx uintptr, path uint32) uint32 {
	p := proc(ctx)
	host, err := p.openat2(p.str(path), syscall.O_RDONLY|syscall.O_DIRECTORY, 0)

	if err != nil {
		return p.pointer(0, err)
	}

	fd, err := p.install(newFile(host, true), 0, true)

	if err != nil {
		return p.pointer(0, err)
	}

	dir := p.malloc(dirBuf + dirBufLen)

	if dir == 0 {
		p.closeFd(fd)

		return p.pointer(0, syscall.ENOMEM)
	}

	p.put32(dir+dirFd, uint32(fd))
	p.put32(dir+dirPos, 0)
	p.put32(dir+dirEnd, 0)

	return p.pointer(dir, nil)
}

func (k *Kernel) readdir(ctx uintptr, dir uint32) uint32 {
	p := proc(ctx)
	pos, end := p.u32(dir+dirPos), p.u32(dir+dirEnd)

	if pos >= end {
		f, err := p.fd(p.u32(dir + dirFd))

		if err != nil {
			return p.pointer(0, err)
		}

		n, err := syscall.Getdents(f.fd, p.span(dir+dirBuf, dirBufLen))

		if err != nil {
			return p.pointer(0, err)
		}

		if n == 0 {
			p.deliver()

			return 0
		}

		pos, end = 0, uint32(n)
		p.put32(dir+dirEnd, end)
	}

	entry := dir + dirBuf + pos
	p.put32(dir+dirPos, pos+uint32(p.u16(entry+16)))
	p.deliver()

	return entry
}

func (k *Kernel) closedir(ctx uintptr, dir uint32) uint32 {
	p := proc(ctx)
	fd := p.u32(dir + dirFd)
	p.free(dir)

	if !p.closeFd(int(fd)) {
		return p.result(0, syscall.EBADF)
	}

	return p.result(0, nil)
}
