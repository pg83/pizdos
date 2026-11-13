#!/usr/bin/env python3
"""`vm run -- rootfs.wasm`: a system from one module and nothing of the
host's file system. rootfs serves the image linked into it, the modules of
busybox, python and init, which the kernel compiles as they are executed;
init mounts its /dev and a /tmp in its memory and starts busybox's shell on
the terminal, and the shell's status is vm's. Each server's files are on a
device of their own and belong to who made them, a directory keeps its
mode, a directory removed while it is read is removed whole, mkdir -p
finds what is there, and rename keeps a directory out of itself and off a
directory that is not empty.

    test_rootfs.py <vm> <rootfs.wasm>
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = """\
echo hello from $0
cat /etc/motd
ls /bin | grep -x -e ash -e cat -e sh
ls -d /dev/null /dev/tty /dev/zero
echo dropped > /dev/null
head -c 4 /dev/zero | wc -c
cd /usr/bin && pwd
readlink /bin/sh
test -x /bin/busybox && echo executable
head -c 4 /bin/busybox | od -An -tx1
(echo nothing > /etc/x) 2>/dev/null || echo read-only
echo hi > /tmp/a && echo more >> /tmp/a && cat /tmp/a | wc -l
chmod 600 /tmp/a && stat -c %a /tmp/a && truncate -s 2 /tmp/a && wc -c < /tmp/a
mkdir /tmp/d && mv /tmp/a /tmp/d/b && ls /tmp/d && rm -r /tmp/d && ls /tmp | wc -l
cd /tmp && echo x > y && touch -d @1000000000 y && stat -c %Y y && cd /
sh -c 'exit 5'; echo inner=$?
seq 1 20000 | sort -rn | head -1
head -c 8 /dev/urandom | wc -c
printf ab | od -An -tx1
python3 -c 'import os, sys; print(2 ** 100, sys.version_info[:2], len(os.urandom(5)), os.path.exists("/usr/lib/python314.zip"))'
python3 -c 'import json, os; print(json.dumps(sorted(n for n in os.listdir("/usr/bin") if n.startswith("python"))))'
python3 -c 'import tempfile; f = tempfile.NamedTemporaryFile("w+", dir="/tmp"); f.write("x" * 5000); f.seek(0); print(len(f.read()), f.name.startswith("/tmp/"))'
ln -s target /tmp/sl && readlink /tmp/sl
ls -ld /tmp | cut -c1-10
mkdir -m 700 /tmp/m && ls -ld /tmp/m | cut -c1-10 && chmod 750 /tmp/m && stat -c %a /tmp/m
touch /tmp/o && test "$(stat -c %u /tmp/o)" = "$(id -u)" && echo owned
stat -c %d / /dev /tmp | sort -u | wc -l
mkdir -p /tmp/p/q /usr/bin && ls -d /tmp/p/q
mkdir /usr /tmp 2>&1
mkdir -p /tmp/r/s && mv /tmp/r /tmp/r/s/t 2>&1; ls -d /tmp/r/s
mkdir /tmp/many && for i in $(seq 1 300); do : > /tmp/many/f$i; done && rm -r /tmp/many && ls /tmp/many 2>&1
mkdir /tmp/u && touch /tmp/u/x /tmp/v && python3 -c 'import os; os.rename("/tmp/v", "/tmp/u")' 2>&1 | tail -1; ls /tmp/u
exit 3
"""

WANT = """\
hello from sh
pizdos
ash
cat
sh
/dev/null
/dev/tty
/dev/zero
4
/usr/bin
busybox
executable
 00 61 73 6d
read-only
2
600
2
b
0
1000000000
inner=5
20000
8
 61 62
1267650600228229401496703205376 (3, 14) 5 True
["python", "python3"]
5000 True
target
drwxrwxrwt
drwx------
750
owned
3
/tmp/p/q
mkdir: can't create directory '/usr': File exists
mkdir: can't create directory '/tmp': File exists
mv: can't rename '/tmp/r': Invalid argument
/tmp/r/s
ls: /tmp/many: No such file or directory
IsADirectoryError: [Errno 21] Is a directory: '/tmp/v' -> '/tmp/u'
x
"""


def main():
    vm, rootfs = sys.argv[1:3]
    # No root, no cwd of the host's: the environment is empty but for what
    # vm needs, and the working directory is one nothing is in.
    with tempfile.TemporaryDirectory(prefix="pizdos-rootfs-") as empty:
        env = {"W2X_MEMORY_TAIL": "strict", "W2X_TAIL_PROFILE": ""}
        result = subprocess.run([vm, "run", "--", rootfs], input=SCRIPT.encode(), capture_output=True, cwd=empty, env=env, timeout=120)
        assert (result.returncode, result.stdout.decode(), result.stderr) == (3, WANT, b""), result
        assert not os.listdir(empty), os.listdir(empty)
        result = subprocess.run([vm, "run", "--", rootfs], input=b"ls /missing\n", capture_output=True, cwd=empty, env=env, timeout=120)
        assert result.returncode == 1 and b"No such file" in result.stderr and result.stdout == b"", result
    print("rootfs: busybox's shell runs from the image a server of the kernel's serves, without the host's files")


if __name__ == "__main__":
    main()
