# pizDOS

An operating system whose kernel is an ordinary program: a WebAssembly VM
written in Go. A process is a wasm module run by a goroutine; a system call
is a call of a kernel import. Bare metal is a port of that program to
hardware, not a different kernel.

## layout

```
bin/aot/      wasm to native code ahead of time with Cranelift (Rust, no_std): module bytes in, artifact bytes out
bin/aot/cli/  aot as a host tool: files, all cores
bin/aot/wasm/ aot as a wasm module with no imports, for the kernel to run
bin/busybox/  busybox for the system: our config and the generator that replaces kconfig
bin/kernel/   the kernel (Go): boots from the rootfs it embeds
bin/rootfs/   the root filesystem service (Go, wasip1) with a read-only layer that holds itself
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

## native code

- `aot` compiles every defined function of a module with Cranelift for
  x86-64-v2 into one artifact (`bin/aot/src/artifact.rs`): code, a function
  table with frame sizes, and relocations to kernel imports and helpers.
- Every function is called with the SystemV convention and the process
  context as its first argument; the context's layout, the helpers and the
  trap codes are in `bin/aot/src/abi.rs`. Memory accesses are bounds-checked
  and code never traps by itself: a failed check calls the kernel's `trap`.
- On entry and on every loop header the code compares its stack pointer with
  a limit in the context and calls the kernel's `stack_check` below it.
- `aot.wasm` exports `aot_input`, `aot_compile`, `aot_output` and
  `aot_output_size` (`bin/aot/wasm/src/lib.rs`) and imports nothing; its
  library is core, compiler_builtins and alloc, built from Rust's sources.

## boot

- `rootfs.wasm` is a Go wasip1 reactor with a read-only layer embedded in it
  (`bin/rootfs/main.go` has the format and the API): `/bin/rootfs`, the
  module itself, `/bin/aot` and busybox with its applet links.
- The kernel embeds `rootfs.wasm` and starts it under wazero, checks that
  `/bin/rootfs` is the module it started, takes `/bin/aot` from it and
  compiles rootfs and aot with it. wazero is only this first stage; running
  the artifacts natively is the next one, not yet.
- The rest of the kernel sees the system through `wireBoard`: a `Compiler`
  and an `FS`.

## building

```
./build          # libc.a, busybox, aot, aot.wasm, the .aot artifacts, rootfs.wasm and the kernel
./build test     # the tests in tst/
```

Needs `clang` with the wasm32 target, `wasm-ld`, `llvm-ar`, `rustc`, `go` and
`python3` on `PATH`; here `ix run bld/rust/96 -- ./build` provides rustc. The
musl and busybox tarballs, the crates of `bin/aot/crates.toml` and rust-src
of rustc's release are fetched by the graph and checked against their sha256;
the kernel's Go modules come from `go mod download`, checked against
`bin/kernel/go.sum`.
