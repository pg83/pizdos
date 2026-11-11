# CLAUDE.md

pizdos: an OS whose kernel is a Go program running wasm processes. Read
README.md for the layout and the C ABI, STYLE.md and STYLE_PRJ.md for style,
doc/ for designs: `doc/fs.md`, file systems (no Linux namespace, the
kernel's `/sys`, file systems as processes speaking FUSE's protocol).

## Build and test

- `./build` builds `libc.a`, `libc++.a`, `libstr.o`, `busybox.wasm`,
  `busybox.links`, `bin/busybox/abi.txt` (every kernel import busybox uses),
  `doom.wasm`, `doom.o` (doomgeneric with our terminal backend, `bin/doom`),
  `llvm.wasm` and `llvm.o` (clang, `bin/clang`), `vm`, native `aot`,
  `aot.wasm`, `aot.aot` and the wazero runner.
- `./build test` runs `tst/test_*.py`, each with the outputs of the nodes
  `TESTED` names for it in build.py.
- `./build unit` runs every `tst/unit/**/*.cpp` as an end-to-end test:
  the host executable is a reference, native AOT and the embedded Wasm AOT
  compile the Wasm program to identical objects, and vm runs both. Each
  case is its own `unit_<name>` target and also belongs to `test`.
  `//@ compile-flags:`, `aot-flags:`, `run-flags:`, `args:` and `env:`
  configure the commands; `stdout:` and `stderr:` are JSON strings,
  `exit-code:` defaults to 0. `host: false` is for kernel-specific cases.
  Tests exercise program behaviour; tests of internal code are forbidden.
- `./build coverage` reruns the end-to-end cases with separate instrumented
  tools (`aot_coverage`, `vm_coverage`), including the Python and spec suites.
  The host supplies GCC 12–15, its assembler, gcov and gcov-tool; select the
  compiler with `COVERAGE_CXX` if g++ is not on PATH. `COVERAGE_TESTS` selects
  case names by a regular expression for a focused measurement. Production
  builds use clang and carry no profiling. Reports in `.build/coverage`
  list AOT line/branch coverage, VM statement coverage, every case run and
  uncovered locations. `coverage_check` enforces 99% AOT lines and VM
  statements; an ordinary report remains available below that threshold.
- Fault injection uses separate `aot_native_chaos`, `aot_wasm_chaos` and
  `vm_chaos` builds. `PIZDOS_AOT_CHAOS=read@N,write@N,grow@N` fails once
  after N visits; vm's rules use `PIZDOS_VM_CHAOS` and points `mmap`,
  `compile-read`, `compile-write`, `memory-grow`, `openat`, `read`, `write`
  and `fork`. AOT's Wasm reactor is configured by its chaos-only export.
  C++ uses `AOT_CHAOS`, Go uses the `pizdoschaos` build tag; ordinary
  builds erase the hooks and ignore those environment variables.
  `test_chaos` checks the actual commands, failures, subsequent recovery
  and atomic compiler output.
- `sh dev/stand/build.sh` supplies the pinned ix revision's libraries and
  tools and runs `./build stand`: decode's im, pdf, djvu, svg and magic
  modules through vm, and their wrappers against the native
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
  other function is our code in `lib/c/src`, most of them calls of the
  kernel's imports. `mbsrtowcs` is ours, a character at a time: musl's
  reads words past a string's end, which the address sanitizer reports.
- Programs see only lib/c's functions. An import is lib/c's alone, declared
  `__KERNEL(name)` under the C name `__kernel_<name>`, and the function
  `name` calls it: `src/kernel.c` defines those by one line each, a
  variadic one passing its va_list on, the pointer the import took when the
  function was the import. malloc, calloc, realloc and free and the eleven
  string functions below are `__INLINE` in their headers (GNU inline,
  always inlined, never emitted), so a call is the import's call, and
  defined in `src/mem.c` for a pointer; setjmp, _setjmp, sigsetjmp, fork,
  vfork and forkpty are `__INLINE` only, their import's call in the
  caller's frame, with no definition to point to.
- Headers are written for this system, not copied from musl or glibc;
  constants use Linux generic values, struct layouts are fixed by the ABI
  because the kernel reads and writes them.
- The imports of memcpy, memmove, memset, memcmp, memchr, strlen, strcpy,
  strcat, strcmp, strspn and strcspn are named `__kernel_<name>`: only the
  compiler implements them, at each call.
- A process has one thread: `pthread.h` is lib/c's own, its locks never
  wait for another thread and `pthread_create` fails; `locale_t` is the one
  C locale with UTF-8 strings, and every `_l` function the function without
  the locale. `_start` runs the module's constructors; `__cxa_atexit` and
  `__cxa_thread_atexit` add to atexit's list, which grows as needed. A
  program is one object: `dlopen` fails and `dladdr` names the program, the
  path the kernel exec'd through its links (`__kernel_program`).
- The link has no `--allow-undefined`: a missing function is a link error,
  not a silent import.
- `test_libc` runs tst/libc/check.c, built against lib/c, in the kernel:
  one line of results per area, compared with the test's expected text.

## lib/cxx

- libc++ and libc++abi of the LLVM release `LLVM` pins in build.py, compiled
  by `lib/cxx/make.py` with the flags and sources of LLVM's runtimes build
  for this system but none of its probes; `__config_site` is ours: musl-like
  libc, lib/c's pthread, the filesystem library, no time zone database, no
  exceptions, and the unstable ABI (version 999) but for the alternate
  string layout, as ix's libc++ has it. `libc++.a` holds both libraries, `include.tar` the
  `include/c++/v1` a C++ program puts before lib/c's headers.
- libc++ takes the generic locale path of a libc without xlocale: it
  defines `strto*_l` and, in C++, `vasprintf` itself, which lib/c therefore
  does not declare for C++.
- `test_cxx` runs tst/cxx/check.cpp, built against lib/cxx, in the kernel:
  one line of results per area between its static objects' lines.

## lib/str

- pg83/str's string and memory functions, fetched at the revision `STR`
  pins in build.py and compiled by str's own compiler (`dev/strcc.py`,
  Python) with `--abi wasm2o` into `libstr.o`, the object vm's loader
  links (`lib/str/make.py`); str's check of that contract (`tst/wasm2o.py`)
  passes before the object is kept.
- The convention is the one between the module's functions: arguments in
  RAX, RCX, RDX, RSI, RDI, R8, the result in RAX, RBX and R9-R12 kept;
  RBP, R13-R15 and vector register 15 never touched; the stack aligned to
  8 bytes only; an `.eh_frame` with the CFA at every instruction. The
  functions take host addresses and check nothing: the caller checks the
  ranges. `str_choose()` gives the host's configuration, whose table holds
  the functions to call. `bin/vm/str.go` links it.
- Changes to the functions or the convention are made in str and the
  revision moved here; the library is never edited in place.

## bin/vm

- The compiler is `bin/aot/aot.cpp`, one freestanding C++ translation unit.
  `build.py` builds native `aot` and `aot.wasm`, then native aot compiles the
  reactor to `aot.aot`. `vm` embeds that object through `go:embed`;
  `dev/go.py --embed` stages it for native builds. There is
  no Go compiler implementation or vm.wasm bootstrap.
- Compiler optimization aims to reduce compilation CPU while generated
  programs run no slower on the stand. ELF bytes may change between
  revisions; native and embedded forms of the same compiler still agree.
  Freeze the compiler's wasm input across revisions with
  `dev/aot_perf.py --compiler-input`, save objects with `--objects`, and
  run the saved and candidate objects through the same stand loader.
- `bin/vm` is vm, the Go loader, kernel and WASI host (the objects' note
  `.note.wasm2o`, the `w2o.` symbols and the `W2O_`/`W2X_` variables keep
  the name it had). `compile.go` links
  embedded code once and gives each invocation its own instance, memory and
  fd streams. `compile()` reads fd 0 and writes ELF to fd 1;
  `compileOptions(flags)` selects tail, range optimization and profiling
  with bits 0, 1 and 2. The default is 3, strict spec compilation is 0.
  Every invocation releases its instance even on compiler or I/O failure.
- The object is ordinary x86-64 ELF: `aot.cpp` writes it, `elf.go` reads it,
  `symbols.go` parses typed boundary symbols and `meta.go` reads the note.
  `abi.go` and `format.go` hold the loader's shared ABI constants and types.
  Generated code lives in Go 1.25 amd64 ABIInternal's world: R14 is g, RBP
  the frame pointer, X15 untouched, and the stack pointer table is exact at
  every instruction. Between the module's functions the convention is its
  own: arguments and results in RAX, RCX, RDX, RSI, RDI and R8, then on
  the stack; RBX and R9-R12 callee-saved; R13 the instance, R15 the memory
  base. The thunks Go calls and the adapters of tables' and exports'
  imports move between the conventions. The stubs the code calls are the
  loader's, Go assembly in `stubs_amd64.s`, which Go never preempts, and
  the object reaches them through the GOT: a stack check calls
  `w2o.morestack` with what it compared on the stack, and the stub compares
  that with `g.stackguard0` again before Go's morestack, so a preemption
  that completed after the check does not grow the stack.
  `W2O_PERFMAP=<dir>` makes the loader write perf-<pid>.map.
- What the code needs of the VM it gets through imports. A program calls
  lib/c and lib/c the kernel's imports, which the kernel implements, but for
  the eleven string functions' `__kernel_<name>`, which the compiler
  compiles at each call, as it does the bulk memory instructions: it checks
  the ranges and calls lib/str's function `str.<name>` with host addresses,
  the arguments in AX, CX and DX, as the module's functions call each other:
  the call changes what such a call may and keeps the instance, the memory
  base and the callee-saved registers. memcpy, memmove and memset of a
  constant length of 32 bytes or less are loads and stores, as a native
  compiler makes them, since programs compile with `-fno-builtin`. The
  others keep the kernel's semantics: memcmp checks both ranges and makes
  the result -1, 0 or 1; memchr searches up to the length or the memory's
  end, whichever is nearer, and traps when the end cut a search that found
  nothing; strlen checks the string's start before the call and its NUL
  after it, and needs the zero byte the loader keeps past the accessible end
  (`newMemory`) to stop a string that runs to it; strcpy and strcat are
  strlen and a memmove of the string and its NUL, the destination's range
  checked first; strcmp, strspn and strcspn take both strings' strlen before
  lib/str's function, strcmp's result -1, 0 or 1. Such an import has no
  adapter and no code of its own: a module that exports it, puts it in a
  table or a global or takes it with ref.func is refused. The loader links
  lib/str's object once (`str.go`), registered with Go as assembly, never
  preempted asynchronously, calls its `str_choose()` and binds each
  `str.<slot>` to the function of that slot in the configuration it returns:
  the GOT holds the host's function itself, no table between. The stand
  copies its inputs into a module with the same memmove.
- A call of an import, or of a function of the VM, is a call of a Go
  function as Go makes it: the instance in AX, the arguments in Go's
  registers and stack slots, `call *GOT[symbol]`, the GOT holding the
  function's code address. No import is a closure: every function of the
  kernel, the WASI host and the tests is a named function of ctx. The
  allocator keeps nothing in registers across such a call; after it the code
  reads the instance from `g.instance`, a field our runtime adds to g, which
  the entry thunks set and restore and the resume and land stubs set, and
  the memory base from the instance; while the instance's interrupt word is
  set it calls the interrupt stub. An import has an adapter `w2o.adapter.*`
  only when the module names it, in an element, an export or a global, which
  is how a pointer reaches it (ref.func keeps to those), and one that
  returns twice has its `.jump` for its calls. `bindContext(code, ctx)`
  makes a stub that calls code with ctx first: the spec runner binds a
  module's import to another module's export thunk with its instance.
- Compilation enables a physical 1 KiB readable and writable tail beyond
  the logical memory size by default (`boundSlack`): a check that an
  address is within the logical size admits every access reaching up to
  1 KiB past it, so later accesses from the same address need no check; an
  address past the logical size traps. Machine-code range versioning lets
  one address guard cover related addresses within 1 KiB on the fast path,
  with each access checked in order on the fallback path. Versioning precedes register
  allocation; emission places fallback blocks after hot code, sends jumps
  past blocks that only jump, falls through blocks that emit nothing and
  uses multi-byte NOPs for loop alignment.
  `memory.size`, growth limits and the zero initialization of newly grown
  pages retain their logical meaning. The object records the extension;
  running it needs this loader but no environment setting.
  A module must keep nothing in its first 1 KiB: every data segment at
  1 KiB or more, wasm-ld's default, and the stack pointer starting there
  (our toolchain links the stack first with 1 KiB past the program's
  budget); the compiler refuses one that does not. The loader maps 1 KiB
  of accessible memory before the linear memory too. The accesses through
  base + c, with c within 1 KiB either way, are then covered by one check
  of the base and addressed from it in 64 bits: a sum that wrapped around
  2^32 is addressed as if it had not, into the free low memory or the
  slack before it, as an access reaching past the end lands in the tail.
  The two sides of the ring are the one constant.
  `W2X_MEMORY_TAIL=plain` disables range versioning for comparisons;
  `W2X_MEMORY_TAIL=strict` compiles with exact Wasm bounds. `vm spec`
  selects strict compilation explicitly.
  `W2X_TAIL_PROFILE=1` adds per-function counters to an optimized object;
  `W2O_TAIL_PROFILE=FILE` when running a stand or WASI command appends JSON
  records with executed guards, fallback entries, omitted checks and
  ordinary checks. Profiled objects are for counting, not timing.
- Speed is measured against native code by the stand: the C++ compiler
  compiling busybox.wasm and aot.wasm, natively and as aot.aot in vm,
  the scalar modules of decode release 16 on their file corpora against
  the same wrappers compiled with the pinned ix libraries, and CPython's
  interpreter on the pure Python benchmarks of `dev/stand/pybench.py`
  (`--only py/NAME` selects one), each timed inside the interpreter; the
  suite enters the corpus mean as one workload, its own geometric mean.
- An import named setjmp, _setjmp, sigsetjmp, vfork, fork or forkpty returns
  twice: its direct calls go through an adapter that writes the caller's
  frame to the instance header (`abi.go`), and the caller keeps an
  invocation token below its saved RBP. `jump.go` returns from such a call
  again through the land stub, which checks the frame on the RBP chain and
  switches stacks, dropping the frames between, Go's too. For fork, the
  frames stub copies the frames up to the entry thunk (they hold no stack
  address but the saved RBPs) and the resume stub places them on another
  goroutine's stack, writing below its stack pointer, which assembly may;
  the header's base keeps jump frames valid in the child. The register allocator needs nothing
  for it: values living across a call are in the callee-saved registers,
  which the frame keeps, or in their vreg's own spill slot.
- The loader (`load.go`, `gort.go`, `wasi.go`, `run.go`, `jump.go`) is linux/amd64
  only and pinned to Go 1.25's moduledata. The C++ compiler targets WASI
  preview1. Code and its registration live as long as the process.
- `lib/goruntime` is our fork of Go's runtime, an overlay of files and
  anchored edits (`edits.py`) that `dev/go.py --runtime` applies to the
  native builds of vm: `runtime.SetInterrupt` and `runtime.Interrupt`
  make the next preemption signal that finds a goroutine in the module's
  functions inject a call of the loader's `w2o.interrupt_call` there, as Go
  injects asyncPreempt. The stub saves every register and flag and calls
  Go; the same stub runs on the way out of any import while the instance's
  interrupt word is set. The kernel delivers signals there, below the
  shadow stack's 128-byte red zone, and repeats Interrupt every
  millisecond until they are taken.
- `vm run [--root dir] [-a argv0] [--asan] -- object.o arg...` runs an
  object importing module `kernel` in the kernel of lib/c (`kernel*.go`):
  the options, then `--`, the object and its arguments. `libc.go` is its table: every function lib/c imports
  is a function `libcX(ctx, …)` there, a call of the Process method `libcX`
  that does the work, the process taken from ctx. A process is a goroutine with its own pid, group,
  descriptors (host descriptors the kernel owns), cwd, umask, limits and
  signals; paths resolve inside `--root` through openat2 `RESOLVE_IN_ROOT`,
  but for a few host devices (/dev/null and kin); exec runs only objects of
  vm and `#!` scripts; vm exits with the first process, which
  adopts orphans. The heap's blocks, environ, DIRs and libc's static
  results live in the guest's memory, and the guest's memory holds data
  only: what the allocator knows of the heap is the kernel's, in Go
  (`kernel_heap.go`, trustme's pages of size classes, 64 KiB pages in
  segments of 32, runs of pages for more than 16 KiB), and every pointer
  freed is checked against it, so a program can neither corrupt the
  allocator nor free what is not its block. fork copies the memory from the
  stack pointer to the heap's top, the heap's state, the instance and the
  frames, into a goroutine of its own; a child of vfork shares its parent's
  heap and runs on its parent's goroutine and frames until it execs or
  exits, then the parent returns from vfork again. A stopped process blocks its
  goroutine until SIGCONT; the terminal's foreground group is the
  kernel's. A longjmp may leave a signal handler through the kernel's Go
  frames: the code that runs handlers holds no lock and defers nothing.
  `test_busybox` builds a root from busybox.links and runs applets and the
  shell in it: pipes, jobs, SIGINT, and Ctrl-Z, jobs and fg on a pty.
- A trap or a `fatal` of the kernel (an invalid pointer freed, say) ends
  the process with a trace through the module's and the kernel's frames as
  one stack (`trace.go`): Go's unwinder walks the registered functions, so
  `runtime.Callers` at the fault has them all. A line is a function of the
  module, named as the name section does, with the offset of its
  instruction and, when the module was linked with `-g`, its `file:line`;
  or a function of the kernel with its `file:line`. The loader's thunks,
  adapters and stubs (`w2o.*`) and the harness above the module's entry are
  left out; each line is marked `program[pid]`. The lines come from the
  module's DWARF line table: the compiler reads `.debug_line` (versions 2
  to 5, `moduleReadLines`), carries each value's and instruction's wasm
  position through lowering and emission (`asmMarkPos`), and writes the
  text's `pc → file:line` table into the note (`objCodeLines`), which the
  loader puts into Go's pcfile and pcln tables (`registerCode`); a trap's
  pad is of its check's instruction.
- `--asan` is the kernel's address sanitizer (`kernel_asan.go`): `vm --asan
  -o` compiles an object whose every access is checked against the shadow
  map, a byte for each byte of memory, instead of the memory's bounds
  (`lowererAsanAddress`: `cmp [shadow + address + offset], -1` with the
  map's base from an instance slot, the map covering every 32-bit address
  and boundSlack past them; the ranges of string functions and bulk
  operations through the loader's stub `w2o.asan_range`; a failed check
  calls `w2o.trap_at` with the address, width and kind), and `vm run
  --asan` runs it: the kernel keeps the map beside the memory from what it
  knows of the process, the stack and data from the first KiB to
  `__heap_base` accessible, a block of the heap accessible for the bytes
  asked for from when it is handed out, the rest of it a red zone, poisoned
  from when it is freed, and everything else never allocated; it checks
  the buffers of the program's calls against the map too (`span`), and
  reports a fault with what the map says of its first inaccessible byte.
  An asan object is refused without the flag and a plain one as the first
  program under it; exec of a plain object in an asan run runs it as ever.
  The code of plain objects is unchanged (their `.text` is identical);
  an asan object is about twice the code and the map doubles the memory
  touched. The compiler's output holds 512 MiB of code, which clang's
  multicall object exceeds under asan. `test_asan` runs tst/asan/check.c's
  memory errors and reads the reports and their lines.
- `test_vm_spec` runs the spec testsuite (pinned in build.py) through
  `vm spec`, which loads every module in its own process; wast2json
  comes from `ix run bld/wabt`. `KNOWN` in `tst/vm_spec.py` lists the
  failures of what is not implemented (table.grow, more than one memory); a
  file failing more, or crashing, fails the test. Unsupported features are
  refused at decoding, never compiled approximately: memory64 and multiple
  memories are refused because the bounds checks assume one 32-bit memory.
- `test_vm` checks the fixed point against native C++: embedded AOT,
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
  MODULES), and links the freezer: no vm, so a compiler change rebuilds
  none of it. `make.py python` compiles the freezer with vm, runs it in
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

## bin/clang

- `make.py` builds LLVM's multicall program, `llvm-driver` (clang,
  clang++, wasm-ld, ld.lld, llvm-ar and the rest by the name it runs as),
  from the release lib/cxx comes from, configured as ix builds clang 23:
  clang, lld and polly, every target, polly linked in, no shared
  libraries; one thread, no zlib, zstd, libxml2 or terminfo. CMake sees
  Pizdos, a Unix of wasm32 modules (a platform file and a toolchain
  make.py writes, dev/wasm.py's flags); libm, libpthread, librt and libdl
  are empty archives, as musl's are. The host gives cmake, ninja and the
  release's llvm-tblgen and clang-tblgen on PATH; nothing is built for it.
- One source edit, a wasm case in Path.inc's `is_local`; and
  `-DCLANG_BUILD_STATIC`, since clang's export macros test `__WASM__`,
  which no compiler defines.
- clang finds itself through `dladdr` and its headers in
  `lib/clang/<version>/include` beside `/bin/llvm`. A compile alone runs
  cc1 in the driver's process; one that also links runs cc1 and wasm-ld as
  children, fork and exec of the object, which must be executable.
- `test_clang` runs it in the kernel in a root with lib/c's and lib/cxx's
  headers and libraries: clang and clang++ compile and link a C and a C++
  program, which vm compiles and the kernel runs.

## bin/busybox

- `config` is the whole configuration: a bool not listed is off, ints and
  strings take their declared defaults, `select` is followed. `gen.py`
  replaces kconfig and busybox's host-side generators; nothing is compiled
  for the host.
