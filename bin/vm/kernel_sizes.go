//go:build linux && amd64

package main

import (
	"fmt"
	"syscall"
)

// The structures lib/c hands the kernel come with their sizes: each after
// its pointer, and the size of one an import returns last (lib/c's
// src/kernel.c). The size is the structure's version. A layout only grows
// at its end, so the kernel can serve every size it has known of a
// structure. These are the sizes it knows, one of each so far: the layouts
// its code reads and writes. tst/test_abi_sizes.py checks them against
// lib/c's headers. A struct sockaddr has its length in every call.
const (
	sizeofAddrinfo    = 32  // struct addrinfo
	sizeofDirent      = 280 // struct dirent
	sizeofEpollEvent  = 16  // struct epoll_event
	sizeofFdSet       = 128 // fd_set
	sizeofFlock       = 32  // struct flock
	sizeofGlob        = 36  // glob_t
	sizeofGroup       = 16  // struct group
	sizeofHostent     = 20  // struct hostent
	sizeofIfNameindex = 8   // struct if_nameindex
	sizeofInAddr      = 4   // struct in_addr
	sizeofIovec       = 8   // struct iovec
	sizeofItimerval   = 32  // struct itimerval
	sizeofJmpBuf      = 80  // jmp_buf
	sizeofLconv       = 56  // struct lconv
	sizeofMsghdr      = 28  // struct msghdr
	sizeofNetent      = 16  // struct netent
	sizeofPasswd      = 28  // struct passwd
	sizeofPollfd      = 8   // struct pollfd
	sizeofProtoent    = 12  // struct protoent
	sizeofRegex       = 32  // regex_t
	sizeofRegmatch    = 8   // regmatch_t
	sizeofRlimit      = 16  // struct rlimit
	sizeofRusage      = 88  // struct rusage
	sizeofSchedParam  = 4   // struct sched_param
	sizeofServent     = 16  // struct servent
	sizeofSigaction   = 24  // struct sigaction
	sizeofSiginfo     = 36  // siginfo_t
	sizeofSigset      = 8   // sigset_t
	sizeofStack       = 12  // stack_t
	sizeofStat        = 112 // struct stat
	sizeofStatfs      = 88  // struct statfs
	sizeofStatvfs     = 72  // struct statvfs
	sizeofSysinfo     = 312 // struct sysinfo
	sizeofTermios     = 60  // struct termios
	sizeofTimespec    = 16  // struct timespec
	sizeofTimeval     = 16  // struct timeval
	sizeofTimezone    = 8   // struct timezone
	sizeofTm          = 44  // struct tm
	sizeofTms         = 32  // struct tms
	sizeofUtimbuf     = 16  // struct utimbuf
	sizeofUtsname     = 390 // struct utsname
	sizeofWinsize     = 8   // struct winsize
)

// sized checks the sizes of the structures a call passes, each followed
// by the kernel's. A program built for a layout this kernel does not know
// ends by SIGSYS, before the kernel touches its memory.
func (p *Process) sized(call string, sizes ...uint32) {
	for k := 0; k+1 < len(sizes); k += 2 {
		if sizes[k] != sizes[k+1] {
			p.crash(fmt.Sprintf("%s: a structure of %d bytes, which this kernel knows of %d", call, sizes[k], sizes[k+1]), callers(1))
			p.exitWith(int(syscall.SIGSYS))
		}
	}
}

// fcntlSize is the size of the structure a command of fcntl passes, or 0.
func fcntlSize(cmd uint32) uint32 {
	switch cmd {
	case fGetlk, fSetlk, fSetlkw:
		return sizeofFlock
	}

	return 0
}

// ioctlSize is the size of the structure a request of ioctl passes, or 0.
func ioctlSize(req uint32) uint32 {
	switch req {
	case tcgets, tcsets, tcsets + 1, tcsets + 2:
		return sizeofTermios
	case tiocgwinsz, tiocswinsz:
		return sizeofWinsize
	}

	return 0
}
