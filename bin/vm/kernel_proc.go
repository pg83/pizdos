//go:build linux && amd64

package main

import (
	"bytes"
	"fmt"
	"math"
	"os"
	"runtime"
	"strings"
	"syscall"
	"unsafe"
)

// Processes: identity, limits, the environment and exec. exec runs an
// object of wasm2o, which it links once per version of the file, or a
// script through its #! interpreter.

const (
	rlimitCount  = 16
	rlimitNofile = 7
	rlimitNproc  = 6
)

type rlimit struct {
	cur, max uint64
}

func hostLimits() [rlimitCount]rlimit {
	var out [rlimitCount]rlimit

	for i := range out {
		var l syscall.Rlimit

		if syscall.Getrlimit(i, &l) == nil {
			out[i] = rlimit{l.Cur, l.Max}
		} else {
			out[i] = rlimit{^uint64(0), ^uint64(0)}
		}
	}

	return out
}

// code links the object a host descriptor reads, or finds it linked: the
// error is ENOEXEC when the file is not an object of the kernel.
func (k *Kernel) code(fd int, name string) (*Code, error) {
	var st syscall.Stat_t

	if err := syscall.Fstat(fd, &st); err != nil {
		return nil, err
	}

	key := fileKey{st.Dev, st.Ino, st.Size, st.Mtim}
	k.mu.Lock()
	c, ok := k.codes[key]
	k.mu.Unlock()

	if ok {
		return c, nil
	}

	data := make([]byte, st.Size)

	for at := 0; at < len(data); {
		n, err := syscall.Pread(fd, data[at:], int64(at))

		if err != nil {
			return nil, err
		}

		if n == 0 {
			return nil, syscall.ENOEXEC
		}

		at += n
	}

	if e := try(func() {
		obj := readObject(data)

		if !importsKernel(obj) {
			throwFmt("not a program of the kernel")
		}

		c = link(name, obj, k)
	}); e != nil {
		fmt.Fprintf(os.Stderr, "wasm2o: %s: %s\n", name, e)
		return nil, syscall.ENOEXEC
	}

	k.mu.Lock()
	k.codes[key] = c
	k.mu.Unlock()

	return c, nil
}

// exec replaces the process's program; it returns only an error.
func (p *Process) exec(path string, argv, envp []string) error {
	return p.execFrom(path, -1, argv, envp)
}

// execFrom is exec of the file a host descriptor reads, when fd is one.
func (p *Process) execFrom(path string, fd int, argv, envp []string) error {
	img, err := p.loadFrom(path, fd, argv)

	if err != nil {
		return err
	}

	img.envp = envp
	p.execImage(img)

	return syscall.ELOOP
}

// loadProgram is exec's search: a wasm object, or the interpreter of a #!
// script, with argv rewritten for it; the image has no environment yet.
func (p *Process) loadProgram(path string, argv []string) (*execImage, error) {
	return p.loadFrom(path, -1, argv)
}

// loadFrom is loadProgram of the file a host descriptor reads, when first
// is one: fexecve's.
func (p *Process) loadFrom(path string, first int, argv []string) (*execImage, error) {
	for range 4 {
		fd, err := first, error(nil)

		if first >= 0 {
			fd, err = dupHigh(first)
			first = -1
		} else {
			fd, err = p.openat2(path, syscall.O_RDONLY, 0)
		}

		if err != nil {
			return nil, err
		}

		var st syscall.Stat_t

		if err := syscall.Fstat(fd, &st); err != nil || st.Mode&syscall.S_IFMT != syscall.S_IFREG {
			syscall.Close(fd)

			return nil, syscall.EACCES
		}

		if err := atCall(sysFaccessat2, fd, "", 1, atEmptyPath); err != nil {
			syscall.Close(fd)

			return nil, err
		}

		head := make([]byte, 256)
		n, _ := syscall.Pread(fd, head, 0)
		head = head[:n]

		if bytes.HasPrefix(head, []byte("#!")) {
			syscall.Close(fd)
			line, _, _ := bytes.Cut(head[2:], []byte("\n"))
			fields := strings.Fields(string(line))

			if len(fields) == 0 {
				return nil, syscall.ENOEXEC
			}

			script := []string{fields[0]}

			if len(fields) > 1 {
				script = append(script, strings.Join(fields[1:], " "))
			}

			if len(argv) > 0 {
				argv = argv[1:]
			}

			argv = append(append(script, path), argv...)
			path = fields[0]

			continue
		}

		code, err := p.k.code(fd, path)
		program := p.k.programPath(fd)
		syscall.Close(fd)

		if err != nil {
			return nil, err
		}

		return &execImage{code: code, argv: argv, path: program}, nil
	}

	return nil, syscall.ELOOP
}

// execPath is exec with a search of PATH when file has no slash.
func (p *Process) execPath(file string, argv, envp []string) error {
	if file == "" {
		return syscall.ENOENT
	}

	if strings.Contains(file, "/") {
		return p.exec(file, argv, envp)
	}

	path, ok := p.getenv("PATH")

	if !ok {
		path = "/usr/local/bin:/bin:/usr/bin"
	}

	err := error(syscall.ENOENT)

	for _, dir := range strings.Split(path, ":") {
		if dir == "" {
			dir = "."
		}

		switch e := p.exec(dir+"/"+file, argv, envp); e {
		case syscall.ENOENT, syscall.ENOTDIR:
		case syscall.EACCES:
			err = e
		default:
			return e
		}
	}

	return err
}

// execReset is what a new program starts with: descriptors marked
// close-on-exec are closed, handled signals are back to their defaults and
// there is no alternate signal stack.
func (p *Process) execReset() {
	for fd, d := range p.files {
		if d != nil && d.cloexec {
			p.closeFd(fd)
		}
	}

	p.sig.mu.Lock()

	for i, a := range p.sig.actions {
		if a.handler > sigIgn {
			p.sig.actions[i] = sigAction{}
		}
	}

	p.sig.altstack.sp, p.sig.altstack.size = 0, 0
	p.sig.mu.Unlock()
	p.comm = ""
}

// environment is the process's environ as it is now.
func (p *Process) environment() []string {
	if p.environAt == 0 {
		return nil
	}

	env := p.u32(p.environAt)

	if env == 0 {
		return nil
	}

	return p.strings(env)
}

// lookupEnv finds a variable: the address of its entry in environ and of
// its value.
func (p *Process) lookupEnv(name []byte) (uint32, uint32, bool) {
	if p.environAt == 0 {
		return 0, 0, false
	}

	env := p.u32(p.environAt)

	if env == 0 {
		return 0, 0, false
	}

	for at := env; ; at += 4 {
		s := p.u32(at)

		if s == 0 {
			return 0, 0, false
		}

		entry := p.cstr(s)

		if len(entry) > len(name) && entry[len(name)] == '=' && bytes.Equal(entry[:len(name)], name) {
			return at, s + uint32(len(name)) + 1, true
		}
	}
}

func (p *Process) getenv(name string) (string, bool) {
	_, value, ok := p.lookupEnv([]byte(name))

	if !ok {
		return "", false
	}

	return p.str(value), true
}

// setEnviron makes a new environ array of pointers.
func (p *Process) setEnviron(entries []uint32) bool {
	arr := p.malloc(uint32(4 * (len(entries) + 1)))

	if arr == 0 {
		return false
	}

	for i, s := range entries {
		p.put32(arr+uint32(4*i), s)
	}

	p.put32(arr+uint32(4*len(entries)), 0)
	old := p.u32(p.environAt)
	p.put32(p.environAt, arr)

	if old != 0 && old == p.env {
		p.free(old)
	}

	p.env = arr

	return true
}

// setVariable replaces the variables of environ named name by entry, or
// removes them for 0. A string setenv made is freed when it goes.
func (p *Process) setVariable(name string, entry uint32) bool {
	var entries, gone []uint32

	if env := p.u32(p.environAt); env != 0 {
		for at := env; p.u32(at) != 0; at += 4 {
			s := p.u32(at)

			if e := p.cstr(s); len(e) > len(name) && e[len(name)] == '=' && string(e[:len(name)]) == name {
				gone = append(gone, s)
			} else {
				entries = append(entries, s)
			}
		}
	}

	if entry != 0 {
		entries = append(entries, entry)
	}

	if !p.setEnviron(entries) {
		return false
	}

	for _, s := range gone {
		if s != entry && p.envStrings[s] {
			delete(p.envStrings, s)
			p.free(s)
		}
	}

	return true
}

// variableName is the name setenv and unsetenv take: not empty, no '='.
func (p *Process) variableName(at uint32) (string, bool) {
	name := p.str(at)

	if name == "" || strings.IndexByte(name, '=') >= 0 {
		p.setErrno(syscall.EINVAL)

		return "", false
	}

	return name, true
}

func (p *Process) libcGetpid() uint32 {
	return uint32(p.pid)
}

func (p *Process) libcGetppid() uint32 {
	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	return uint32(p.ppid)
}

func (p *Process) libcGetpgrp() uint32 {
	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	return uint32(p.pgid)
}

func (p *Process) libcGetuid() uint32 {
	return uint32(os.Getuid())
}

func (p *Process) libcGeteuid() uint32 {
	return uint32(os.Geteuid())
}

func (p *Process) libcGetgid() uint32 {
	return uint32(os.Getgid())
}

func (p *Process) libcGetegid() uint32 {
	return uint32(os.Getegid())
}

func (p *Process) libcGetgroups(n, list uint32) uint32 {
	groups, err := os.Getgroups()

	if err != nil {
		return p.result(0, err)
	}

	if n == 0 {
		return p.result(len(groups), nil)
	}

	if int(n) < len(groups) {
		return p.result(0, syscall.EINVAL)
	}

	for i, g := range groups {
		p.put32(list+uint32(4*i), uint32(g))
	}

	return p.result(len(groups), nil)
}

func (p *Process) libcUname(at uint32) uint32 {
	var u syscall.Utsname

	if err := syscall.Uname(&u); err != nil {
		return p.result(0, err)
	}

	b := p.span(at, 390)
	raw := unsafe.Slice((*byte)(unsafe.Pointer(&u)), unsafe.Sizeof(u))
	copy(b, raw)

	return p.result(0, nil)
}

// sysconf's -1 for the buffers of the user and group databases
// means no limit.
func (p *Process) libcSysconf(name uint32) uint32 {
	switch name {
	case 85:
		return uint32(pages(false))
	case 86:
		return uint32(pages(true))
	}

	values := map[uint32]int64{
		0: 131072, 1: int64(min64(p.limits[rlimitNproc].cur, 1<<30)), 2: 100, 3: 65536,
		4: int64(min64(p.limits[rlimitNofile].cur, 1<<30)), 7: 1, 8: 1, 29: 200809, 30: 4096, 43: 2048, 60: 1024,
		69: -1, 70: -1, 71: 256, 72: 32, 83: int64(runtime.NumCPU()), 84: int64(runtime.NumCPU()), 149: 200809, 173: 40, 180: 64,
	}

	if v, ok := values[name]; ok {
		return uint32(v)
	}

	return p.result(0, syscall.EINVAL)
}

func (p *Process) libcUmask(mask uint32) uint32 {
	old := p.umask
	p.umask = mask & 0o777

	return old
}

func (p *Process) libcRand() uint32 {
	p.seed = 6364136223846793005*p.seed + 1

	return uint32(p.seed >> 33)
}

func (p *Process) libcSrand(seed uint32) {
	p.seed = uint64(seed) - 1
}

func (p *Process) libcGetenv(name uint32) uint32 {
	_, value, _ := p.lookupEnv(p.cstr(name))

	return value
}

// putenv without a '=' removes the variable, as musl's.
func (p *Process) libcPutenv(s uint32) uint32 {
	name, _, set := strings.Cut(p.str(s), "=")

	if name == "" {
		p.setErrno(syscall.EINVAL)

		return math.MaxUint32
	}

	if !set {
		s = 0
	}

	if !p.setVariable(name, s) {
		p.setErrno(syscall.ENOMEM)

		return math.MaxUint32
	}

	return 0
}

func (p *Process) libcSetenv(at, value, overwrite uint32) uint32 {
	name, ok := p.variableName(at)

	if !ok {
		return math.MaxUint32
	}

	if _, _, set := p.lookupEnv([]byte(name)); set && overwrite == 0 {
		return 0
	}

	s := p.newString(name + "=" + p.str(value))

	if s == 0 || !p.setVariable(name, s) {
		p.free(s)
		p.setErrno(syscall.ENOMEM)

		return math.MaxUint32
	}

	p.envStrings[s] = true

	return 0
}

func (p *Process) libcUnsetenv(at uint32) uint32 {
	name, ok := p.variableName(at)

	if !ok {
		return math.MaxUint32
	}

	if !p.setVariable(name, 0) {
		p.setErrno(syscall.ENOMEM)

		return math.MaxUint32
	}

	return 0
}

func (p *Process) libcClearenv() uint32 {
	old := p.u32(p.environAt)
	p.put32(p.environAt, 0)

	if old != 0 && old == p.env {
		p.free(old)
	}

	p.env = 0

	return 0
}

func (p *Process) libcExecve(path, argv, envp uint32) uint32 {
	return p.result(0, p.exec(p.str(path), p.strings(argv), p.strings(envp)))
}

func (p *Process) libcExecvp(file, argv uint32) uint32 {
	return p.result(0, p.execPath(p.str(file), p.strings(argv), p.environment()))
}

func (p *Process) libcExecv(path, argv uint32) uint32 {
	return p.result(0, p.exec(p.str(path), p.strings(argv), p.environment()))
}

func (p *Process) libcExecvpe(file, argv, envp uint32) uint32 {
	return p.result(0, p.execPath(p.str(file), p.strings(argv), p.strings(envp)))
}

// execle's environment follows the null pointer that ends the
// arguments.
func (p *Process) libcExecle(path, arg0, va uint32) uint32 {
	argv := p.argList(arg0, va)
	envp := p.varArg(va, uint32(max(len(argv), 1)))

	if arg0 == 0 {
		envp = p.varArg(va, 0)
	}

	return p.result(0, p.exec(p.str(path), argv, p.strings(envp)))
}

// fexecve runs the file of a descriptor; a script's interpreter gets
// its path.
func (p *Process) libcFexecve(fd, argv, envp uint32) uint32 {
	f, err := p.fd(fd)

	if err != nil {
		return p.result(0, err)
	}

	host, err := p.k.hostPath(f.fd)

	if err != nil {
		return p.result(0, err)
	}

	return p.result(0, p.execFrom(p.k.guestPath(host), f.fd, p.strings(argv), p.strings(envp)))
}

func (p *Process) libcExecl(path, arg0, va uint32) uint32 {
	return p.result(0, p.exec(p.str(path), p.argList(arg0, va), p.environment()))
}

func (p *Process) libcExeclp(file, arg0, va uint32) uint32 {
	return p.result(0, p.execPath(p.str(file), p.argList(arg0, va), p.environment()))
}

func (p *Process) libc_exit(code uint32) {
	p.exitWith(int(code&0xff) << 8)
}

func (p *Process) libc_Exit(code uint32) {
	p.exitWith(int(code&0xff) << 8)
}

// system runs a command in the shell of the root and waits for it:
// a child of the process with its descriptors, its wait status
// shifted as the shell's system reports it. The shell missing is
// 127, the way a child that cannot run ends.
func (p *Process) libcSystem(cmd uint32) uint32 {
	argv := []string{"sh", "-c", string(p.cstr(cmd))}
	img, err := p.loadProgram("/bin/sh", argv)

	if err != nil {
		return uint32(int32(127 << 8))
	}

	img.envp = p.environment()

	c := p.k.newProcess(p)
	c.inherit(p)
	c.execReset()
	done := make(chan int, 1)

	go func() {
		c.run(img)
		done <- c.status
	}()

	status := <-done
	// its parent never waits for it: the child is reaped here
	p.k.mu.Lock()
	delete(p.k.procs, c.pid)
	p.k.mu.Unlock()

	return uint32(int32(status))
}

// argList reads the arguments of execl: the first, then the variadic ones
// up to a null pointer.
func (p *Process) argList(arg0, va uint32) []string {
	if arg0 == 0 {
		return nil
	}

	args := []string{p.str(arg0)}

	for i := uint32(0); ; i++ {
		s := p.varArg(va, i)

		if s == 0 {
			return args
		}

		args = append(args, p.str(s))
	}
}

func (p *Process) groupExists(pgid int32) bool {
	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	for _, q := range p.k.procs {
		if q.pgid == pgid && q.sid == p.sid {
			return true
		}
	}

	return false
}

func (p *Process) libcSetpgid(pid, pgid uint32) uint32 {
	target := p

	if pid != 0 && int32(pid) != p.pid {
		p.k.mu.Lock()
		q, ok := p.k.procs[int32(pid)]
		ok = ok && q.ppid == p.pid
		p.k.mu.Unlock()

		if !ok {
			return p.result(0, syscall.ESRCH)
		}

		target = q
	}

	group := int32(pgid)

	if group == 0 {
		group = target.pid
	}

	if group < 0 || group != target.pid && !p.groupExists(group) {
		return p.result(0, syscall.EPERM)
	}

	p.k.mu.Lock()
	target.pgid = group
	p.k.mu.Unlock()

	return p.result(0, nil)
}

func (p *Process) libcTcgetpgrp(fd uint32) uint32 {
	if p.libcIsatty(fd) == 0 {
		return p.result(0, p.errno())
	}

	p.k.mu.Lock()
	defer p.k.mu.Unlock()

	return p.result(int(p.k.fg), nil)
}

func (p *Process) libcTcsetpgrp(fd, pgrp uint32) uint32 {
	if p.libcIsatty(fd) == 0 {
		return p.result(0, p.errno())
	}

	if !p.groupExists(int32(pgrp)) {
		return p.result(0, syscall.EPERM)
	}

	p.k.mu.Lock()
	p.k.fg = int32(pgrp)
	p.k.mu.Unlock()

	return p.result(0, nil)
}
