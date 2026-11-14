//go:build linux && amd64

package main

import "syscall"

const umountNofollow = 8

// mount mounts the directory source names on the directory target names:
// target's server hands every walk into target, and every call on its
// name, on to the server of source from then on (MOUNT, kernel_remote.go).
// source is opened as a directory, a server's: the end of that open is
// what is mounted. fstype, flags and data are not used.
func (p *Process) libcMount(source, target, fstype, flags, data uint32) uint32 {
	f, err := p.openPath(p.str(source), syscall.O_RDONLY|syscall.O_DIRECTORY, 0)

	if err != nil {
		return p.result(0, err)
	}

	defer f.unref()

	served, ok := f.(*remoteDir)

	if !ok {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(0, p.onName(atFdcwd, p.str(target), true, func(d Dir, name string, st *syscall.Stat_t) error {
		at, ok := d.(*remoteDir)

		if !ok || name == "" {
			return syscall.EINVAL
		}

		return at.mount(name, served.end)
	}))
}

// umount2 unmounts what is mounted on the directory target names; the
// flags but UMOUNT_NOFOLLOW are not used.
func (p *Process) libcUmount2(target, flags uint32) uint32 {
	return p.result(0, p.onName(atFdcwd, p.str(target), flags&umountNofollow == 0, func(d Dir, name string, st *syscall.Stat_t) error {
		at, ok := d.(*remoteDir)

		if !ok || name == "" {
			return syscall.EINVAL
		}

		return at.simpleName(rUnmount, 0, []byte(name))
	}))
}

func (p *Process) libcUmount(target uint32) uint32 {
	return p.libcUmount2(target, 0)
}
