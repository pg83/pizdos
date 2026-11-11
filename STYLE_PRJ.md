# Project style settings

Per-project settings that the shared [STYLE.md](STYLE.md) delegates here.

- **Languages.** `lib/c` is C. Go code (`bin/vm`, `tst/wazero`, later
  `bin/kernel`, `bin/rootfs`) follows the monorepo Go style and carries its
  own `STYLE.md` next to its `go.mod`, as `trustme/bin/cargo` does. C++ for the system's own programs will follow
  STYLE.md as written once it appears.
- **Headers.** Every header except `assert.h` starts with `#pragma once`.
- **Names.** Functions, types and macros of the libc ABI keep their external
  spelling (`__stdio_write`, `struct stat`, `O_CLOEXEC`). Our own helpers and
  locals follow STYLE.md: `lowerCamelCase`, `UpperCamelCase` types.
- **Kernel imports.** A libc function the kernel implements is lib/c's own
  function calling the import, declared `__KERNEL(name)` under the C name
  `__kernel_<name>` (`src/kernel.c`, or `__INLINE` in its header); the
  import name is spelled out so the linker can tell an import from a
  missing definition. No other declaration has the attribute.
- **Third-party code.** musl and busybox sources are fetched as release
  tarballs and are never edited; what they need from us lives in `lib/c/musl`
  (replacement internal headers) and `lib/c/src`.
- **Formatter.** None yet: `style.py` covers C++ only.
