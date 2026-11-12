#!/usr/bin/env python3
"""Build a Nim program for the kernel: Nim emits C, clang compiles it for
wasm32 against lib/c's headers, wasm-ld links it with libc.a.

    nim.py <nim> <clang> <wasm-ld> <libc-include> <libc.a> <main.nim> <out.wasm> [--image FILE] [nim option]...

--image FILE links the file in as data: rootfs_image_base(), its bytes,
and rootfs_image_size, declared by image.h for the program to import.

Nim's runtime is ORC with goto exceptions and panics, no threads and no
signal handler of its own: malloc is lib/c's. Nim's main takes three
arguments, which the wasm target leaves named main; a shim gives lib/c's
crt the __main_argc_argv it calls.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import wasm  # noqa: E402

NIM_FLAGS = [
    "c", "--compileOnly", "--skipUserCfg", "--skipParentCfg", "--noNimblePath",
    "--cpu:wasm32", "--os:linux", "--mm:orc", "--exceptions:goto", "--panics:on", "--threads:off",
    "--stackTrace:off", "--lineTrace:off", "--lineDir:off",
    "-d:useMalloc", "-d:noSignalHandler", "-d:release",
    "--hints:off", "--verbosity:0", "--passC:--target=wasm32-unknown-unknown",
]

LINK = [
    "--stack-first",
    "-z", "stack-size=1049600",
    "--max-memory=4294967296",
    "--export=__heap_base",
    "--export=__stack_pointer",
    "--gc-sections",
]

SHIM = """\
extern char** environ;
int main(int argc, char** argv, char** env);

int __main_argc_argv(int argc, char** argv) {
    return main(argc, argv, environ);
}
"""

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

IMAGE_H = """\
extern const unsigned char rootfs_image[];
extern const unsigned int rootfs_image_size;

static inline const unsigned char* rootfs_image_base(void) {
    return rootfs_image;
}
"""


def nim_lib(nim):
    """The directory of nimbase.h: nim dump lists the library paths after its
    defined symbols."""
    out = subprocess.run([nim, "dump", "--hints:off"], capture_output=True, text=True, cwd="/").stdout + \
        subprocess.run([nim, "dump", "--hints:off"], capture_output=True, text=True, cwd="/").stderr

    for line in out.splitlines():
        candidate = Path(line.strip())

        if (candidate / "nimbase.h").is_file():
            return candidate

    fallback = Path(nim).resolve().parents[1] / "share" / "nim" / "lib"

    if (fallback / "nimbase.h").is_file():
        return fallback

    raise SystemExit("nim.py: cannot find nimbase.h")


def main():
    nim, cc, ld, include, libc, source, output = sys.argv[1:8]
    extra = sys.argv[8:]
    image = None

    if extra[:1] == ["--image"]:
        image, extra = Path(extra[1]).resolve(), extra[2:]

    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    lib = nim_lib(nim)

    with tempfile.TemporaryDirectory(dir=output.parent, prefix="nim-") as work:
        work = Path(work)
        cache = work / "cache"
        subprocess.run([nim, *NIM_FLAGS, *extra, f"--nimcache:{cache}", str(Path(source).resolve())], check=True)
        (work / "shim.c").write_text(SHIM)
        sources = sorted(cache.glob("*.c")) + [work / "shim.c"]
        flags = ["-O2", "-w", "-isystem", include, "-I", str(lib), "-I", str(work)]
        units = [(c, work / (c.stem + ".o"), flags) for c in sources]

        if image is not None:
            (work / "image.h").write_text(IMAGE_H)
            (work / "image.s").write_text(IMAGE_S.format(path=image, size=image.stat().st_size))
            units.append((work / "image.s", work / "image.o", []))

        wasm.compile_all(cc, units)
        subprocess.run([ld, *LINK, "-o", str(output), *(str(o) for _, o, _ in units), libc], check=True)


if __name__ == "__main__":
    main()
