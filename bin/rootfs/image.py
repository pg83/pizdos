#!/usr/bin/env python3
"""The image rootfs serves: a ustar archive of /bin/busybox, the links to
it busybox.links names, /bin/init, and the directories a system expects.

    image.py <busybox.o> <busybox.links> <init.o> <rootfs.tar>

No times, no owners: the same inputs are the same archive.
"""
import io
import os
import sys
import tarfile
from pathlib import Path


def entry(name, kind, size=0, mode=0o755, target=""):
    info = tarfile.TarInfo(name)
    info.type, info.size, info.mode, info.linkname = kind, size, mode, target
    info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""

    return info


def main():
    busybox, links, init, output = sys.argv[1:]
    busybox, init = Path(busybox).read_bytes(), Path(init).read_bytes()
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    files = {"bin/busybox": busybox, "bin/init": init, "etc/motd": b"pizdos\n"}
    symlinks = {}

    for link in Path(links).read_text().split():
        link = link.lstrip("/")
        symlinks[link] = os.path.relpath("bin/busybox", os.path.dirname(link))

    dirs = {"bin", "dev", "etc", "sbin", "tmp", "usr", "usr/bin", "usr/sbin", "var"}

    for path in list(files) + list(symlinks):
        parent = os.path.dirname(path)

        while parent:
            dirs.add(parent)
            parent = os.path.dirname(parent)

    with tarfile.open(output, "w", format=tarfile.USTAR_FORMAT) as archive:
        for name in sorted(dirs):
            archive.addfile(entry(name + "/", tarfile.DIRTYPE))

        for name in sorted(files):
            archive.addfile(entry(name, tarfile.REGTYPE, len(files[name]), 0o755 if name.startswith("bin/") else 0o644), io.BytesIO(files[name]))

        for name in sorted(symlinks):
            archive.addfile(entry(name, tarfile.SYMTYPE, target=symlinks[name]))


if __name__ == "__main__":
    main()
