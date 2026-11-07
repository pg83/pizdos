# CLAUDE.md

pizdos: an OS whose kernel is a Go program running wasm processes. Read
README.md for the layout and the C ABI, STYLE.md and STYLE_PRJ.md for style.

## Build and test

- `./build` builds `libc.a`, `busybox.wasm`, `busybox.links` and
  `bin/busybox/abi.txt` (every kernel import busybox uses).
- `./build test` runs `tst/test_*.py`, each against the built busybox.
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

## bin/busybox

- `config` is the whole configuration: a bool not listed is off, ints and
  strings take their declared defaults, `select` is followed. `gen.py`
  replaces kconfig and busybox's host-side generators; nothing is compiled
  for the host.
