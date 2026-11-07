# pizDOS

An operating system whose kernel is an ordinary program: a WebAssembly VM
written in Go. A process is a wasm module run by a goroutine; a system call
is a call of a kernel import. Bare metal is a port of that program to
hardware, not a different kernel.

## layout

```
bin/wasm2o/   wasm to x86-64 (Go): a module in, an ELF object out; `wasm2o run` loads one onto a goroutine
bin/busybox/  busybox for the system: our config and the generator that replaces kconfig
bin/kernel/   the kernel (Go), not yet
bin/rootfs/   the root filesystem service (Go, wasm), not yet
lib/c/        the C library processes link: headers, the in-process part, the musl subset
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
  the exported `__heap_base` and is the kernel's to manage.

## native code

- `wasm2o` compiles a module ahead of time to an ELF relocatable object:
  code, absolute relocations to the symbols a loader defines, and a `.w2o`
  section describing the module (`bin/wasm2o/meta.go`). The code needs no
  signal handlers, guard pages or reserved address space: memory accesses
  are checked explicitly, a trap calls Go, and linear memory may move when
  it grows.
- The code runs on a goroutine's stack under Go 1.25 amd64 ABIInternal
  (`bin/wasm2o/goabi.go`): exports are functions Go calls, imports are Go
  function values, prologues grow the stack through Go's morestack, and the
  loader registers every code region with Go's runtime, so the garbage
  collector, stack copying, panics, tracebacks and asynchronous preemption
  go through the frames.
- `wasm2o run object.o -- args` is that loader with WASI preview1 in Go. The
  build makes `wasm2o` natively, `wasm2o.wasm` (the same compiler for wasip1)
  and `wasm2o.o`, wasm2o.wasm compiled by wasm2o: run by `wasm2o run` and
  under wazero, it compiles wasm2o.wasm and busybox.wasm to the bytes
  `wasm2o` produces.

## building

```
./build          # libc.a, busybox, wasm2o, wasm2o.wasm, wasm2o.o, the wazero runner
./build test     # the tests in tst/, the spec testsuite included
```

Needs `clang` with the wasm32 target, `wasm-ld`, `llvm-ar`, Go 1.25 and
`python3` on `PATH`, and `wast2json` for the spec testsuite;
`ix run bld/compiler bld/wabt` provides them. The musl and
busybox tarballs are fetched by the graph and checked against their sha256;
the wazero runner's Go modules come from `go mod download`, checked against
`tst/wazero/go.sum`.
