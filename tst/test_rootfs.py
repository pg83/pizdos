#!/usr/bin/env python3
"""`vm run -- rootfs.wasm`: a system from one module and nothing of the
host's file system. rootfs serves the image linked into it, the modules of
busybox and python, which the kernel compiles as they are executed, and a
/dev and a /tmp in its memory, and starts busybox's shell on the terminal;
the shell's status is vm's. Each server's files are on a
device of their own and belong to who made them, a directory keeps its
mode, a directory removed while it is read is removed whole, mkdir -p
finds what is there, and rename keeps a directory out of itself and off a
directory that is not empty. Appends from two shells lose no line, flock's
and fcntl's locks are an open file's, a FIFO's readers and writers meet,
a file has hard links, ctime is its own, statvfs tells the capacity of
/tmp and a root that is read-only, and who is not root gets no more than
the modes give.

    test_rootfs.py <vm> <rootfs.wasm>
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = """\
echo hello from $0
for i in 1 2 3; do ls /tmp > /dev/null; done; echo fresh > /tmp/fresh && cat /tmp/fresh && rm /tmp/fresh
cat /etc/motd
ls /bin | grep -x -e ash -e cat -e sh
ls -d /dev/null /dev/tty /dev/zero
echo dropped > /dev/null
head -c 4 /dev/zero | wc -c
readlink /usr/bin && ls /usr && ls -d /sbin /lib 2>&1
printf '#!/usr/bin/env sh\\necho through env\\n' > /tmp/e.sh && chmod +x /tmp/e.sh && /tmp/e.sh && rm /tmp/e.sh
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
python3 -c 'import json, os, sys; print(2 ** 100, sys.version_info[:2], len(os.urandom(5)), json.__file__)'
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
(for i in $(seq 1 200); do echo a$i >> /tmp/log; done) & (for i in $(seq 1 200); do echo b$i >> /tmp/log; done) & wait
sort -u /tmp/log | wc -l
touch /tmp/lk && flock -n /tmp/lk -c 'flock -n /tmp/lk -c true || echo held' && flock -n /tmp/lk -c 'echo free'
python3 -c '
import fcntl, os, sys
f = open("/tmp/rl", "w")
fcntl.lockf(f, fcntl.LOCK_EX, 10, 0)
if os.fork() == 0:
    g = open("/tmp/rl", "a")
    for start in (0, 10):
        try:
            fcntl.lockf(g, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, start); print("taken", start)
        except OSError as e: print("refused", start)
    sys.stdout.flush()
    os._exit(0)
os.wait()
fcntl.lockf(f, fcntl.LOCK_UN, 10, 0)
fcntl.lockf(open("/tmp/rl", "a"), fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 0); print("taken after")
'
mkfifo /tmp/ff && ls -l /tmp/ff | cut -c1
(echo through > /tmp/ff &); cat /tmp/ff
python3 -c '
import os
try:
    os.open("/tmp/ff", os.O_WRONLY | os.O_NONBLOCK)
except OSError as e: print(e.strerror)
r = os.open("/tmp/ff", os.O_RDONLY | os.O_NONBLOCK)
w = os.open("/tmp/ff", os.O_WRONLY); os.write(w, b"1"); os.close(w)
print(os.read(r, 10), os.read(r, 10))
w = os.open("/tmp/ff", os.O_WRONLY); os.write(w, b"2"); os.close(w)
print(os.read(r, 10))
w = os.open("/tmp/ro2", os.O_WRONLY | os.O_CREAT, 0o600)
try: os.read(w, 1)
except OSError as e: print(e.strerror)
'
echo hl > /tmp/h1 && ln /tmp/h1 /tmp/h2 && stat -c %h /tmp/h1 && [ "$(stat -c %i /tmp/h1)" = "$(stat -c %i /tmp/h2)" ] && rm /tmp/h1 && cat /tmp/h2 && stat -c %h /tmp/h2
mkdir /tmp/hd && ln /tmp/hd /tmp/hd2 2>&1; ln /bin/busybox /tmp/bb 2>&1
mkdir -p /tmp/nl/a /tmp/nl/b && stat -c %h /tmp/nl
touch -d @1000000000 /tmp/c && [ "$(stat -c %Z /tmp/c)" != 1000000000 ] && echo changed
python3 -c 'import os; t, r = os.statvfs("/tmp"), os.statvfs("/"); print(t.f_blocks, t.f_bsize, t.f_bfree < t.f_blocks, t.f_ffree < t.f_files, t.f_flag & 1, r.f_flag & 1, r.f_bfree, r.f_blocks > 1000)'
"""

# Who is not root may do no more than the modes say.
PERMISSIONS = """\
echo a > /tmp/ro && chmod 400 /tmp/ro && (echo b >> /tmp/ro) 2>&1; cat /tmp/ro
mkdir /tmp/ns && touch /tmp/ns/f && chmod 600 /tmp/ns && ls /tmp/ns/f 2>&1; chmod 700 /tmp/ns
chmod 777 /tmp 2>&1
python3 -c 'import os; os.chown("/tmp/ro", 0, 0)' 2>&1 | tail -1
printf '#!/bin/sh\\necho ran\\n' > /tmp/s.sh && /tmp/s.sh 2>&1; chmod +x /tmp/s.sh && /tmp/s.sh
"""

PERMITTED = """\
sh: can't create /tmp/ro: Permission denied
a
ls: /tmp/ns/f: Permission denied
chmod: /tmp: Operation not permitted
PermissionError: [Errno 1] Operation not permitted: '/tmp/ro'
sh: /tmp/s.sh: Permission denied
ran
"""

WANT = """\
hello from sh
fresh
pizdos
ash
cat
sh
/dev/null
/dev/tty
/dev/zero
4
/bin
bin
ls: /sbin: No such file or directory
ls: /lib: No such file or directory
through env
busybox
executable
 28 b5 2f fd
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
1267650600228229401496703205376 (3, 14) 5 /bin/python3/json/__init__.py
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
400
held
free
refused 0
taken 10
taken after
p
through
No such device or address
b'1' b''
b'2'
Bad file descriptor
2
hl
1
ln: /tmp/hd2: Operation not permitted
ln: /tmp/bb: Invalid cross-device link
4
changed
262144 4096 True True 0 1 0 True
"""


def main():
    vm, rootfs = sys.argv[1:3]
    # No root, no cwd of the host's: the environment is empty but for what
    # vm needs, and the working directory is one nothing is in.
    script, want = SCRIPT, WANT
    if os.getuid() != 0:
        script, want = script + PERMISSIONS, want + PERMITTED
    with tempfile.TemporaryDirectory(prefix="pizdos-rootfs-") as empty:
        env = {"W2X_MEMORY_TAIL": "strict", "W2X_TAIL_PROFILE": ""}
        result = subprocess.run([vm, "run", "--", rootfs], input=(script + "exit 3\n").encode(), capture_output=True, cwd=empty, env=env,
                                timeout=180)
        assert (result.returncode, result.stdout.decode(), result.stderr) == (3, want, b""), result
        assert not os.listdir(empty), os.listdir(empty)
        result = subprocess.run([vm, "run", "--", rootfs], input=b"ls /missing\n", capture_output=True, cwd=empty, env=env, timeout=120)
        assert result.returncode == 1 and b"No such file" in result.stderr and result.stdout == b"", result
    print("rootfs: busybox's shell runs from the image a server of the kernel's serves, without the host's files")


if __name__ == "__main__":
    main()
