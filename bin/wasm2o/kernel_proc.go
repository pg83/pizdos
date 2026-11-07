//go:build linux && amd64

package main

import (
	"bytes"
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
		return nil, syscall.ENOEXEC
	}

	k.mu.Lock()
	k.codes[key] = c
	k.mu.Unlock()

	return c, nil
}

// exec replaces the process's program; it returns only an error.
func (p *Process) exec(path string, argv, envp []string) error {
	for range 4 {
		fd, err := p.openat2(path, syscall.O_RDONLY, 0)

		if err != nil {
			return err
		}

		var st syscall.Stat_t

		if err := syscall.Fstat(fd, &st); err != nil || st.Mode&syscall.S_IFMT != syscall.S_IFREG {
			syscall.Close(fd)

			return syscall.EACCES
		}

		if err := atCall(sysFaccessat2, fd, "", 1, atEmptyPath); err != nil {
			syscall.Close(fd)

			return err
		}

		head := make([]byte, 256)
		n, _ := syscall.Pread(fd, head, 0)
		head = head[:n]

		if bytes.HasPrefix(head, []byte("#!")) {
			syscall.Close(fd)
			line, _, _ := bytes.Cut(head[2:], []byte("\n"))
			fields := strings.Fields(string(line))

			if len(fields) == 0 {
				return syscall.ENOEXEC
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
		syscall.Close(fd)

		if err != nil {
			return err
		}

		panic(&execImage{code: code, argv: argv, envp: envp})
	}

	return syscall.ELOOP
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
// close-on-exec are closed, handled signals are back to their defaults.
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

	p.sig.mu.Unlock()
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

func (k *Kernel) procFunctions() map[string]any {
	ids := func(f func() int) func(uintptr) uint32 {
		return func(uintptr) uint32 {
			return uint32(f())
		}
	}

	return map[string]any{
		"getpid": func(ctx uintptr) uint32 {
			return uint32(proc(ctx).pid)
		},
		"getppid": func(ctx uintptr) uint32 {
			return uint32(proc(ctx).ppid)
		},
		"getpgrp": func(ctx uintptr) uint32 {
			return uint32(proc(ctx).pgid)
		},
		"setpgid":   k.setpgid,
		"tcgetpgrp": k.tcgetpgrp,
		"tcsetpgrp": k.tcsetpgrp,
		"getuid":    ids(os.Getuid),
		"geteuid":   ids(os.Geteuid),
		"getgid":    ids(os.Getgid),
		"getegid":   ids(os.Getegid),
		"getgroups": func(ctx uintptr, n, list uint32) uint32 {
			p := proc(ctx)
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
		},
		"getrlimit": func(ctx uintptr, res, at uint32) uint32 {
			p := proc(ctx)

			if res >= rlimitCount {
				return p.result(0, syscall.EINVAL)
			}

			p.put64(at, p.limits[res].cur)
			p.put64(at+8, p.limits[res].max)

			return p.result(0, nil)
		},
		"setrlimit": func(ctx uintptr, res, at uint32) uint32 {
			p := proc(ctx)

			if res >= rlimitCount {
				return p.result(0, syscall.EINVAL)
			}

			l := rlimit{p.u64(at), p.u64(at + 8)}

			if l.cur > l.max {
				return p.result(0, syscall.EINVAL)
			}

			if l.max > p.limits[res].max && os.Geteuid() != 0 {
				return p.result(0, syscall.EPERM)
			}

			p.limits[res] = l

			return p.result(0, nil)
		},
		"uname": func(ctx uintptr, at uint32) uint32 {
			p := proc(ctx)
			var u syscall.Utsname

			if err := syscall.Uname(&u); err != nil {
				return p.result(0, err)
			}

			b := p.span(at, 390)
			raw := unsafe.Slice((*byte)(unsafe.Pointer(&u)), unsafe.Sizeof(u))
			copy(b, raw)

			return p.result(0, nil)
		},
		"sysconf": func(ctx uintptr, name uint32) uint32 {
			p := proc(ctx)
			values := map[uint32]int64{
				0: 131072, 1: int64(min64(p.limits[rlimitNproc].cur, 1<<30)), 2: 100, 3: 65536,
				4: int64(min64(p.limits[rlimitNofile].cur, 1<<30)), 7: 1, 8: 1, 29: 200809, 30: 4096, 43: 2048, 60: 1024,
				71: 256, 72: 32, 83: int64(runtime.NumCPU()), 84: int64(runtime.NumCPU()), 149: 200809, 173: 40, 180: 64,
			}

			if v, ok := values[name]; ok {
				return uint32(v)
			}

			return p.result(0, syscall.EINVAL)
		},
		"umask": func(ctx uintptr, mask uint32) uint32 {
			p := proc(ctx)
			old := p.umask
			p.umask = mask & 0o777

			return old
		},
		"rand": func(ctx uintptr) uint32 {
			p := proc(ctx)
			p.seed = 6364136223846793005*p.seed + 1

			return uint32(p.seed >> 33)
		},
		"srand": func(ctx uintptr, seed uint32) {
			proc(ctx).seed = uint64(seed) - 1
		},
		"getenv": func(ctx uintptr, name uint32) uint32 {
			p := proc(ctx)
			_, value, _ := p.lookupEnv(p.cstr(name))

			return value
		},
		"putenv": func(ctx uintptr, s uint32) uint32 {
			p := proc(ctx)
			entry := p.cstr(s)
			name, _, set := bytes.Cut(entry, []byte("="))
			var entries []uint32

			if env := p.u32(p.environAt); env != 0 {
				for at := env; p.u32(at) != 0; at += 4 {
					e := p.cstr(p.u32(at))

					if !(len(e) > len(name) && e[len(name)] == '=' && bytes.Equal(e[:len(name)], name)) {
						entries = append(entries, p.u32(at))
					}
				}
			}

			if set {
				entries = append(entries, s)
			}

			if !p.setEnviron(entries) {
				return p.result(0, syscall.ENOMEM)
			}

			return 0
		},
		"clearenv": func(ctx uintptr) uint32 {
			p := proc(ctx)
			old := p.u32(p.environAt)
			p.put32(p.environAt, 0)

			if old != 0 && old == p.env {
				p.free(old)
			}

			p.env = 0

			return 0
		},
		"execve": func(ctx uintptr, path, argv, envp uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.exec(p.str(path), p.strings(argv), p.strings(envp)))
		},
		"execvp": func(ctx uintptr, file, argv uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.execPath(p.str(file), p.strings(argv), p.environment()))
		},
		"execl": func(ctx uintptr, path, arg0, va uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.exec(p.str(path), p.argList(arg0, va), p.environment()))
		},
		"execlp": func(ctx uintptr, file, arg0, va uint32) uint32 {
			p := proc(ctx)

			return p.result(0, p.execPath(p.str(file), p.argList(arg0, va), p.environment()))
		},
		"fork": func(ctx uintptr) uint32 {
			return proc(ctx).result(0, syscall.ENOSYS)
		},
		"vfork": func(ctx uintptr) uint32 {
			return proc(ctx).result(0, syscall.ENOSYS)
		},
		"waitpid": func(ctx uintptr, pid, status, options uint32) uint32 {
			return proc(ctx).result(0, syscall.ECHILD)
		},
		"wait": func(ctx uintptr, status uint32) uint32 {
			return proc(ctx).result(0, syscall.ECHILD)
		},
		"_exit": func(ctx uintptr, code uint32) {
			proc(ctx).exitWith(int(code&0xff) << 8)
		},
		"_Exit": func(ctx uintptr, code uint32) {
			proc(ctx).exitWith(int(code&0xff) << 8)
		},
	}
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

func (k *Kernel) setpgid(ctx uintptr, pid, pgid uint32) uint32 {
	p := proc(ctx)
	target := p

	if pid != 0 && int32(pid) != p.pid {
		k.mu.Lock()
		q, ok := k.procs[int32(pid)]
		k.mu.Unlock()

		if !ok || q.ppid != p.pid {
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

	target.pgid = group

	return p.result(0, nil)
}

func (k *Kernel) tcgetpgrp(ctx uintptr, fd uint32) uint32 {
	p := proc(ctx)

	if k.isatty(ctx, fd) == 0 {
		return p.result(0, p.errno())
	}

	k.mu.Lock()
	defer k.mu.Unlock()

	return p.result(int(k.fg), nil)
}

func (k *Kernel) tcsetpgrp(ctx uintptr, fd, pgrp uint32) uint32 {
	p := proc(ctx)

	if k.isatty(ctx, fd) == 0 {
		return p.result(0, p.errno())
	}

	if !p.groupExists(int32(pgrp)) {
		return p.result(0, syscall.EPERM)
	}

	k.mu.Lock()
	k.fg = int32(pgrp)
	k.mu.Unlock()

	return p.result(0, nil)
}
