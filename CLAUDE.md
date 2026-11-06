# CLAUDE.md

pizdos: an OS whose kernel is a Go program running wasm processes. Read
README.md for the layout and the C ABI, STYLE.md and STYLE_PRJ.md for style.

## Build and test

- `./build` builds `libc.a`, `busybox.wasm`, `busybox.links`,
  `bin/busybox/abi.txt` (every kernel import busybox uses), `aot`,
  `aot.wasm` and `busybox.aot`. rustc comes from `ix run bld/rust/96 --
  ./build`.
- `./build test` runs `tst/test_*.py`, each with the outputs of the node
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

- `bin/aot` is the compiler as a no_std library, a module's bytes in and an
  artifact's bytes out; `cli/` (host tool) and `wasm/` (module with no
  imports) are the only places that touch the outside. Keep it that way: no
  std, no I/O, no threads in `bin/aot/src`.
- `crates.toml` is the whole crate set, resolved by hand: build.py compiles
  each crate with rustc through `dev/crate.py`, never cargo, once per target
  that needs it (host, wasm32). `features` is the exact feature set; cargo's
  defaults and unification do not apply. Everything aot links is no_std.
- wasm32's core, compiler_builtins and alloc (`sysroot` entries) are built
  from rust-src of rustc's own release; build.py refuses another rustc.
- A crate with `sha256` is fetched from crates.io and never edited. A
  third-party crate we have to patch is kept whole under
  `bin/aot/crates/<name>-<version>/` and named by `path`; none is patched yet.
- `src/abi.rs` is the contract with the kernel and `src/artifact.rs` the file
  format; a change to either bumps the artifact version.
- Our crate builds with `-D warnings` and is formatted by rustfmt with
  `bin/aot/rustfmt.toml`.
- `tst/test_aot.py` runs aot's code through ctypes (`tst/aot/harness.py`)
  against wasm semantics written out in Python; a new operator gets a case
  in `tst/aot/ops.c` there. `tst/test_aot_wasm.py` runs aot.wasm as aot
  compiled it and checks it compiles ops.wasm and itself to the host tool's
  bytes (about two minutes).

## bin/busybox

- `config` is the whole configuration: a bool not listed is off, ints and
  strings take their declared defaults, `select` is followed. `gen.py`
  replaces kconfig and busybox's host-side generators; nothing is compiled
  for the host.
