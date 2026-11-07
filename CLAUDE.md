# CLAUDE.md

pizdos: an OS whose kernel is a Go program running wasm processes. Read
README.md for the layout and the C ABI, STYLE.md and STYLE_PRJ.md for style.

## Build and test

- `./build` builds `libc.a`, `busybox.wasm`, `busybox.links`,
  `bin/busybox/abi.txt` (every kernel import busybox uses), `wasm2o`,
  `wasm2o.wasm`, `wasm2o.o` and the wazero runner.
- `./build test` runs `tst/test_*.py`, each with the outputs of the nodes
  `TESTED` names for it in build.py.
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

## bin/wasm2o

- The compiler is Go with no dependencies, one `package main`: a module's
  bytes in, an ELF object out (`compileModule`); file access stays in
  `main.go`. The object is ordinary ELF a linker can take (`elf.go`):
  symbols name imports and exports with their types (`symbols.go`),
  `.eh_frame` holds the stack pointer tables, `.note.wasm2o` only what ELF
  cannot say (`meta.go`). `goabi.go` is the contract with Go: code runs on a goroutine's stack under Go 1.25 amd64
  ABIInternal, R14 is g, RBP the frame pointer, X15 untouched, and the stack
  pointer table must be exact at every instruction, since Go preempts the
  code by signals and copies its stack.
- The pipeline: SSA from the locals (`frontend.go`), the SSA optimizer
  (`opt.go`: folding, jump threading with SSA repair, which turns the
  dispatch loop of a Go function into direct jumps, trivial parameters,
  dead code), lowering (`lower.go`), copy coalescing (`coalesce.go`),
  linear scan (`regalloc.go`), emission (`emit.go`). `W2X_DUMP=<function>`
  prints each stage, `W2X_CHECK=1` verifies the SSA after the optimizer and
  every allocation, `W2X_OPT_FUNCS=lo-hi` optimizes only those functions,
  for bisecting a miscompilation; `W2O_PERFMAP=<dir>` makes the loader
  write perf-<pid>.map for perf.
- Speed is measured against wasm2c in its bounds-checked mode (clang -O3,
  `WASM_RT_MEMCHECK_BOUNDS_CHECK`, memory on mmap, counted call depth):
  both run wasm2o.wasm compiling busybox.wasm and wasm2o.wasm, to the bytes
  of the native wasm2o.
- An import named setjmp, _setjmp, sigsetjmp, vfork or fork returns twice:
  its direct calls go through an adapter that writes the caller's frame to
  the instance header (`abi.go`), and the caller keeps an invocation token
  below its saved RBP. `jump.go` returns from such a call again through the
  `w2o.land` stub, which checks the frame on the RBP chain and switches
  stacks with a `ret`, dropping the frames between, Go's too. For fork,
  `w2o.frames` copies the frames up to the entry thunk (they hold no stack
  address but the saved RBPs) and `w2o.resume` places them on another
  goroutine's stack; it writes below its stack pointer, so the loader marks
  it assembly, which Go never preempts, and the header's base keeps jump
  frames valid in the child. The register allocator needs nothing for it:
  values living across a call are in R10-R12, which the frame keeps, or in
  their vreg's own spill slot.
- The loader (`load.go`, `gort.go`, `wasi.go`, `run.go`, `jump.go`) is linux/amd64
  only and pinned to Go 1.25's moduledata; the compiler alone builds for
  wasip1 too. Code and its registration live as long as the process.
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
  lib/c (`kernel*.go`): a process is a goroutine with its own pid, group,
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
- `test_wasm2o` is the fixed point: wasm2o.o under `wasm2o run`, the same
  object relinked by `ld.lld -r`, and wasm2o.wasm under wazero compile
  wasm2o.wasm and busybox.wasm to the bytes of the native `wasm2o`. A change to the compiler keeps it.
- Go modules (the wazero runner's) are pinned by `go.mod` and `go.sum`;
  `dev/go.py download` fetches them in a node of its own and the build runs
  offline.

## bin/busybox

- `config` is the whole configuration: a bool not listed is off, ints and
  strings take their declared defaults, `select` is followed. `gen.py`
  replaces kconfig and busybox's host-side generators; nothing is compiled
  for the host.
