#!/usr/bin/env python3
"""Build a program of the system for the kernel: rootfs, or a service it
starts. clang compiles the source for wasm32 against lib/c's headers, with
no C++ library, and wasm-ld links it with libc.a; rootfs's image is linked
in as data.

    make.py <clang> <wasm-ld> <source.cpp> <libc-include> <libc.a> <output.wasm> [<rootfs.tar>]

The image is the symbols rootfs_image, its bytes, and rootfs_image_size,
which rootfs.cpp declares.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "dev"))

import wasm  # noqa: E402

CXXFLAGS = ["-O2", "-std=c++23", "-fno-exceptions", "-fno-rtti", "-nostdinc++", "-Wall", "-Wextra", "-Werror"]

LINK = ["--stack-first", "-z", "stack-size=1049600", "--max-memory=4294967296", "--export=__heap_base",
        "--export=__stack_pointer", "--gc-sections"]

IMAGE_S = """\
\t.section .rodata.rootfs_image,"",@
\t.globl rootfs_image
\t.p2align 4
rootfs_image:
\t.incbin "{path}"
\t.size rootfs_image, {size}
\t.globl rootfs_image_size
\t.p2align 2
rootfs_image_size:
\t.int32 {size}
\t.size rootfs_image_size, 4
"""


def main():
    if len(sys.argv) not in (7, 8):
        raise SystemExit(__doc__)
    cc, ld, source, include, libc, output = sys.argv[1:7]
    image = Path(sys.argv[7]).resolve() if len(sys.argv) == 8 else None
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(dir=output.parent, prefix=output.stem + "-") as work:
        work = Path(work)
        units = [(Path(source), work / (Path(source).stem + ".o"), [*CXXFLAGS, "-isystem", str(Path(include).resolve())])]
        if image:
            (work / "image.s").write_text(IMAGE_S.format(path=image, size=image.stat().st_size))
            units.append((work / "image.s", work / "image.o", []))
        wasm.compile_all(cc, units)
        subprocess.run([ld, *LINK, "-o", str(output), *(str(o) for _, o, _ in units), libc], check=True)


if __name__ == "__main__":
    main()
