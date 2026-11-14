#!/usr/bin/env python3
"""Mounts: mount <where> <what> (bin/rootfs/mount.cpp, mount(2)) mounts a
server's directory on a directory of rootfs's: a walk into it, and a call
on its name, go on to that server, which answers the kernel itself. Here
the server is rootfs, a directory of /tmp mounted on another: the names,
the bytes and what is written are the mounted directory's. A mount point
is busy to rmdir and to another mount; umount lets go of it; what is not
there, not a directory, or a device is refused.

    test_mounts.py <vm> <rootfs>
"""
import os
import subprocess
import sys
import tempfile

SCRIPT = """\
mkdir /tmp/where /tmp/what && echo inside > /tmp/what/f && mkdir /tmp/what/d
mount /tmp/where /tmp/what && ls /tmp/where && cat /tmp/where/f && ls -d /tmp/where/d
stat -c %i /tmp/where /tmp/what | uniq | wc -l
echo more > /tmp/where/g && cat /tmp/what/g
find /tmp/where | sort
rmdir /tmp/where
mount /tmp/where /tmp/what
umount /tmp/where && ls /tmp/where | wc -l
umount /tmp/where
mount /tmp/missing /tmp/what
mount /tmp/where /tmp/what/f
mount /tmp/where /dev/null
mount /tmp /tmp/what
"""

WANT = """\
d
f
inside
/tmp/where/d
1
more
/tmp/where
/tmp/where/d
/tmp/where/f
/tmp/where/g
0
"""

ERRORS = """\
rmdir: '/tmp/where': Device or resource busy
mount: /tmp/what on /tmp/where: Device or resource busy
umount: /tmp/where: Invalid argument
mount: /tmp/what on /tmp/missing: No such file or directory
mount: /tmp/what/f on /tmp/where: Not a directory
mount: /dev/null on /tmp/where: Not a directory
mount: /tmp/what on /tmp: Device or resource busy
"""


def main():
    vm, rootfs = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="pizdos-mounts-") as empty:
        env = {"W2X_MEMORY_TAIL": "strict", "W2X_TAIL_PROFILE": ""}
        result = subprocess.run([vm, "run", "--", rootfs], input=(SCRIPT + "exit 3\n").encode(), capture_output=True, cwd=empty,
                                env=env, timeout=180)
        assert (result.returncode, result.stdout.decode(), result.stderr.decode()) == (3, WANT, ERRORS), result
    print("mounts: a server's directory mounted on another answers the walks into it")


if __name__ == "__main__":
    main()
