#!/usr/bin/env python3
"""Build rootfs for the kernel: clang compiles rootfs.cpp for wasm32 against
lib/c's headers, with no C++ library, the image is linked in as data, and
wasm-ld links them with libc.a.

    make.py <clang> <wasm-ld> <rootfs.cpp> <libc-include> <libc.a> <rootfs.tar> <rootfs.wasm>

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
    cc, ld, source, include, libc, image, output = sys.argv[1:8]
    image, output = Path(image).resolve(), Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(dir=output.parent, prefix="rootfs-") as work:
        work = Path(work)
        (work / "image.s").write_text(IMAGE_S.format(path=image, size=image.stat().st_size))
        units = [(Path(source), work / "rootfs.o", [*CXXFLAGS, "-isystem", str(Path(include).resolve())]),
                 (work / "image.s", work / "image.o", [])]
        wasm.compile_all(cc, units)
        subprocess.run([ld, *LINK, "-o", str(output), *(str(o) for _, o, _ in units), libc], check=True)


if __name__ == "__main__":
    main()
