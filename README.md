# pizdos

An operating system whose kernel is an ordinary program: a WebAssembly VM
written in Go. A process is a wasm module run by a goroutine; a system call
is a call of a kernel import. Bare metal is a port of that program to
hardware, not a different kernel.

## layout

```
bin/busybox/  busybox for the system: our config and the generator that replaces kconfig
bin/kernel/   the kernel (Go), not yet
bin/rootfs/   the root filesystem service (Go, wasm), not yet
lib/c/        the C library processes link: headers, the in-process part, the musl subset
dev/          toolchain helpers shared by the graph
tst/          tests
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

## building

```
./build          # libc.a, busybox.wasm, busybox.links and the ABI busybox needs (abi.txt)
./build test     # the tests in tst/
```

Needs `clang` with the wasm32 target, `wasm-ld`, `llvm-ar` and `python3` on
`PATH`. The musl and busybox tarballs are fetched by the graph and checked
against their sha256.
