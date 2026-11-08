# pizDOS

An operating system whose kernel is an ordinary program: a WebAssembly VM
written in Go. A process is a wasm module run by a goroutine; a system call
is a call of a kernel import. Bare metal is a port of that program to
hardware, not a different kernel.

## layout

```
bin/wasm2o/   Go loader and kernel; invokes its embedded aot.aot to compile wasm
bin/aot/      wasm to x86-64 compiler in one freestanding C++ file, targeting WASI preview1
bin/busybox/  busybox for the system: our config and the generator that replaces kconfig
bin/kernel/   the kernel (Go), not yet
bin/rootfs/   the root filesystem service (Go, wasm), not yet
lib/c/        the C library processes link: headers, the in-process part, the musl subset
lib/goruntime/ our fork of Go's runtime: what wasm2o adds to it, as an overlay
dev/          toolchain helpers shared by the graph
tst/          tests
tst/wazero/   a WASI runner on wazero, what wasm2o is measured against
build         the build engine (shared across the monorepo)
build.py      the build graph
```

## the C ABI

- wasm32, `long double` is 64-bit, `time_t`, `off_t`, `ino_t`, `dev_t` are
  64-bit. Everything is linked statically; there is no `dlopen`.
- Most of libc is the kernel: a header declares such a function with
  `__KERNEL(name)` and the program imports it from module `kernel`.
- The process keeps stdio, `qsort`/`bsearch`, libm, `getopt`, number
  parsing and multibyte conversion (taken from musl), plus `memcpy`,
  `memmove`, `memset` (`memory.copy`/`memory.fill`), `exit`/`atexit`,
  `popen`, `syslog` and the crt.
- Start-up: the kernel calls the exported `_start`, which calls
  `__kernel_start(&errno, &environ, &argc, &argv)`; the kernel has placed the
  argument vector in the process heap and keeps `errno` and `environ`
  current. Signal handlers are called through the exported
  `__call_signal_handler` and `__call_signal_action`.
- Memory: the stack comes first (1 MiB, overflow traps), the heap starts at
  the exported `__heap_base` and is the kernel's to manage. `__stack_pointer`
  is exported too: `longjmp` restores it.
- `setjmp` and its kin, `vfork` and `fork` are imports that return twice;
  `wasm2o` recognizes them by name, and `longjmp` returns from them again,
  out of a signal handler too. `fork` copies memory from the stack pointer
  to the top of the heap, so the stack must come first.

## native code

`./build aot aot_command` builds the C++ compiler without a sysroot, libc,
or a C++ runtime. `aot.wasm` exports `void compile()`: it reads wasm from
WASI preview1 fd 0 and writes ELF to fd 1. Input is stored in 64 KiB pages;
output is streamed by section, including data segments borrowed from input.
The host supplies the two descriptors through WASI, with no guest-memory ABI.

`./build wasm2o` constructs the complete bootstrap through `build.py`:

```
aot.cpp --host clang--> native aot
        --wasm clang--> aot.wasm
native aot < aot.wasm > aot.aot
wasm2o = Go loader + embedded aot.aot
```

The `aot_aot` node creates `aot.aot`; `dev/go.py --embed` copies it into the
Go build tree for `go:embed`. No pre-existing wasm2o binary is needed.
At run time wasm2o links the object once, creates an instance for each
compilation, calls its `compile` export, and releases the instance and its
linear memory. The CLI streams its input and output files through WASI fd
0 and fd 1. It needs no external compiler, wasm interpreter or temporary
compiler executable.

`aot-command.wasm` also exports `_start`: wasm on stdin, ELF on stdout.
`./build aot_native` builds the equivalent native bootstrap command using
host libc and the usual `CPPFLAGS`/`LDFLAGS`. Its optional `--strict`
selects exact Wasm bounds. The reactor's `compileOptions(flags)` export
selects the tail, range optimization and profiling with bits 0, 1 and 2;
`compile()` uses flags 3. The spec runner calls the embedded compiler with
flags 0 for exact Wasm semantics.

The C++ implementation adapts `trustme/bin/rustc/malloc.cpp`: 2 MiB segments,
64 KiB payload pages, class-local bump allocation and intrusive free lists.
Size classes step by 16 bytes through 256 bytes, then four classes per power
of two, up to 64 KiB. Empty pages can immediately serve another size class.
There are no multi-page object runs or large-object mappings. Every object
allocation, including input and output storage, is at most 64 KiB.
Native segments use libc
`mmap`; WASI segments use `memory.grow`, with metadata outside payload pages.
Pool chunks still grow geometrically up to 64 KiB. SSA values, operands and
machine code use owning page vectors. SSA blocks have a dedicated ObjPool;
machine blocks and optional instruction operands also live in ObjPool.
Their vectors release old buffers on growth and
replacement. Copied slice and map fields elsewhere remain views into pool
storage. Separate pools hold module structures, SSA, and machine IR; SSA is
released before register allocation, and machine IR after each function.
CFG and loop analyses use temporary pools. Container `operator[]` follows
the C++ in-range contract; `at()` checks the index. Input reader bounds and
validation remain checked.
Pending register intervals merge a sorted initial stream with a heap of
splits, preserving stable ties. Predecessor queries use epoch markers
instead of quadratic prefix searches. Coalescing visits blocks by loop
depth without sorting individual copies; interval uses are deduplicated
and ordered as the backwards traversal builds them.
Liveness revisits only predecessors affected by a changed live-in set.
Interval and active-register tables own their buffers; resolve walks split
children directly and emits moves without a separate counting pass.
`./build test_aot` checks both WASI entry points against wasm2o,
including compilation of the new compiler itself and repeated export calls.

`./build aot_native aot_measure wasm2o` builds both compiler forms and a
small `wait4` measurement parent. Run
`python3 dev/aot_perf.py --json .build/aot-perf.json --check` to compare all
seven main inputs using native C++ and the embedded AOT. Every ELF must
match byte for byte. Three fresh-process runs alternate compiler order;
the report records user+system CPU and peak RSS. Optional
`--max-cpu-ratio` and `--max-rss-ratio` bound embedded/native overhead.
Compiler changes are measured against a saved revision on the same wasm
inputs; generated code may change, provided its execution on the stand
does not slow down. `--compiler-input saved/aot.wasm` freezes the compiler
workload, and `--objects saved/objects` keeps the verified ELF outputs.
`dev/stand.py --only im,pdf,djvu,svg,magic --objects saved/objects` measures
those outputs on the pinned corpus. Compare saved and candidate objects
with the same loader and runtime settings.

- `wasm2o` compiles a module ahead of time to an ordinary x86-64 ELF
  relocatable object (written by `bin/aot/aot.cpp`, read by `bin/wasm2o/elf.go`): code in `.text`, constants and
  segments in `.rodata`, stack pointer tables as `.eh_frame`, calls of Go
  through the GOT, imports and exports of functions as symbols that carry
  their wasm types (`bin/wasm2o/symbols.go`), and the rest of the module's
  description in a `.note.wasm2o`. `ld.lld -r` relinks it and the loader
  runs the result. Memory accesses use explicit range guards and a trap
  calls Go. Linear memory is reserved at its maximum and grows in place.
  The default AOT semantics allow reads and writes into an additional
  64 KiB tail beyond the logical size. `memory.size` and growth limits
  remain logical; newly grown pages are zeroed. Generic range versioning
  before register allocation shares one guard across related accesses;
  checked fallbacks live at the end of the function. No compiler flag is
  needed. `W2X_MEMORY_TAIL=strict` selects exact Wasm bounds for diagnostics,
  and the spec runner always uses those bounds.
- The code runs on a goroutine's stack under Go 1.25 amd64 ABIInternal
  (the ABI in `bin/aot/aot.cpp`): exports are functions Go calls, imports are Go
  function values, prologues grow the stack through Go's morestack, and the
  loader registers every code region with Go's runtime, so the garbage
  collector, stack copying, panics, tracebacks and asynchronous preemption
  go through the frames. Our fork of the runtime (`lib/goruntime`) lets the
  kernel interrupt that code the way Go preempts it, for the processes'
  signals.
- `wasm2o run [--root dir] [-a argv0] object.o -- args` is that loader: a
  WASI preview1 command, or, for an object importing `kernel`, a program of
  lib/c run by a kernel in Go (`bin/wasm2o/kernel*.go`). Its processes are
  goroutines, paths resolve inside the root, exec runs objects of wasm2o and
  `#!` scripts; `fork` copies a process into a goroutine of its own, a child
  of `vfork` borrows its parent's. The
  build makes native `wasm2o` with `aot.aot` embedded. Fixed-point tests
  compare native C++, embedded AOT, self-compiled and relinked commands,
  and the reactor under wazero on aot.wasm and busybox.wasm.

## building

```
./build          # libc.a, busybox, native aot, aot.wasm, aot.aot, wasm2o, wazero
./build test     # the tests in tst/, the spec testsuite included
sh dev/stand/build.sh
                 # the full stand, with its pinned ix toolchain and libraries
dev/stand.py --runs 5 --json .build/stand/results.json
                 # native (A), wasm2o (C), wasm2c (R); the goal is C/A <= 1.2
dev/stand.py --only im,pdf,djvu,svg,magic
                 # just the C/C++ decoder workloads
```

Needs `clang` with the wasm32 target, `wasm-ld`, `llvm-ar`, Go 1.25 and
`python3` on `PATH`, and `wast2json` for the spec testsuite;
`ix run bld/compiler bld/wabt` provides them. The musl and
busybox tarballs are fetched by the graph and checked against their sha256;
the wazero runner's Go modules come from `go mod download`, checked against
`tst/wazero/go.sum`.

The stand's build script keeps a private ix checkout in `.build-stand/ix`,
at the revision that produced [decode release 16](https://github.com/impulse-desktop/decode/releases/tag/16).
It supplies the native ImageMagick, PDFium, DjVuLibre, SVG and libmagic
libraries, wasm2c and the host compiler, then runs `./build stand`.
`dev/stand/scalar.py` applies reproducible edits to that private checkout:
native codec libraries and wrappers use -O3 with loop/SLP vectorization,
AVX and FMA disabled. The codecs' own SIMD dispatch is disabled as well
(scalar Highway for JPEG XL, generic libaom, portable libdeflate). SSE
registers remain available for the x86-64 scalar floating-point ABI.
AVIF uses libaom on both sides. The scalar flag is scoped to the decoder
libraries so the host compiler keeps its normal build.
The graph fetches the release's scalar wasm modules and the same revision's
wrappers, checks their hashes and builds each through wasm2o, bounds-checked
wasm2c with clang -O3 and ThinLTO, and the native libraries. Normal builds
and tests do not need these native libraries.

The [corpus](https://github.com/pg83/store/blob/1a4411b2ee6fdf6c3a05b7ddafd4d71ab79dbb54/decode-corpus-1.tgz)
is 89 files (36 images, 6 PDFs, 3 DjVus, 44 SVGs), about 49 MiB compressed.
It carries `MANIFEST` with sources and conversions, and `SHA256SUMS`.
`dev/stand/corpus.sh` regenerates it with the encoders it names; v1 was
generated with ix `5cea87b37c702d257c19fba698dea94aa4ce6dd2`, and two
complete builds with that toolchain produced identical archives.

For compilation the stand compares native C++, the embedded AOT in wasm2o,
and aot-command.wasm through wasm2c, compiling busybox.wasm and aot.wasm.
The complete ELF outputs must match. These times include
startup. For decode it opens each file in the same process and decodes an
image or renders up to 16 pages at a longest side of 1600 pixels; libmagic
classifies all 89 files. The measured work includes input allocation and
copying, calls and freeing their results; startup, linking, file I/O and
host pixel hashing are excluded. Short workloads repeat in the same instance to
reach at least 0.25 seconds per reference sample. A, C and R run interleaved,
with `GOMAXPROCS=1`, `MAGICK_THREAD_LIMIT=1` and `OMP_NUM_THREADS=1`.

C and R must agree exactly on every result, including the hashes of rendered
RGBA pixels. Native must agree on success, dimensions and page counts;
its pixel hashes can differ with floating point and libm, and
the report states how many match exactly. `--json` saves the samples and
every file's result. The original positional `dev/stand.py` invocation still
runs the Go workloads alone; `./build stand_bin` builds their wasm2c reference.
