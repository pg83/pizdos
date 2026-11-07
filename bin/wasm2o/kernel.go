//go:build linux && amd64

package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"runtime"
	"strings"
	"sync"
	"syscall"
)

// The kernel runs programs linked against lib/c, whose system is the module
// "kernel" (lib/c/include declares its functions). A process is a goroutine
// running an instance; the kernel keeps its pid, descriptors, working
// directory, signals and children. Paths resolve inside the root directory
// `wasm2o run` is given, and exec runs only objects of wasm2o and scripts.
//
// A function of the kernel finds its process through the instance. Pointers
// are offsets in the process's memory: one out of it traps, as the guest's
// own access would. A system call sets errno and returns through result,
// which delivers pending signals.

type Kernel struct {
	// root is an O_PATH descriptor of the root directory, rootPath its host
	// path.
	root     int
	rootPath string
	mu       sync.Mutex
	procs    map[int32]*Process
	lastPid  int32
	codes    map[fileKey]*Code
	// fg is the foreground process group of the terminal.
	fg        int32
	first     *Process
	exit      chan int
	functions map[string]any
}

// fileKey identifies a version of a file: device, inode, size, mtime.
type fileKey struct {
	dev, ino uint64
	size     int64
	mtime    syscall.Timespec
}

type Process struct {
	k    *Kernel
	pid  int32
	ppid int32
	pgid int32
	sid  int32
	inst *Instance
	// image is what the instance runs: its code, arguments and environment.
	image *execImage
	files []*Descriptor
	// cwd is the absolute guest path of the working directory.
	cwd   string
	umask uint32
	// errnoAt and environAt are where the crt keeps errno and environ.
	errnoAt   uint32
	environAt uint32
	// heap is the allocator's header in memory, static the block of the
	// results libc keeps in static storage, env the environ array the kernel
	// allocated last.
	heap      uint32
	static    uint32
	env       uint32
	seed      uint64
	limits    [rlimitCount]rlimit
	regexes   map[uint32]*posixRegex
	nextRegex uint32
	// zoneNames are the heap's strings of zone names for tm_zone, siginfo
	// the siginfo_t of handlers.
	zoneNames map[string]uint32
	siginfo   uint32
	// jumps are the setjmps of the image by their jmp_buf, stackAt where
	// the instance keeps __stack_pointer.
	jumps   map[uint32]*jump
	stackAt uint32
	sig     signalState
	// wake is an eventfd a blocking call polls with its descriptors: a
	// signal sent to the process makes it readable.
	wake int
}

// execImage is a program a process runs; panicked by exec, it unwinds the
// old image.
type execImage struct {
	code *Code
	argv []string
	envp []string
}

// procExit is the panic of a process that ends; status is a wait status.
type procExit struct {
	status int
}

func newKernel(root string) *Kernel {
	k := &Kernel{procs: map[int32]*Process{}, codes: map[fileKey]*Code{}, exit: make(chan int, 1)}
	k.root = throw2(syscall.Open(root, oPath|syscall.O_DIRECTORY|syscall.O_CLOEXEC, 0))
	k.rootPath = throw2(os.Readlink(fmt.Sprintf("/proc/self/fd/%d", k.root)))
	k.functions = k.functionTable()
	// The kernel applies the processes' umasks itself.
	syscall.Umask(0)
	raiseFileLimit()
	k.routeSignals()

	return k
}

// proc is the process of the instance ctx.
func proc(ctx uintptr) *Process {
	return instanceAt(ctx).host.(*Process)
}

// Imports: the kernel's functions; the module imports nothing else.
func (k *Kernel) function(imp *ImportMeta) any {
	if imp.module != "kernel" {
		return nil
	}

	return k.functions[imp.name]
}

func (k *Kernel) global(*ImportMeta) uintptr {
	return 0
}

func (k *Kernel) memory(*ImportMeta) *LinearMemory {
	return nil
}

func (k *Kernel) table(*ImportMeta) uintptr {
	return 0
}

// functionTable lists the kernel's functions by import name.
func (k *Kernel) functionTable() map[string]any {
	t := map[string]any{"__kernel_start": k.kernelStart}

	for _, part := range []map[string]any{k.fileFunctions(), k.pathFunctions(), k.heapFunctions(), k.stringFunctions(),
		k.timeFunctions(), k.regexFunctions(), k.signalFunctions(), k.procFunctions(), k.jumpFunctions()} {
		for name, f := range part {
			t[name] = f
		}
	}

	return t
}

func importsKernel(obj *Object) bool {
	for _, imp := range obj.meta.imports {
		if imp.module == "kernel" {
			return true
		}
	}

	return false
}

// runKernel runs an object as the first process and returns the exit code
// of wasm2o: the process's, or 128 and the signal that ended it.
func runKernel(path, root string, argv []string) int {
	k := newKernel(root)
	fd := throw2(syscall.Open(path, syscall.O_RDONLY|syscall.O_CLOEXEC, 0))
	code, err := k.code(fd, path)
	syscall.Close(fd)
	throw(err)
	p := k.newProcess(nil)
	p.cwd = k.guestPath(throw2(os.Getwd()))
	env := []string{"PWD=" + p.cwd}

	for _, e := range os.Environ() {
		if !strings.HasPrefix(e, "PWD=") {
			env = append(env, e)
		}
	}

	for fd := range 3 {
		host, err := dupHigh(fd)

		if err == nil {
			p.files = append(p.files, &Descriptor{file: newFile(host, false)})
		} else {
			p.files = append(p.files, nil)
		}
	}

	k.first = p
	k.fg = p.pgid

	go p.run(&execImage{code: code, argv: argv, envp: env})

	return <-k.exit
}

// newProcess makes a process: a child of parent, or the first, the leader
// of its group and session.
func (k *Kernel) newProcess(parent *Process) *Process {
	k.mu.Lock()
	defer k.mu.Unlock()

	k.lastPid++
	p := &Process{k: k, pid: k.lastPid, umask: 0o022, regexes: map[uint32]*posixRegex{}, seed: 0}
	p.wake = throw2(eventfd())

	if parent == nil {
		p.pgid, p.sid = p.pid, p.pid
		p.limits = hostLimits()
	} else {
		p.ppid, p.pgid, p.sid = parent.pid, parent.pgid, parent.sid
		p.cwd, p.umask, p.limits, p.seed = parent.cwd, parent.umask, parent.limits, parent.seed
	}

	k.procs[p.pid] = p

	return p
}

// guestPath is the guest's path of a host path, which is inside the root
// or else the root itself.
func (k *Kernel) guestPath(host string) string {
	if k.rootPath == "/" {
		return host
	}

	if host == k.rootPath {
		return "/"
	}

	if strings.HasPrefix(host, k.rootPath+"/") {
		return host[len(k.rootPath):]
	}

	return "/"
}

// run runs the process's programs, one after another as it execs, then
// ends it.
func (p *Process) run(img *execImage) {
	status := 0

	for img != nil {
		img, status = p.runImage(img)

		if img != nil {
			p.execReset()
		}
	}

	p.terminate(status)
}

func (p *Process) runImage(img *execImage) (next *execImage, status int) {
	i := newInstance(img.code, p.k, runStack)
	i.host = p
	p.inst, p.image = i, img
	p.heap, p.static, p.env, p.errnoAt, p.environAt, p.siginfo = 0, 0, 0, 0, 0, 0
	p.regexes, p.zoneNames = map[uint32]*posixRegex{}, map[string]uint32{}
	p.jumps, p.stackAt = map[uint32]*jump{}, 0

	defer func() {
		switch e := recover().(type) {
		case nil:
		case *procExit:
			status = e.status
		case *execImage:
			next = e
		case *Trap:
			fmt.Fprintf(os.Stderr, "%s[%d]: %s\n", img.argv[0], p.pid, e.message())
			status = trapSignal(e.code)
		default:
			panic(e)
		}

		runtime.KeepAlive(i)
		p.inst = nil
		i.release()
	}()

	i.start()
	start, _ := i.export("_start")
	start.Interface().(func(uintptr))(i.ctx())

	return nil, 0
}

// trapSignal is the signal a trap ends a process with.
func trapSignal(code uint32) int {
	switch code {
	case trapMemoryOOB, trapTableOOB, trapNullRef, trapUndefinedElem, trapUninitElem:
		return int(syscall.SIGSEGV)
	case trapIntOverflow, trapDivByZero, trapConversion:
		return int(syscall.SIGFPE)
	}

	return int(syscall.SIGILL)
}

// terminate ends the process with a wait status: its descriptors close, it
// becomes a zombie for its parent, and with the first process wasm2o ends.
func (p *Process) terminate(status int) {
	for fd := range p.files {
		p.closeFd(fd)
	}

	syscall.Close(p.wake)
	p.k.mu.Lock()
	delete(p.k.procs, p.pid)
	p.k.mu.Unlock()

	if p == p.k.first {
		p.k.exit <- hostCode(status)
	}
}

// hostCode is wasm2o's exit code for a wait status.
func hostCode(status int) int {
	if status&0x7f != 0 {
		return 128 + status&0x7f
	}

	return status >> 8 & 0xff
}

// exitWith ends the process from inside one of its calls.
func (p *Process) exitWith(status int) {
	panic(&procExit{status})
}

func (k *Kernel) kernelStart(ctx uintptr, errnoAt, environAt, argcAt, argvAt uint32) {
	p := proc(ctx)
	p.errnoAt, p.environAt = errnoAt, environAt
	argv := p.stringArray(p.image.argv)
	p.env = p.stringArray(p.image.envp)

	if argv == 0 || p.env == 0 {
		p.exitWith(int(syscall.SIGABRT))
	}

	p.put32(argcAt, uint32(len(p.image.argv)))
	p.put32(argvAt, argv)
	p.put32(environAt, p.env)
}

// stringArray copies strings to the heap as a null-terminated array of
// pointers, or returns 0 when the heap is exhausted.
func (p *Process) stringArray(list []string) uint32 {
	arr := p.malloc(uint32(4 * (len(list) + 1)))

	if arr == 0 {
		return 0
	}

	for i, s := range list {
		at := p.newString(s)

		if at == 0 {
			return 0
		}

		p.put32(arr+uint32(4*i), at)
	}

	p.put32(arr+uint32(4*len(list)), 0)

	return arr
}

// newString copies a string to the heap, or returns 0.
func (p *Process) newString(s string) uint32 {
	at := p.malloc(uint32(len(s) + 1))

	if at != 0 {
		b := p.span(at, uint32(len(s)+1))
		copy(b, s)
		b[len(s)] = 0
	}

	return at
}

// memory

func (p *Process) mem() []byte {
	return p.inst.memory.bytes()
}

func oob() {
	panic(&Trap{trapMemoryOOB})
}

// span is n bytes of memory at at.
func (p *Process) span(at, n uint32) []byte {
	m := p.mem()

	if uint64(at)+uint64(n) > uint64(len(m)) {
		oob()
	}

	return m[at : at+n]
}

func (p *Process) u8(at uint32) byte {
	return p.span(at, 1)[0]
}

func (p *Process) u16(at uint32) uint16 {
	return binary.LittleEndian.Uint16(p.span(at, 2))
}

func (p *Process) u32(at uint32) uint32 {
	return binary.LittleEndian.Uint32(p.span(at, 4))
}

func (p *Process) u64(at uint32) uint64 {
	return binary.LittleEndian.Uint64(p.span(at, 8))
}

func (p *Process) put16(at uint32, v uint16) {
	binary.LittleEndian.PutUint16(p.span(at, 2), v)
}

func (p *Process) put32(at, v uint32) {
	binary.LittleEndian.PutUint32(p.span(at, 4), v)
}

func (p *Process) put64(at uint32, v uint64) {
	binary.LittleEndian.PutUint64(p.span(at, 8), v)
}

// cstr is the string at at, without its terminator.
func (p *Process) cstr(at uint32) []byte {
	m := p.mem()

	if uint64(at) >= uint64(len(m)) {
		oob()
	}

	n := bytes.IndexByte(m[at:], 0)

	if n < 0 {
		oob()
	}

	return m[at : at+uint32(n)]
}

func (p *Process) str(at uint32) string {
	return string(p.cstr(at))
}

// strings reads a null-terminated array of string pointers.
func (p *Process) strings(at uint32) []string {
	var out []string

	for ; ; at += 4 {
		s := p.u32(at)

		if s == 0 {
			return out
		}

		out = append(out, p.str(s))
	}
}

// putString copies s and a terminator to at, which has room for them.
func (p *Process) putString(at uint32, s string) {
	b := p.span(at, uint32(len(s)+1))
	copy(b, s)
	b[len(s)] = 0
}

// errors

func errnoOf(err error) syscall.Errno {
	var e syscall.Errno

	if errors.As(err, &e) {
		return e
	}

	return syscall.EIO
}

func (p *Process) setErrno(e syscall.Errno) {
	p.put32(p.errnoAt, uint32(e))
}

func (p *Process) errno() syscall.Errno {
	return syscall.Errno(p.u32(p.errnoAt))
}

// result returns r, or -1 with errno set when err is not nil, after
// delivering the pending signals: a system call returns through it.
func (p *Process) result(r int, err error) uint32 {
	if err != nil {
		p.setErrno(errnoOf(err))
		r = -1
	}

	p.deliver()

	return uint32(r)
}

// result64 is result for a 64-bit return value.
func (p *Process) result64(r int64, err error) uint64 {
	if err != nil {
		p.setErrno(errnoOf(err))
		r = -1
	}

	p.deliver()

	return uint64(r)
}

// pointer returns a pointer result: 0 with errno set when err is not nil.
func (p *Process) pointer(at uint32, err error) uint32 {
	if err != nil {
		p.setErrno(errnoOf(err))
		at = 0
	}

	p.deliver()

	return at
}

// staticArea is the guest block of libc's static results, made at first
// use: offsets staticDot.. below.
const (
	staticDot       = 0
	staticSlash     = 2
	staticTm        = 8
	staticCtime     = 56
	staticStrerror  = 88
	staticStrsignal = 184
	staticSize      = 256
)

func (p *Process) staticAt(off uint32) uint32 {
	if p.static == 0 {
		p.static = p.malloc(staticSize)

		if p.static == 0 {
			p.exitWith(int(syscall.SIGABRT))
		}

		clear(p.span(p.static, staticSize))
		p.putString(p.static+staticDot, ".")
		p.putString(p.static+staticSlash, "/")
	}

	return p.static + off
}
