# CLAUDE.md

pizdos: an OS whose kernel is a Go program running wasm processes. Read
README.md for the layout and the C ABI, STYLE.md and STYLE_PRJ.md for style.

## Build and test

- `./build` builds `libc.a`, `busybox.wasm`, `busybox.links`,
  `bin/busybox/abi.txt` (every kernel import busybox uses), `aot`,
  `aot.wasm`, `aot.self` and the wazero runner. Run it as
  `ix run lib/c bld/compiler -- ./build`: aot.self links the host's libc.
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

## bin/aot

- The compiler is Go with no dependencies, one `package main`: a module's
  bytes in, an ELF object and a C header out (`compileModule`); file access
  stays in `main.go`. `rt/w2x.h` is the whole contract with the host.
- `test_aot` is the fixed point: aot.self and aot.wasm under wazero compile
  aot.wasm and busybox.wasm to the bytes of the native `aot`. A change to
  the compiler keeps it.
- The spec testsuite runner lives in `../wasm/tst/spec.py` and needs wabt
  (`ix run lib/c bld/compiler bld/wabt`); its baseline is 27036 passed and
  59 failed (memory64, multi-memory, table.grow in its driver).
- Go modules (the wazero runner's) are pinned by `go.mod` and `go.sum`;
  `dev/go.py download` fetches them in a node of its own and the build runs
  offline.

## bin/busybox

- `config` is the whole configuration: a bool not listed is off, ints and
  strings take their declared defaults, `select` is followed. `gen.py`
  replaces kconfig and busybox's host-side generators; nothing is compiled
  for the host.
