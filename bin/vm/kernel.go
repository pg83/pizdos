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
	"sync/atomic"
	"syscall"
)

// The kernel runs programs linked against lib/c, whose system is the module
// "kernel" (lib/c/include declares its functions). A process is a goroutine
// running an instance; the kernel keeps its pid, descriptors, working
// directory, signals and children. Paths resolve inside the root directory
// `vm run` is given, and exec runs only objects of vm and scripts.
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
	// abstract prefixes the abstract socket names of the kernel's
	// processes; unixNames are the guest paths of the host paths their
	// sockets are bound by (kernel_socket.go).
	abstract  string
	unixNames sync.Map
	// asan is `vm run --asan`: the first program is an asan object, and so
	// may be any it execs (kernel_asan.go); without it they are refused.
	asan bool
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
	// nice and policy are the scheduling of the process (kernel_identity.go),
	// pdeathsig the signal of prctl it gets when its parent ends, all under
	// the kernel's lock.
	nice      int32
	policy    int32
	pdeathsig int
	// comm, undumpable and noNewPrivs are prctl's.
	comm       string
	undumpable bool
	noNewPrivs bool
	inst       *Instance
	// image is what the instance runs: its code, arguments and environment.
	image *execImage
	files []*Descriptor
	// cwd is the absolute guest path of the working directory.
	cwd   string
	umask uint32
	// errnoAt and environAt are where the crt keeps errno and environ, tzAt
	// tzname, timezone and daylight, herrnoAt h_errno.
	errnoAt   uint32
	environAt uint32
	tzAt      [3]uint32
	herrnoAt  uint32
	// heap is what the kernel knows of the heap, static the block of the
	// results libc keeps in static storage, env the environ array the kernel
	// allocated last.
	heap      *Heap
	static    uint32
	env       uint32
	seed      uint64
	limits    [rlimitCount]rlimit
	regexes   map[uint32]*posixRegex
	nextRegex uint32
	// constants are strings of the heap the kernel returns and the program
	// does not free: zone names for tm_zone, locale names and items;
	// siginfo is the siginfo_t of handlers.
	constants map[string]uint32
	siginfo   uint32
	// locale is the name of each category's locale, "" for C;
	// envStrings are the strings of environ setenv made, mappings the
	// lengths of mmap's blocks.
	locale     [lcAll]string
	envStrings map[uint32]bool
	mappings   map[uint32]mapping
	timers     itimers
	// jumps are the setjmps of the image by their jmp_buf, stackAt where
	// the instance keeps __stack_pointer.
	jumps   map[uint32]*jump
	stackAt uint32
	sig     signalState
	// vfork is set while the process runs on its parent's frames, from vfork
	// until it execs or ends (kernel_fork.go).
	vfork *vforkFrame
	// zombie tells an ended process its parent has not reaped, status its
	// wait status; ppid, zombie and status are the kernel's lock's.
	zombie bool
	status int
	// halted is set while the process is stopped; report is the wait status
	// of a stop or continue its parent has not seen, under the same lock.
	halted bool
	report int
	// childEvents counts the changes of the children, for a blocked wait.
	childEvents atomic.Uint64
	// token is the goroutine running the process's code, for Go to
	// interrupt it, chasing set while a goroutine repeats that
	// (kernel_signal.go).
	token   atomic.Uintptr
	chasing atomic.Bool
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
	// path is the guest's absolute path of the program's file, through its
	// links, or empty when the file is outside the root.
	path string
}

// procExit is the panic of a process that ends; status is a wait status.
type procExit struct {
	status int
}

func newKernel(root string) *Kernel {
	k := &Kernel{procs: map[int32]*Process{}, codes: map[fileKey]*Code{}, exit: make(chan int, 1), abstract: fmt.Sprintf("pizdos.%d.", os.Getpid())}
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
	return instanceAt(ctx).proc
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

func importsKernel(obj *Object) bool {
	for _, imp := range obj.meta.imports {
		if imp.module == "kernel" {
			return true
		}
	}

	return false
}

// runKernel runs an object as the first process and returns the exit code
// of vm: the process's, or 128 and the signal that ended it.
func runKernel(path, root string, argv []string, asan bool) int {
	k := newKernel(root)
	k.asan = asan
	fd := throw2(syscall.Open(path, syscall.O_RDONLY|syscall.O_CLOEXEC, 0))
	code, err := k.code(fd, path)
	program := k.programPath(fd)
	syscall.Close(fd)
	throw(err)

	if asan && !code.obj.meta.asan() {
		throwFmt("%s: not compiled with --asan", path)
	}
	p := throw2(k.newProcess(nil))
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

	go p.run(&execImage{code: code, argv: argv, envp: env, path: program})

	return <-k.exit
}

// newProcess makes a process: a child of parent, or the first, the leader
// of its group and session.
func (k *Kernel) newProcess(parent *Process) (*Process, error) {
	wake, err := eventfd()

	if err != nil {
		return nil, err
	}

	k.mu.Lock()
	defer k.mu.Unlock()

	k.lastPid++
	p := &Process{k: k, pid: k.lastPid, umask: 0o022, regexes: map[uint32]*posixRegex{}, seed: 0, wake: wake}

	if parent == nil {
		p.pgid, p.sid = p.pid, p.pid
		p.limits = hostLimits()
	} else {
		p.ppid, p.pgid, p.sid = parent.pid, parent.pgid, parent.sid
		p.nice, p.policy = parent.nice, parent.policy
		p.cwd, p.umask, p.limits, p.seed = parent.cwd, parent.umask, parent.limits, parent.seed
	}

	k.procs[p.pid] = p

	return p, nil
}

// programPath is the guest's path of the program a host descriptor reads,
// or empty when it is outside the root.
func (k *Kernel) programPath(fd int) string {
	host, err := k.hostPath(fd)

	if err != nil || host != k.rootPath && k.rootPath != "/" && !strings.HasPrefix(host, k.rootPath+"/") {
		return ""
	}

	return k.guestPath(host)
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
	p.runFrom(img, nil, nil)
}

// runFrom is run with the first program already in i, entered by enter:
// in a child of fork.
func (p *Process) runFrom(img *execImage, i *Instance, enter func()) {
	status := 0

	if err := try(func() {
		for img != nil {
			p.image = img
			img, status = p.runImage(img, i, enter)
			i, enter = nil, nil

			if img != nil {
				p.execReset()
			}
		}
	}); err != nil {
		p.crash(err.message(), nil)
		status = 1 << 8
	}

	p.terminate(status)
}

func (p *Process) runImage(img *execImage, i *Instance, enter func()) (next *execImage, status int) {
	if i == nil {
		i = newInstance(img.code, p.k, runStack)
		i.attach(p)
		p.heap, p.static, p.env, p.errnoAt, p.environAt, p.siginfo = nil, 0, 0, 0, 0, 0
		p.regexes, p.constants, p.locale = map[uint32]*posixRegex{}, map[string]uint32{}, [lcAll]string{}
		p.envStrings, p.mappings = map[uint32]bool{}, map[uint32]mapping{}
		p.jumps, p.stackAt = map[uint32]*jump{}, 0
		enter = func() {
			p.asanStart()
			i.start()
			p.asanInit()
			i.startCommand()
		}
	}

	p.setInstance(i)
	p.image = img
	funcs, ok := i.functionCode()

	if ok {
		p.token.Store(runtime.SetInterrupt(uintptr(funcs[0]), uintptr(funcs[1]), interruptStub()))
	}

	defer func() {
		runtime.SetInterrupt(0, 0, 0)
		p.token.Store(0)
		e := recover()
		who := i.host.(*Process)
		p.endBorrowers(i, e)

		switch e := e.(type) {
		case nil:
		case *procExit:
			status = e.status
		case *execImage:
			next = e
		case *Trap:
			msg := e.message()

			if e.code == trapAsan {
				msg = who.asanReport(e)
			}

			who.crash(msg, e.stack)
			status = trapSignal(e.code)
		default:
			panic(e)
		}

		runtime.KeepAlive(i)
		p.unmapAll()
		p.setInstance(nil)
		i.release()
	}()

	enter()

	return nil, 0
}

// trapSignal is the signal a trap ends a process with.
func trapSignal(code uint32) int {
	switch code {
	case trapMemoryOOB, trapTableOOB, trapNullRef, trapUndefinedElem, trapUninitElem, trapAsan:
		return int(syscall.SIGSEGV)
	case trapIntOverflow, trapDivByZero, trapConversion:
		return int(syscall.SIGFPE)
	}

	return int(syscall.SIGILL)
}

// terminate ends the process with a wait status: its descriptors close, its
// children go to the first process, it becomes a zombie its parent reaps,
// or nothing when the parent ignores SIGCHLD; with the first process
// vm ends.
func (p *Process) terminate(status int) {
	p.stopTimers()

	for fd := range p.files {
		p.closeFd(fd)
	}

	p.sig.mu.Lock()
	p.sig.dead = true
	syscall.Close(p.wake)
	p.sig.mu.Unlock()
	k := p.k
	k.mu.Lock()
	parent := k.procs[p.ppid]
	adopted := false

	var bereaved []*Process

	for _, q := range k.procs {
		if q.ppid == p.pid {
			q.ppid = k.first.pid
			adopted = adopted || q.zombie

			if q.pdeathsig != 0 && !q.zombie {
				bereaved = append(bereaved, q)
			}
		}
	}

	notify := parent != nil && !parent.discardsChildren()

	if notify {
		p.zombie, p.status = true, status
	} else {
		delete(k.procs, p.pid)
	}

	k.mu.Unlock()

	if adopted && p != k.first {
		k.first.childChanged(int(syscall.SIGCHLD))
	}

	for _, q := range bereaved {
		q.send(q.pdeathsig)
	}

	if notify {
		parent.childChanged(int(syscall.SIGCHLD))
	}

	if p == k.first {
		k.exit <- hostCode(status)
	}
}

// discardsChildren tells that the process's ended children are not kept for
// it: it ignores SIGCHLD, or asked so with SA_NOCLDWAIT.
func (p *Process) discardsChildren() bool {
	p.sig.mu.Lock()
	defer p.sig.mu.Unlock()

	act := p.sig.actions[syscall.SIGCHLD]

	return act.handler == sigIgn || act.flags&saNocldwait != 0
}

// childChanged tells the process that a child changed, with a signal or
// none.
func (p *Process) childChanged(sig int) {
	p.childEvents.Add(1)

	if sig != 0 {
		p.send(sig)
	}

	p.wakeUp()
}

// hostCode is vm's exit code for a wait status.
func hostCode(status int) int {
	if status&0x7f != 0 {
		return 128 + status&0x7f
	}

	return status >> 8 & 0xff
}

// exitWith ends the process from inside one of its calls. A child of vfork
// gives its parent's frames back.
func (p *Process) exitWith(status int) {
	if v := p.vfork; v != nil {
		p.vfork = nil
		p.terminate(status)
		v.resume(p.pid)
	}

	panic(&procExit{status})
}

func (p *Process) libc__kernel_start(errnoAt, environAt, argcAt, argvAt, tznameAt, timezoneAt, daylightAt, herrnoAt uint32) {
	p.errnoAt, p.environAt = errnoAt, environAt
	p.tzAt, p.herrnoAt = [3]uint32{tznameAt, timezoneAt, daylightAt}, herrnoAt
	argv := p.stringArray(p.image.argv)
	p.env = p.stringArray(p.image.envp)

	if argv == 0 || p.env == 0 {
		p.exitWith(int(syscall.SIGABRT))
	}

	p.put32(argcAt, uint32(len(p.image.argv)))
	p.put32(argvAt, argv)
	p.put32(environAt, p.env)
}

// libc__kernel_program writes the guest's path of the program to buf, as
// getcwd writes the working directory's: what lib/c's dladdr names.
func (p *Process) libc__kernel_program(buf, size uint32) uint32 {
	path := p.image.path

	if path == "" {
		return p.pointer(0, syscall.ENOENT)
	}

	if uint32(len(path)) >= size {
		return p.pointer(0, syscall.ERANGE)
	}

	p.putString(buf, path)

	return p.pointer(buf, nil)
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
	return p.inst.mem
}

func oob() {
	panic(newTrap(trapMemoryOOB, 1))
}

// span is n bytes of memory at at, which the shadow map of an asan object
// must allow.
func (p *Process) span(at, n uint32) []byte {
	m := p.mem()

	if uint64(at)+uint64(n) > uint64(len(m)) {
		oob()
	}

	if p.inst.memory.shadow != nil {
		p.asanCheck(at, n)
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

	if p.inst.memory.shadow != nil {
		p.asanCheck(at, uint32(n)+1)
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

// constant is a string of the heap for results the program reads but
// does not free; the process keeps one copy of each.
func (p *Process) constant(s string) uint32 {
	if at, ok := p.constants[s]; ok {
		return at
	}

	at := p.newString(s)

	if at == 0 {
		p.exitWith(int(syscall.SIGABRT))
	}

	p.constants[s] = at

	return at
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
	staticDot        = 0
	staticSlash      = 2
	staticEmpty      = 4
	staticTm         = 8
	staticCtime      = 56
	staticStrerror   = 88
	staticStrsignal  = 184
	staticLconv      = 256
	staticStrtok     = 312
	staticPasswd     = 316
	staticGroup      = 320
	staticPwent      = 324
	staticGrent      = 328
	staticShell      = 332
	staticLogIdent   = 336
	staticLogOption  = 368
	staticLogMask    = 372
	staticShellIndex = 376
	staticPass       = 380
	staticRandom     = 384
	staticRandomInit = 388
	staticHostent    = 516
	staticHostIndex  = 520
	staticServent    = 524
	staticProtoent   = 528
	staticNtoa       = 532
	staticCrypt      = 548
	staticSize       = 560
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
