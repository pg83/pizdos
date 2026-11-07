# Project style settings

Per-project settings that the shared [STYLE.md](STYLE.md) delegates here.

- **Languages.** `lib/c` is C. Go code (`bin/wasm2o`, `tst/wazero`, later
  `bin/kernel`, `bin/rootfs`) follows the monorepo Go style and carries its
  own `STYLE.md` next to its `go.mod`, as `trustme/bin/cargo` does. C++ for the system's own programs will follow
  STYLE.md as written once it appears.
- **Headers.** Every header except `assert.h` starts with `#pragma once`.
- **Names.** Functions, types and macros of the libc ABI keep their external
  spelling (`__stdio_write`, `struct stat`, `O_CLOEXEC`). Our own helpers and
  locals follow STYLE.md: `lowerCamelCase`, `UpperCamelCase` types.
- **Kernel imports.** A libc function implemented by the kernel is declared
  in its public header with `__KERNEL(name)`; the import name is spelled out
  so the linker can tell an import from a missing definition. A function
  defined in the process has no attribute.
- **Third-party code.** musl and busybox sources are fetched as release
  tarballs and are never edited; what they need from us lives in `lib/c/musl`
  (replacement internal headers) and `lib/c/src`.
- **Formatter.** None yet: `style.py` covers C++ only.
