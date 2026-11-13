#!/usr/bin/env python3
"""The image rootfs serves: a ustar archive of the programs of the system
and the directories it expects.

    image.py <rootfs.tar> --busybox <busybox.wasm> <busybox.links> --python <python.wasm>

The programs are modules, which the kernel compiles as they are executed,
and every one is in /bin: busybox is /bin/busybox, and each name of
busybox.links, wherever busybox would install it, a link to it in /bin;
python is /bin/python3, its standard library in its own file. busybox is
kept compressed by zstd, which the kernel unpacks before it compiles;
python is not, as its standard library is read from its file. /usr is a
link of its bin to /bin, for the scripts that name /usr/bin, and nothing
else. No times, no owners: the same inputs are the same archive.
"""
import io
import os
import sys
import tarfile
from compression import zstd
from pathlib import Path

DIRS = {"bin", "dev", "etc", "tmp", "usr", "var"}


def entry(name, kind, size=0, mode=0o755, target=""):
    info = tarfile.TarInfo(name)
    info.type, info.size, info.mode, info.linkname = kind, size, mode, target
    info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""

    return info


def compressed(module):
    """A module as one zstd frame, of level 19, with its size and a
    checksum: the same module is the same frame."""
    options = {zstd.CompressionParameter.compression_level: 19, zstd.CompressionParameter.checksum_flag: 1}

    return zstd.compress(module, options=options)


def main():
    output = sys.argv[1]
    args = sys.argv[2:]
    files = {"etc/motd": (b"pizdos\n", 0o644), "etc/passwd": (b"root:x:0:0:root:/:/bin/sh\n", 0o644), "etc/group": (b"root:x:0:\n", 0o644)}
    symlinks = {"usr/bin": "/bin"}
    dirs = set(DIRS)

    while args:
        match args:
            case ["--busybox", busybox, links, *rest]:
                files["bin/busybox"] = (compressed(Path(busybox).read_bytes()), 0o755)

                for link in Path(links).read_text().split():
                    symlinks["bin/" + os.path.basename(link)] = "busybox"
            case ["--python", python, *rest]:
                files["bin/python3"] = (Path(python).read_bytes(), 0o755)
                symlinks["bin/python"] = "python3"
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
