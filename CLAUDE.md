# CLAUDE.md

pizdos: an OS whose kernel is a Go program running wasm processes. Read
README.md for the layout and the C ABI, STYLE.md and STYLE_PRJ.md for style.

## Build and test

- `./build` builds `libc.a`, `busybox.wasm`, `busybox.links`,
  `bin/busybox/abi.txt` (every kernel import busybox uses), `doom.wasm`,
  `doom.o` (doomgeneric with our terminal backend, `bin/doom`), `wasm2o`,
  native `aot`, `aot.wasm`, `aot.aot` and the wazero runner.
- `./build test` runs `tst/test_*.py`, each with the outputs of the nodes
  `TESTED` names for it in build.py.
- `sh dev/stand/build.sh` supplies the pinned ix revision's libraries and
  tools and runs `./build stand`: decode's im, pdf, djvu, svg and magic
  modules through wasm2o, and their wrappers against the native
  libraries; python.o and CPython built natively from the same tarball
  (`dev/stand/python.py`: no computed gotos, the same built-in modules).
  `dev/stand.py --runs 5 --json FILE` runs native (A) and our
  AOT (C) interleaved and prints the geometric mean of C/A; C/A <= 1.2 is
  the goal. Native is ordinary clang code of the same program, never
  wasm2c. Native codec libraries and wrappers disable SIMD implementations,
  loop/SLP vectorization, AVX and FMA (scalar.py edits only the private ix
  checkout); AVIF uses libaom, as the wasm module does. The stand_scalar
  flag belongs to each decoder library, never to the whole ix realm or
  host toolchain. Decode excludes startup, I/O and host hashing and repeats short
  workloads. The compiler's outputs must be identical; decode must agree on
  status and sizes, with pixel hash differences counted separately. The 89-file corpus is
  pinned by hash in build.py and fetched from pg83/store; corpus.sh makes
  it reproducibly with the same encoder toolchain (README.md).
- A node that consumes another node's output names it in `deps=`: the engine
  restores only dependency outputs into a node's work directory.
- Commit messages in English.

## lib/c

- Our own libc. Musl sources are used only for stdio, sorting, libm, getopt,
  number parsing and multibyte conversion (`lib/c/musl/files.txt`); every
  other function is a kernel import or our code in `lib/c/src`.
- A kernel import is declared `__KERNEL(name)` in its public header. Never
  define a function that a header declares that way, and never declare an
  in-process function with it.
- Headers are written for this system, not copied from musl or glibc;
  constants use Linux generic values, struct layouts are fixed by the ABI
  because the kernel reads and writes them.
- The link has no `--allow-undefined`: a missing function is a link error,
  not a silent import.
- `test_libc` runs tst/libc/check.c, built against lib/c, in the kernel:
  one line of results per area, compared with the test's expected text.

## bin/wasm2o

- The compiler is `bin/aot/aot.cpp`, one freestanding C++ translation unit.
  `build.py` builds native `aot` and `aot.wasm`, then native aot compiles the
  reactor to `aot.aot`. `wasm2o` embeds that object through `go:embed`;
  `dev/go.py --embed` stages it for both native builds and Go tests. There is
  no Go compiler implementation or wasm2o.wasm bootstrap.
- Compiler optimization aims to reduce compilation CPU while generated
  programs run no slower on the stand. ELF bytes may change between
  revisions; native and embedded forms of the same compiler still agree.
  Freeze the compiler's wasm input across revisions with
  `dev/aot_perf.py --compiler-input`, save objects with `--objects`, and
  run the saved and candidate objects through the same stand loader.
- `bin/wasm2o` is the Go loader, kernel and WASI host. `compile.go` links
  embedded code once and gives each invocation its own instance, memory and
  fd streams. `compile()` reads fd 0 and writes ELF to fd 1;
  `compileOptions(flags)` selects tail, range optimization and profiling
  with bits 0, 1 and 2. The default is 3, strict spec compilation is 0.
  Every invocation releases its instance even on compiler or I/O failure.
- The object is ordinary x86-64 ELF: `aot.cpp` writes it, `elf.go` reads it,
  `symbols.go` parses typed boundary symbols and `meta.go` reads the note.
  `abi.go` and `format.go` hold the loader's shared ABI constants and types.
  Generated code follows Go 1.25 amd64 ABIInternal: R14 is g, RBP the frame
  pointer, X15 untouched, and the stack pointer table is exact at every
  instruction. A stack check calls the `w2o.morestack` stub with what it
  compared on the stack; the loader marks the stub assembly, which Go
  never preempts, and the stub compares that with `g.stackguard0` again
  before Go's morestack, so a preemption that completed after the check
  does not grow the stack. `W2O_PERFMAP=<dir>` makes the loader write perf-<pid>.map.
- What the code needs of the VM it gets through imports. lib/c's mem* and
  str* functions are kernel imports (`__KERNEL`), so a program calls lib/c
  and lib/c the kernel, which implements them. The imports of memcpy,
  memmove and memset the compiler compiles itself, as it does the bulk
  memory instructions: it checks the ranges and calls the native memory
  functions `w2o.memcpy`, `w2o.memmove` and `w2o.memset` with host
  addresses, the destination, the source or byte and the length in AX, BX
  and CX, as Go passes them, Go's spill room reserved. The loader binds
  Go's `runtime.memmove` and its `hostMemset` to them; pg83/str's functions
  take their place later. A call through a pointer still reaches the
  kernel's `libcMemcpy`.
- A call of an import, or of a function of the VM, is a call of a Go
  function as Go makes it: the instance in AX, the arguments in Go's
  registers and stack slots, `call *GOT[symbol]`, the GOT holding the
  function's code address. No import is a closure: every function of the
  kernel, the WASI host and the tests is a named function of ctx. The
  allocator keeps nothing in registers across such a call; after it the
  code reads the instance from `g.instance`, a field our runtime adds to
  g, which the entry thunks set and restore and the resume and land stubs
  set, and the memory base from the instance; while the instance's
  interrupt word is set it calls the interrupt stub. The adapters
  `w2o.adapter.*` remain for the imports tables and exports name and for
  the `.jump` calls of imports that return twice. `bindContext(code, ctx)`
  makes a stub that calls code with ctx first: the spec runner binds a
  module's import to another module's export thunk with its instance.
- Compilation enables a physical 64 KiB readable and writable tail beyond
  the logical memory size by default: a check that an address is within
  the logical size admits every access reaching up to 64 KiB past it, so
  later accesses from the same address need no check; an address past the
  logical size traps. Machine-code range versioning lets one address guard
  cover related addresses within 64 KiB on the fast path, with each access
  checked in order on the fallback path. Versioning precedes register
  allocation; emission places fallback blocks after hot code, sends jumps
  past blocks that only jump, falls through blocks that emit nothing and
  uses multi-byte NOPs for loop alignment.
  `memory.size`, growth limits and the zero initialization of newly grown
  pages retain their logical meaning. The object records the extension;
  running it needs this loader but no environment setting.
  `W2X_MEMORY_TAIL=plain` disables range versioning for comparisons;
  `W2X_MEMORY_TAIL=strict` compiles with exact Wasm bounds. `wasm2o spec`
  and boundary-assertion unit tests select strict compilation explicitly.
  `W2X_TAIL_PROFILE=1` adds per-function counters to an optimized object;
  `W2O_TAIL_PROFILE=FILE` when running a stand or WASI command appends JSON
  records with executed guards, fallback entries, omitted checks and
  ordinary checks. Profiled objects are for counting, not timing.
- Speed is measured against native code by the stand: the C++ compiler
  compiling busybox.wasm and aot.wasm, natively and as aot.aot in wasm2o,
  the scalar modules of decode release 16 on their file corpora against
  the same wrappers compiled with the pinned ix libraries, and CPython's
  interpreter on the pure Python benchmarks of `dev/stand/pybench.py`
  (`--only py/NAME` selects one), each timed inside the interpreter; the
  suite enters the corpus mean as one workload, its own geometric mean.
- An import named setjmp, _setjmp, sigsetjmp, vfork, fork or forkpty returns
  twice: its direct calls go through an adapter that writes the caller's
  frame to the instance header (`abi.go`), and the caller keeps an
  invocation token below its saved RBP. `jump.go` returns from such a call
  again through the `w2o.land` stub, which checks the frame on the RBP chain
  and switches stacks with a `ret`, dropping the frames between, Go's too.
  For fork, `w2o.frames` copies the frames up to the entry thunk (they hold
  no stack address but the saved RBPs) and `w2o.resume` places them on
  another goroutine's stack; it writes below its stack pointer, so the
  loader marks it assembly, which Go never preempts, and the header's base
  keeps jump frames valid in the child. The register allocator needs nothing
  for it: values living across a call are in R10-R12, which the frame keeps,
  or in their vreg's own spill slot.
- The loader (`load.go`, `gort.go`, `wasi.go`, `run.go`, `jump.go`) is linux/amd64
  only and pinned to Go 1.25's moduledata. The C++ compiler targets WASI
  preview1. Code and its registration live as long as the process.
- `lib/goruntime` is our fork of Go's runtime, an overlay of files and
  anchored edits (`edits.py`) that `dev/go.py --runtime` applies to the
  native builds of wasm2o: `runtime.SetInterrupt` and `runtime.Interrupt`
  make the next preemption signal that finds a goroutine in the module's
  functions inject a call of `w2o.interrupt_call` there, as Go injects
  asyncPreempt. The stub saves every register and flag and calls Go; the
  same stub runs on the way out of any import while the instance's
  interrupt word is set. The kernel delivers signals there, below the
  shadow stack's 128-byte red zone, and repeats Interrupt every
  millisecond until they are taken.
- `wasm2o run` runs an object importing module `kernel` in the kernel of
  lib/c (`kernel*.go`). `libc.go` is its table: every function lib/c imports
  is a function `libcX(ctx, …)` there, a call of the Process method `libcX`
  that does the work, the process taken from ctx. A process is a goroutine with its own pid, group,
  descriptors (host descriptors the kernel owns), cwd, umask, limits and
  signals; paths resolve inside `--root` through openat2 `RESOLVE_IN_ROOT`,
  but for a few host devices (/dev/null and kin); exec runs only objects of
  wasm2o and `#!` scripts; wasm2o exits with the first process, which
  adopts orphans. The heap, environ, DIRs and libc's static results live in
  the guest's memory: fork copies it from the stack pointer to the heap's
  top, the instance and the frames, into a goroutine of its own; a child of
  vfork runs on its parent's goroutine and frames until it execs or exits,
  then the parent returns from vfork again. A stopped process blocks its
  goroutine until SIGCONT; the terminal's foreground group is the
  kernel's. A longjmp may leave a signal handler through the kernel's Go
  frames: the code that runs handlers holds no lock and defers nothing.
  `test_busybox` builds a root from busybox.links and runs applets and the
  shell in it: pipes, jobs, SIGINT, and Ctrl-Z, jobs and fg on a pty.
- `test_wasm2o_go` runs `bin/wasm2o/*_test.go`: modules built in the test
  run through the loader while Go collects, copies stacks, parks, preempts a
  call-free loop on one processor and unwinds traps through native frames;
  wide signatures cross both boundaries on the stack; longjmp lands across a
  copied stack, through Go frames and under collection, and refuses a frame
  that returned; fork copies 10000 frames and memory, and the child's
  longjmp to the parent's setjmp lands; a call-free loop interrupted
  thousands of times under collection keeps its registers and flags.
- `test_wasm2o_spec` runs the spec testsuite (pinned in build.py) through
  `wasm2o spec`, which loads every module in its own process; wast2json
  comes from `ix run bld/wabt`. `KNOWN` in `tst/wasm2o_spec.py` lists the
  failures of what is not implemented (table.grow, more than one memory); a
  file failing more, or crashing, fails the test. Unsupported features are
  refused at decoding, never compiled approximately: memory64 and multiple
  memories are refused because the bounds checks assume one 32-bit memory.
- `test_wasm2o` checks the fixed point against native C++: embedded AOT,
  self-compiled and ld.lld-relinked commands, and aot.wasm under wazero
  compile aot.wasm and busybox.wasm to identical bytes. `test_aot` also
  checks both WASI entry points, malformed modules and repeated reactor calls.
- `test_doom` plays the shareware demo twice headless (`DG_HEADLESS=1`)
  demanding the same frame hashes and the timedemo exit, then a pty session:
  the alternate screen, truecolor half blocks and Ctrl-C restoring the
  terminal. `bin/doom/make.py` builds the pinned doomgeneric release with our
  terminal backend, patching one call through a cast the wasm table types
  would refuse; `doom1.wad` comes with its sha256.
- Go modules (the wazero runner's) are pinned by `go.mod` and `go.sum`;
  `dev/go.py download` fetches them in a node of its own and the build runs
  offline.

## bin/python

- `make.py core` compiles CPython 3.14 from its tarball into libpython.a,
  the Makefile's objects and the built-in modules (Setup.bootstrap's and
  MODULES), and links the freezer: no wasm2o, so a compiler change rebuilds
  none of it. `make.py python` compiles the freezer with wasm2o, runs it in
  the kernel on every module the Makefile freezes, links python.wasm and
  writes python314.zip: Lib without tests, stored, with
  `_sysconfigdata__pizdos_.py` made from pyconfig.h.
- `pyconfig.h` is the configuration: a HAVE_ line names what lib/c has.
  Threads are CPython's stubs; nothing needs more than lib/c (no sockets,
  zlib or ctypes); sys.platform is `pizdos`.
- `test_python` runs a script of the kernel's features and CPython's
  regrtest on TESTS; tests that need sockets, threads or the C API's test
  modules stay out, as do those relying on the C stack's depth, which the
  shadow stack does not measure (CPython skips them on WASI).

## bin/busybox

- `config` is the whole configuration: a bool not listed is off, ints and
  strings take their declared defaults, `select` is followed. `gen.py`
  replaces kconfig and busybox's host-side generators; nothing is compiled
  for the host.
