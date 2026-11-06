#!/usr/bin/env python3
"""Build busybox.wasm from a release tarball, our config and lib/c.

    make.py <cc> <ar> <wasm-ld> <busybox-tarball> <config> <libc-include> <libc.a> <busybox.wasm> <busybox.links>

The tree is configured by gen.py, every selected object is compiled for
wasm32, the lib-y objects go into an archive the way busybox's own lib.a
files do, and the result is linked against libc.a. An undefined symbol is a
link error: every libc function is either defined in libc.a or declared as a
kernel import by its header.
"""
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / "dev"))

import gen  # noqa: E402
import wasm  # noqa: E402

FLAGS = [
    "-Os",
    "-std=gnu99",
    "-funsigned-char",
    "-Wno-string-plus-int",
    "-Wno-format-security",
    "-include", "include/autoconf.h",
    "-I", "include",
    "-I", "libbb",
    "-D_GNU_SOURCE",
    "-DNDEBUG",
    "-D_LARGEFILE_SOURCE",
    "-D_LARGEFILE64_SOURCE",
    "-D_FILE_OFFSET_BITS=64",
]

LINK = [
    "--stack-first",
    "-z", "stack-size=1048576",
    "--max-memory=4294967296",
    "--export=__heap_base",
    "--gc-sections",
    "--error-limit=0",
]


def version(tree):
    values = {}

    for line in (tree / "Makefile").read_text().splitlines()[:8]:
        if "=" in line:
            name, value = line.split("=", 1)
            values[name.strip()] = value.strip()

    return f"{values['VERSION']}.{values['PATCHLEVEL']}.{values['SUBLEVEL']}{values.get('EXTRAVERSION', '')}"


def source_of(tree, obj):
    for suffix in (".c", ".S"):
        source = tree / obj.with_suffix(suffix)

        if source.exists():
            return source

    raise SystemExit(f"no source for {obj}")


def main():
    cc, ar, ld, tarball, config, libc_include, libc, output, links = sys.argv[1:10]
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "busybox"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    with tarfile.open(tarball) as archive:
        archive.extractall(work, filter="data")

    tree = next(work.glob("busybox-*"))
    objects_list = work / "objects.txt"
    os.environ.setdefault("CC", cc)
    Path(links).parent.mkdir(parents=True, exist_ok=True)
    sys.argv = ["gen.py", str(tree), config, str(objects_list), links]
    gen.main()

    flags = FLAGS + ["-isystem", str(Path(libc_include).resolve()), f'-DBB_VER="{version(tree)}"']
    units = []
    archived = []
    linked = []

    for line in objects_list.read_text().splitlines():
        kind, name = line.split()
        obj = work / "obj" / name
        units.append((source_of(tree, Path(name)), obj, flags))
        (archived if kind == "lib" else linked).append(obj)

    wasm.compile_all(cc, units, cwd=tree)
    archive = work / "libbusybox.a"
    wasm.archive(ar, archive, archived)
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([ld, *LINK, "-o", output, *map(str, linked), str(archive), libc], check=True)


if __name__ == "__main__":
    main()
