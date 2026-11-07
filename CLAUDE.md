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
  `main.go`. `meta.go` is the object's description for a loader, `goabi.go`
  the contract with Go: code runs on a goroutine's stack under Go 1.25 amd64
  ABIInternal, R14 is g, RBP the frame pointer, X15 untouched, and the stack
  pointer table must be exact at every instruction, since Go preempts the
  code by signals and copies its stack.
- The loader (`load.go`, `gort.go`, `wasi.go`, `run.go`) is linux/amd64
  only and pinned to Go 1.25's moduledata; the compiler alone builds for
  wasip1 too. Code and its registration live as long as the process.
- `test_wasm2o` is the fixed point: wasm2o.o under `wasm2o run` and
  wasm2o.wasm under wazero compile wasm2o.wasm and busybox.wasm to the bytes
  of the native `wasm2o`. A change to the compiler keeps it.
- Go modules (the wazero runner's) are pinned by `go.mod` and `go.sum`;
  `dev/go.py download` fetches them in a node of its own and the build runs
  offline.

## bin/busybox

- `config` is the whole configuration: a bool not listed is off, ints and
  strings take their declared defaults, `select` is followed. `gen.py`
  replaces kconfig and busybox's host-side generators; nothing is compiled
  for the host.
