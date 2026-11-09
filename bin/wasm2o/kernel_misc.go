//go:build linux && amd64

package main

import (
	"bytes"
	"fmt"
	"math"
	"path"
	"strings"
	"syscall"
	"unsafe"
)

// The rest of libc's services: random's generator, suboptions, daemon,
// prctl, getpass and the list of shells. random is musl's: its state is an
// array of words, the first n, i and j, in the guest's memory; the static
// area keeps the default one and the address of the one in use.

var randomInit = [32]uint32{
	0x00000000, 0x5851f42d, 0xc0b18ccf, 0xcbb5f646, 0xc7033129, 0x30705b04, 0x20fd5db4, 0x9a8b7f78,
	0x502959d8, 0xab894868, 0x6c0356a7, 0x88cdb7ff, 0xb477d43f, 0x70a3a52b, 0xa8e4baf1, 0xfd8341fc,
	0x8ae16fd9, 0x742d2f7a, 0x0d1f0796, 0x76035e09, 0x40f7702c, 0x6fa72ca5, 0xaaa84157, 0x58a0df74,
	0xc74a0364, 0xae533cc4, 0x04185faf, 0x6de3b115, 0x0cab8628, 0xf043bfa4, 0x398150e9, 0x37521657,
}

// randomState is the state random uses: its first word, then x[0]...
func (p *Process) randomState() uint32 {
	slot := p.staticAt(staticRandom)

	if at := p.u32(slot); at != 0 {
		return at
	}

	at := p.staticAt(staticRandomInit)

	for i, v := range randomInit {
		p.put32(at+4*uint32(i), v)
	}

	p.put32(at, 31<<16|3<<8)
	p.put32(slot, at)

	return at
}

func (p *Process) srandom(state, seed uint32) {
	n := p.u32(state) >> 16
	x := state + 4

	if n == 0 {
		p.put32(x, seed)

		return
	}

	i := uint32(1)

	if n == 31 || n == 7 {
		i = 3
	}

	s := uint64(seed)

	for k := range n {
		s = 6364136223846793005*s + 1
		p.put32(x+4*k, uint32(s>>32))
	}

	p.put32(x, p.u32(x)|1)
	p.put32(state, n<<16|i<<8)
}

func (p *Process) random() uint32 {
	state := p.randomState()
	h := p.u32(state)
	n, i, j := h>>16, h>>8&0xff, h&0xff
	x := state + 4

	if n == 0 {
		v := (1103515245*p.u32(x) + 12345) & 0x7fffffff
		p.put32(x, v)

		return v
	}

	if i >= n || j >= n {
		i, j = 0, 0
	}

	p.put32(x+4*i, p.u32(x+4*i)+p.u32(x+4*j))
	k := p.u32(x+4*i) >> 1

	if i++; i == n {
		i = 0
	}

	if j++; j == n {
		j = 0
	}

	p.put32(state, n<<16|i<<8|j)

	return k
}

// temper is rand_r's, musl's.
func temper(x uint32) uint32 {
	x ^= x >> 11
	x ^= x << 7 & 0x9d2c5680
	x ^= x << 15 & 0xefc60000
	x ^= x >> 18

	return x
}

// getsubopt takes the suboption *opt starts with, as musl's: it returns
// the index of its key or -1, its value at *val.
func (p *Process) getsubopt(opt, keys, val uint32) uint32 {
	s := p.u32(opt)
	p.put32(val, 0)
	text := p.cstr(s)
	n := uint32(len(text))

	if i := bytes.IndexByte(text, ','); i >= 0 {
		p.span(s+uint32(i), 1)[0] = 0
		p.put32(opt, s+uint32(i)+1)
		n = uint32(i)
	} else {
		p.put32(opt, s+n)
	}

	sub := string(p.span(s, n))

	for i := uint32(0); ; i++ {
		key := p.u32(keys + 4*i)

		if key == 0 {
			return math.MaxUint32
		}

		k := p.str(key)

		if !strings.HasPrefix(sub, k) {
			continue
		}

		switch {
		case len(sub) > len(k) && sub[len(k)] == '=':
			p.put32(val, s+uint32(len(k))+1)
		case len(sub) > len(k):
			continue
		}

		return i
	}
}

// daemon detaches the process as fork, setsid and the parent's exit would:
// it goes on with a new pid in a session of its own, its children and its
// old pid, which ends with status 0, its parent's. The first process ends
// wasm2o when it ends, so it stays as it is.
func (p *Process) daemon(nochdir, noclose uint32) uint32 {
	if nochdir == 0 {
		p.cwd = "/"
	}

	if noclose == 0 {
		host, err := p.openat2("/dev/null", syscall.O_RDWR, 0)

		if err != nil {
			return p.result(0, err)
		}

		f := newFile(host, true)

		for fd := range uint32(3) {
			p.dupTo2(f, fd)
		}

		f.unref()
	}

	k := p.k

	if p == k.first {
		return p.result(0, nil)
	}

	k.mu.Lock()
	parent := k.procs[p.ppid]
	old := p.pid
	k.lastPid++
	delete(k.procs, old)
	p.pid, p.pgid, p.sid, p.ppid = k.lastPid, k.lastPid, k.lastPid, k.first.pid
	k.procs[p.pid] = p

	for _, q := range k.procs {
		if q.ppid == old {
			q.ppid = k.first.pid
		}
	}

	k.mu.Unlock()

	if parent != nil && !parent.discardsChildren() {
		k.mu.Lock()
		k.procs[old] = &Process{k: k, pid: old, ppid: parent.pid, pgid: parent.pgid, sid: parent.sid, zombie: true}
		k.mu.Unlock()
		parent.childChanged(int(syscall.SIGCHLD))
	}

	return p.result(0, nil)
}

// dupTo2 makes a descriptor refer to a file, closing what it referred to.
func (p *Process) dupTo2(f *File, fd uint32) {
	if int(fd) >= len(p.files) {
		p.files = append(p.files, make([]*Descriptor, int(fd)+1-len(p.files))...)
	}

	f.ref()
	p.closeFd(int(fd))
	p.files[fd] = &Descriptor{file: f}
}

const (
	prSetPdeathsig  = 1
	prGetPdeathsig  = 2
	prGetDumpable   = 3
	prSetDumpable   = 4
	prSetName       = 15
	prGetName       = 16
	prSetNoNewPrivs = 38
	prGetNoNewPrivs = 39
)

// name is the process's name of prctl: its program's file name, at most
// 15 bytes, until it sets another.
func (p *Process) name() string {
	if p.comm == "" && p.image != nil && len(p.image.argv) > 0 {
		return truncate(path.Base(p.image.argv[0]), 15)
	}

	return p.comm
}

func (p *Process) prctl(option, va uint32) uint32 {
	arg := p.varArg(va, 0)

	switch option {
	case prSetPdeathsig:
		if arg != 0 && !validSignal(arg) {
			return p.result(0, syscall.EINVAL)
		}

		p.k.mu.Lock()
		p.pdeathsig = int(arg)
		p.k.mu.Unlock()
	case prGetPdeathsig:
		p.k.mu.Lock()
		sig := p.pdeathsig
		p.k.mu.Unlock()
		p.put32(arg, uint32(sig))
	case prGetDumpable:
		return p.result(int(bit(!p.undumpable)), nil)
	case prSetDumpable:
		if arg > 1 {
			return p.result(0, syscall.EINVAL)
		}

		p.undumpable = arg == 0
	case prSetName:
		name := p.cstr(arg)
		p.comm = truncate(string(name), 15)
	case prGetName:
		b := p.span(arg, 16)
		clear(b)
		copy(b, p.name())
	case prSetNoNewPrivs:
		if arg != 1 {
			return p.result(0, syscall.EINVAL)
		}

		p.noNewPrivs = true
	case prGetNoNewPrivs:
		return p.result(int(bit(p.noNewPrivs)), nil)
	default:
		return p.result(0, syscall.EINVAL)
	}

	return p.result(0, nil)
}

// getpass reads a line from the terminal with echo off, as musl's: at most
// 127 bytes, its newline dropped, in a static block.
func (p *Process) getpass(prompt uint32) uint32 {
	fd, err := p.openat2("/dev/tty", syscall.O_RDWR|syscall.O_NOCTTY, 0)

	if err != nil {
		return p.pointer(0, err)
	}

	defer syscall.Close(fd)

	var saved, t hostTermios

	if err := ioctlPtr(fd, tcgets, unsafe.Pointer(&saved)); err != nil {
		return p.pointer(0, err)
	}

	t = saved
	t.lflag = t.lflag&^(echo|isig) | icanon
	t.iflag = t.iflag&^(inlcr|igncr) | icrnl
	ioctlPtr(fd, tcsets+2, unsafe.Pointer(&t))
	defer ioctlPtr(fd, tcsets+2, unsafe.Pointer(&saved))

	syscall.Write(fd, []byte(p.str(prompt)))
	f := newFile(fd, false)
	var line []byte

	for len(line) < 127 {
		var b [1]byte
		n, err := p.readFile(f, b[:])

		if err != nil {
			return p.pointer(0, err)
		}

		if n == 0 || b[0] == '\n' {
			break
		}

		line = append(line, b[0])
	}

	syscall.Write(fd, []byte("\n"))
	block := p.staticResult(staticPass, 128)

	if block == 0 {
		return p.pointer(0, syscall.ENOMEM)
	}

	p.putString(block, string(line))

	return p.pointer(block, nil)
}

// shells are /etc/shells' paths, or musl's two without it.
func (p *Process) shells() []string {
	fd, err := p.openat2("/etc/shells", syscall.O_RDONLY, 0)

	if err != nil {
		return []string{"/bin/sh", "/bin/csh"}
	}

	var data []byte
	buf := make([]byte, 4096)

	for len(data) < databaseMax {
		n, err := syscall.Read(fd, buf)

		if n <= 0 || err != nil {
			break
		}

		data = append(data, buf[:n]...)
	}

	syscall.Close(fd)
	var out []string

	for _, line := range strings.Split(string(data), "\n") {
		if line = strings.TrimSpace(line); line != "" && !strings.HasPrefix(line, "#") {
			out = append(out, line)
		}
	}

	return out
}

func (k *Kernel) miscFunctions() map[string]any {
	return map[string]any{
		"random":        libcRandom,
		"srandom":       libcSrandom,
		"initstate":     libcInitstate,
		"setstate":      libcSetstate,
		"rand_r":        libcRand_r,
		"secure_getenv": libcSecure_getenv,
		"getsubopt":     libcGetsubopt,
		"daemon":        libcDaemon,
		"prctl":         libcPrctl,
		"getpass":       libcGetpass,
		"vhangup":       libcVhangup,
		"getusershell":  libcGetusershell,
		"setusershell":  libcSetusershell,
		"endusershell":  libcEndusershell,
		"gethostname":   libcGethostname,
		"getdomainname": libcGetdomainname,
		"sethostname":   libcSethostname,
		"setdomainname": libcSetdomainname,
		"gethostid":     libcGethostid,
		"sethostid":     libcSethostid,
		"psignal":       libcPsignal,
		"psiginfo":      libcPsiginfo,
		"__assert_fail": libc__assert_fail,
	}
}

func libcRandom(ctx uintptr) uint32 {
	return proc(ctx).random()
}

func libcSrandom(ctx uintptr, seed uint32) {
	p := proc(ctx)
	p.srandom(p.randomState(), seed)
}

// initstate makes a state of size bytes the one in use and returns
// the one before.
func libcInitstate(ctx uintptr, seed, state, size uint32) uint32 {
	p := proc(ctx)

	if size < 8 {
		return 0
	}

	old := p.randomState()
	n := uint32(63)

	switch {
	case size < 32:
		n = 0
	case size < 64:
		n = 7
	case size < 128:
		n = 15
	case size < 256:
		n = 31
	}

	p.span(state, 4*(n+1))
	p.put32(state, n<<16)
	p.srandom(state, seed)
	p.put32(p.staticAt(staticRandom), state)

	return old
}

func libcSetstate(ctx uintptr, state uint32) uint32 {
	p := proc(ctx)
	old := p.randomState()
	p.span(state, 4*(p.u32(state)>>16+1))
	p.put32(p.staticAt(staticRandom), state)

	return old
}

func libcRand_r(ctx uintptr, seed uint32) uint32 {
	p := proc(ctx)
	s := p.u32(seed)*1103515245 + 12345
	p.put32(seed, s)

	return temper(s) / 2
}

// No program is set-user-ID: secure_getenv is getenv.
func libcSecure_getenv(ctx uintptr, name uint32) uint32 {
	p := proc(ctx)
	_, value, _ := p.lookupEnv(p.cstr(name))

	return value
}

func libcGetsubopt(ctx uintptr, opt, keys, val uint32) uint32 {
	return proc(ctx).getsubopt(opt, keys, val)
}

func libcDaemon(ctx uintptr, nochdir, noclose uint32) uint32 {
	return proc(ctx).daemon(nochdir, noclose)
}

func libcPrctl(ctx uintptr, option, va uint32) uint32 {
	return proc(ctx).prctl(option, va)
}

func libcGetpass(ctx uintptr, prompt uint32) uint32 {
	return proc(ctx).getpass(prompt)
}

// Hanging up the terminal needs privilege.
func libcVhangup(ctx uintptr) uint32 {
	return proc(ctx).result(0, syscall.EPERM)
}

func libcGetusershell(ctx uintptr) uint32 {
	p := proc(ctx)
	shell := nextEntry(p, staticShellIndex, p.shells())

	if shell == nil {
		return 0
	}

	block := p.staticResult(staticShell, uint32(len(*shell)+1))

	if block != 0 {
		p.putString(block, *shell)
	}

	return block
}

func libcSetusershell(ctx uintptr) {
	p := proc(ctx)
	p.put32(p.staticAt(staticShellIndex), 0)
}

func libcEndusershell(ctx uintptr) {
	p := proc(ctx)
	p.put32(p.staticAt(staticShellIndex), 0)
}

// The host's names, which no process may set; the host id is
// musl's, 0.
func libcGethostname(ctx uintptr, name, n uint32) uint32 {
	p := proc(ctx)
	var u syscall.Utsname

	if err := syscall.Uname(&u); err != nil {
		return p.result(0, err)
	}

	host := utsField(u.Nodename[:])

	if n > 0 {
		p.putString(name, truncate(host, int(n-1)))
	}

	return p.result(0, nil)
}

func libcGetdomainname(ctx uintptr, name, n uint32) uint32 {
	p := proc(ctx)
	var u syscall.Utsname

	if err := syscall.Uname(&u); err != nil {
		return p.result(0, err)
	}

	domain := utsField(u.Domainname[:])

	if uint32(len(domain)) >= n {
		return p.result(0, syscall.EINVAL)
	}

	p.putString(name, domain)

	return p.result(0, nil)
}

func libcSethostname(ctx uintptr, name, n uint32) uint32 {
	return proc(ctx).result(0, syscall.EPERM)
}

func libcSetdomainname(ctx uintptr, name, n uint32) uint32 {
	return proc(ctx).result(0, syscall.EPERM)
}

func libcGethostid(ctx uintptr) uint32 {
	return 0
}

func libcSethostid(ctx uintptr, id uint32) uint32 {
	return proc(ctx).result(0, syscall.EPERM)
}

func libcPsignal(ctx uintptr, sig, s uint32) {
	p := proc(ctx)
	p.complain(s, signalMessage(int(sig)))
}

func libcPsiginfo(ctx uintptr, info, s uint32) {
	p := proc(ctx)
	p.complain(s, signalMessage(int(p.u32(info))))
}

func libc__assert_fail(ctx uintptr, expr, file, line, function uint32) {
	p := proc(ctx)
	msg := fmt.Sprintf("Assertion failed: %s (%s: %s: %d)\n", p.str(expr), p.str(file), p.str(function), int32(line))

	if f, err := p.fd(2); err == nil {
		p.writeFile(f, []byte(msg))
	}

	p.abort()
}

// complain writes a message to standard error after a prefix, perror's
// way.
func (p *Process) complain(prefix uint32, msg string) {
	if prefix != 0 && p.u8(prefix) != 0 {
		msg = p.str(prefix) + ": " + msg
	}

	if f, err := p.fd(2); err == nil {
		p.writeFile(f, []byte(msg+"\n"))
	}
}

func utsField(b []int8) string {
	out := make([]byte, 0, len(b))

	for _, c := range b {
		if c == 0 {
			break
		}

		out = append(out, byte(c))
	}

	return string(out)
}
