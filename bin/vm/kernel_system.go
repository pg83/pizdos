//go:build linux && amd64

package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

// What the system tells of itself: memory, load and resource usage, which
// are the host's and vm's, configuration values, and the names of the
// login and terminals.

// The system logger: there is none to send messages to, so syslog writes
// one only to standard error, when openlog asked with LOG_PERROR. openlog
// copies the identity, as musl does; the static area keeps it, the options
// and the levels setlogmask masked.
const (
	logPid       = 0x01
	logPerror    = 0x20
	logIdentSize = 32
)

func (p *Process) syslog(priority, message uint32) {
	if p.u32(p.staticAt(staticLogMask))&(1<<(priority&7)) != 0 {
		return
	}

	option := p.u32(p.staticAt(staticLogOption))

	if option&logPerror == 0 {
		return
	}

	line := p.str(p.staticAt(staticLogIdent))

	if option&logPid != 0 {
		line += fmt.Sprintf("[%d]", p.pid)
	}

	line += ": " + p.str(message)

	if !strings.HasSuffix(line, "\n") {
		line += "\n"
	}

	if f, err := p.fd(2); err == nil {
		p.writeFile(f, []byte(line))
	}
}

// pathconfValues are musl's values of pathconf by _PC_ name; -1 means no
// limit.
var pathconfValues = []int32{8, 255, 255, 255, 4096, 4096, 1, 1, 0, 1, -1, -1, -1, 64, 4096, 4096, 4096, 4096, 4096, -1, 1}

func (p *Process) pathconf(name uint32) uint32 {
	if name >= uint32(len(pathconfValues)) {
		return p.result(0, syscall.EINVAL)
	}

	return p.result(int(pathconfValues[name]), nil)
}

// sysinfo writes the guest's struct sysinfo: the host's, its sizes in a
// unit that fits them in 32 bits, and the kernel's count of processes.
func (p *Process) sysinfo(at uint32) error {
	var si syscall.Sysinfo_t

	if err := syscall.Sysinfo(&si); err != nil {
		return err
	}

	sizes := []uint64{si.Totalram, si.Freeram, si.Sharedram, si.Bufferram, si.Totalswap, si.Freeswap, si.Totalhigh, si.Freehigh}
	unit := uint64(max(si.Unit, 1))
	scale := unit

	for _, s := range sizes {
		for s*unit/scale > math.MaxUint32 {
			scale *= 2
		}
	}

	b := p.span(at, 312)
	clear(b)
	p.put32(at, uint32(min(uint64(si.Uptime), math.MaxUint32)))

	for i, l := range si.Loads {
		p.put32(at+4+4*uint32(i), uint32(l))
	}

	for i, s := range sizes[:6] {
		p.put32(at+16+4*uint32(i), uint32(s*unit/scale))
	}

	p.put16(at+40, uint16(min(len(p.k.processes(func(*Process) bool { return true })), math.MaxUint16)))
	p.put32(at+44, uint32(sizes[6]*unit/scale))
	p.put32(at+48, uint32(sizes[7]*unit/scale))
	p.put32(at+52, uint32(scale))

	return nil
}

// pages is the host's memory, or its free memory, in pages.
func pages(free bool) int {
	var si syscall.Sysinfo_t

	if syscall.Sysinfo(&si) != nil {
		return -1
	}

	n := si.Totalram

	if free {
		n = si.Freeram
	}

	return int(min(n*uint64(max(si.Unit, 1))/4096, math.MaxInt32))
}

// putRusage writes the guest's struct rusage: CPU times and the counts
// of the host's, which vm's processes share.
func (p *Process) putRusage(at uint32, ru *syscall.Rusage) {
	clear(p.span(at, 88))

	if ru == nil {
		return
	}

	p.putTimeval(at, time.Duration(ru.Utime.Nano()))
	p.putTimeval(at+16, time.Duration(ru.Stime.Nano()))

	for i, v := range []int64{ru.Maxrss, ru.Ixrss, ru.Idrss, ru.Isrss, ru.Minflt, ru.Majflt, ru.Nswap, ru.Inblock, ru.Oublock, ru.Msgsnd,
		ru.Msgrcv, ru.Nsignals, ru.Nvcsw, ru.Nivcsw} {
		p.put32(at+32+4*uint32(i), uint32(min(v, math.MaxInt32)))
	}
}

// terminalName is the name of a descriptor's terminal: one of the host's
// devices a guest's path names.
func (p *Process) terminalName(fd uint32) (string, syscall.Errno) {
	f, err := p.fd(fd)

	if err != nil {
		return "", syscall.EBADF
	}

	var t hostTermios

	if ioctlPtr(f.fd, tcgets, unsafe.Pointer(&t)) != nil {
		return "", syscall.ENOTTY
	}

	name, err := os.Readlink(procFd(f.fd))

	if err != nil {
		return "", errnoOf(err)
	}

	if !hostDevice(name) {
		name = p.k.guestPath(name)
	}

	return name, 0
}

// limitOf reads or sets a resource limit of a process.
func (p *Process) limitOf(q *Process, res, set, old uint32) error {
	if res >= rlimitCount {
		return syscall.EINVAL
	}

	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	cur := q.limits[res]

	if set != 0 {
		l := rlimit{p.u64(set), p.u64(set + 8)}

		if l.cur > l.max {
			return syscall.EINVAL
		}

		if l.max > cur.max && os.Geteuid() != 0 {
			return syscall.EPERM
		}

		q.limits[res] = l
	}

	if old != 0 {
		p.put64(old, cur.cur)
		p.put64(old+8, cur.max)
	}

	return nil
}

func (p *Process) libcOpenlog(ident, option, facility uint32) {
	name := ""

	if ident != 0 {
		name = truncate(p.str(ident), logIdentSize-1)
	}

	p.putString(p.staticAt(staticLogIdent), name)
	p.put32(p.staticAt(staticLogOption), option)
}

func (p *Process) libcCloselog() {
}

// setlogmask returns the levels it let through; 0 changes nothing.
func (p *Process) libcSetlogmask(mask uint32) uint32 {
	at := p.staticAt(staticLogMask)
	old := ^p.u32(at) & 0xff

	if mask != 0 {
		p.put32(at, ^mask&0xff)
	}

	return old
}

func (p *Process) libcSysinfo(at uint32) uint32 {
	return p.result(0, p.sysinfo(at))
}

func (p *Process) libcGet_nprocs() uint32 {
	return uint32(runtime.NumCPU())
}

func (p *Process) libcGet_nprocs_conf() uint32 {
	return uint32(runtime.NumCPU())
}

func (p *Process) libcGet_phys_pages() uint32 {
	return uint32(pages(false))
}

func (p *Process) libcGet_avphys_pages() uint32 {
	return uint32(pages(true))
}

func (p *Process) libcGetloadavg(at, n uint32) uint32 {
	switch {
	case n == 0:
		return 0
	case int32(n) < 0:
		return math.MaxUint32
	}

	var si syscall.Sysinfo_t

	if syscall.Sysinfo(&si) != nil {
		return math.MaxUint32
	}

	n = min(n, 3)

	for i := range n {
		p.put64(at+8*i, math.Float64bits(float64(si.Loads[i])/65536))
	}

	return n
}

// getrusage of the process, or its thread, is vm's: its
// children's are in it.
func (p *Process) libcGetrusage(who, at uint32) uint32 {
	switch int32(who) {
	case 0, 1:
		var ru syscall.Rusage

		if err := syscall.Getrusage(syscall.RUSAGE_SELF, &ru); err != nil {
			return p.result(0, err)
		}

		p.putRusage(at, &ru)
	case -1:
		p.putRusage(at, nil)
	default:
		return p.result(0, syscall.EINVAL)
	}

	return p.result(0, nil)
}

func (p *Process) libcGetrlimit(res, at uint32) uint32 {
	return p.result(0, p.limitOf(p, res, 0, at))
}

func (p *Process) libcSetrlimit(res, at uint32) uint32 {
	return p.result(0, p.limitOf(p, res, at, 0))
}

func (p *Process) libcPrlimit(pid, res, set, old uint32) uint32 {
	q, err := p.process(int32(pid))

	if err == nil {
		err = p.limitOf(q, res, set, old)
	}

	return p.result(0, err)
}

func (p *Process) libcGetdtablesize() uint32 {
	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	return uint32(min(p.limits[rlimitNofile].cur, math.MaxInt32))
}

// confstr knows _CS_PATH.
func (p *Process) libcConfstr(name, buf, n uint32) uint32 {
	if name != 0 {
		p.setErrno(syscall.EINVAL)

		return 0
	}

	s := "/bin:/usr/bin"

	if n > 0 {
		p.putString(buf, s[:min(int(n-1), len(s))])
	}

	return uint32(len(s) + 1)
}

func (p *Process) libcPathconf(path, name uint32) uint32 {
	if err := p.onPath(p.str(path), true, func(int) error { return nil }); err != nil {
		return p.result(0, err)
	}

	return p.pathconf(name)
}

func (p *Process) libcFpathconf(fd, name uint32) uint32 {
	if _, err := p.fd(fd); err != nil {
		return p.result(0, err)
	}

	return p.pathconf(name)
}

// The login name is LOGNAME's, as musl's.
func (p *Process) libcGetlogin() uint32 {
	_, value, _ := p.lookupEnv([]byte("LOGNAME"))

	return value
}

func (p *Process) libcGetlogin_r(buf, n uint32) uint32 {
	name, ok := p.getenv("LOGNAME")

	switch {
	case !ok:
		return uint32(syscall.ENXIO)
	case uint32(len(name)) >= n:
		return uint32(syscall.ERANGE)
	}

	p.putString(buf, name)

	return 0
}

func (p *Process) libcCtermid(s uint32) uint32 {
	if s == 0 {
		return p.constant("/dev/tty")
	}

	p.putString(s, "/dev/tty")

	return s
}

func (p *Process) libcTtyname(fd uint32) uint32 {
	name, e := p.terminalName(fd)

	if e != 0 {
		p.setErrno(e)

		return 0
	}

	return p.constant(name)
}

// ttyname_r returns an error number.
func (p *Process) libcTtyname_r(fd, buf, n uint32) uint32 {
	name, e := p.terminalName(fd)

	switch {
	case e != 0:
		return uint32(e)
	case uint32(len(name)) >= n:
		return uint32(syscall.ERANGE)
	}

	p.putString(buf, name)

	return 0
}

func (p *Process) libc__kernel_syslog(priority, message uint32) {
	p.syslog(priority, message)
}
