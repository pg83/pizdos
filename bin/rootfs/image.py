#!/usr/bin/env python3
"""The image rootfs serves: a ustar archive of the programs of the system
and the directories it expects.

    image.py <rootfs.tar> --busybox <busybox.wasm> <busybox.links> --init <init.wasm> --python <python.wasm> <python314.zip>

The programs are modules, which the kernel compiles as they are executed:
busybox is /bin/busybox and every path of busybox.links a link to it; init
is /bin/init; python is /usr/bin/python3 with its standard library at
/usr/lib/python314.zip, where it looks. No times, no owners: the same
inputs are the same archive.
"""
import io
import os
import sys
import tarfile
from pathlib import Path

DIRS = {"bin", "dev", "etc", "sbin", "tmp", "usr", "usr/bin", "usr/lib", "usr/sbin", "var"}


def entry(name, kind, size=0, mode=0o755, target=""):
    info = tarfile.TarInfo(name)
    info.type, info.size, info.mode, info.linkname = kind, size, mode, target
    info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""

    return info


def main():
    output = sys.argv[1]
    args = sys.argv[2:]
    files = {"etc/motd": (b"pizdos\n", 0o644), "etc/passwd": (b"root:x:0:0:root:/:/bin/sh\n", 0o644), "etc/group": (b"root:x:0:\n", 0o644)}
    symlinks = {}
    dirs = set(DIRS)

    while args:
        match args:
            case ["--busybox", busybox, links, *rest]:
                files["bin/busybox"] = (Path(busybox).read_bytes(), 0o755)

                for link in Path(links).read_text().split():
                    link = link.lstrip("/")
                    symlinks[link] = os.path.relpath("bin/busybox", os.path.dirname(link))
            case ["--init", init, *rest]:
                files["bin/init"] = (Path(init).read_bytes(), 0o755)
            case ["--python", python, zipped, *rest]:
                files["usr/bin/python3"] = (Path(python).read_bytes(), 0o755)
                files["usr/lib/python314.zip"] = (Path(zipped).read_bytes(), 0o644)
                symlinks["usr/bin/python"] = "python3"
                dirs.add("usr/lib/python3.14/lib-dynload")
            case _:
                raise SystemExit(f"image.py: unknown arguments {args}")

        args = rest

    for path in list(files) + list(symlinks):
        parent = os.path.dirname(path)

        while parent:
            dirs.add(parent)
            parent = os.path.dirname(parent)

    Path(output).parent.mkdir(parents=True, exist_ok=True)

    with tarfile.open(output, "w", format=tarfile.USTAR_FORMAT) as archive:
        for name in sorted(dirs):
            archive.addfile(entry(name + "/", tarfile.DIRTYPE))

        for name in sorted(files):
            data, mode = files[name]
            archive.addfile(entry(name, tarfile.REGTYPE, len(data), mode), io.BytesIO(data))

        for name in sorted(symlinks):
            archive.addfile(entry(name, tarfile.SYMTYPE, target=symlinks[name]))


if __name__ == "__main__":
    main()
