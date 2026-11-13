#!/usr/bin/env python3
"""CPython under `vm run`: python.wasm is /usr/bin/python3 of a root whose
standard library is /usr/lib/python314.zip and whose shell is busybox's.
A script prints what the interpreter computes and what it gets from the
kernel: processes, pipes, a pseudoterminal, signals, timers, locks,
mappings written back, the user database, locales and the tokenizer's
positions. Then a set of CPython's own tests, from the release tarball's
Lib/test, runs with its regrtest.

    test_python.py <vm> <python.wasm> <python314.zip> <Python.tgz> <busybox.wasm> <busybox.links>
"""
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

# CPython's tests that need nothing the build leaves out: no sockets, no
# threads, no test modules of the C API.
TESTS = [
    "test_bisect", "test_bool", "test_dict", "test_errno", "test_exceptions", "test_float", "test_fstring",
    "test_glob", "test_grammar", "test_grp", "test_list", "test_long", "test_pwd", "test_re", "test_resource",
    "test_scope", "test_select", "test_set", "test_str", "test_string", "test_struct", "test_time", "test_tuple",
]

SCRIPT = r'''
import errno, fcntl, hashlib, io, json, locale, math, mmap, os, pty, pwd, random, re, resource, select, signal
import struct, subprocess, sys, time, tokenize

print(sys.version.split()[0], sys.platform, len(sys.builtin_module_names))
print(hashlib.sha256(b"abc").hexdigest()[:16], random.Random("seed").random())
print((2**200 + 1) // 7 % 10**9, repr(1 / 3), format(2.5e-5, ".17g"), struct.pack("<dq", 1.1, -5).hex())
print(json.dumps({"x": [1.5, 1e300, -0.0]}), re.sub(r"(\w+)", r"<\1>", "hi there"), sorted({"b": 1, "a": 2}))

# the tokenizer's positions, which a misordered spill once lost
print([t.start for t in tokenize.generate_tokens(io.StringIO("a b\n").readline)])
try:
    compile("1 +\n", "<s>", "exec")
except SyntaxError as e:
    print("syntax", e.lineno, e.offset, repr(e.text))

# processes: fork and wait, the shell, exec of python itself
pid = os.fork()
if pid == 0:
    os._exit(7)
print("fork", os.waitpid(pid, 0)[1] >> 8)
print("sh", subprocess.run(["sh", "-c", "echo $((6 * 7))"], capture_output=True, text=True).stdout.strip())
print("python", subprocess.run([sys.executable, "-c", "print(2 ** 10)"], capture_output=True, text=True).stdout.strip())
pid = os.fork()
if pid == 0:
    os._exit(3)
info = os.waitid(os.P_PID, pid, os.WEXITED)
print("waitid", info.si_status, info.si_code == os.CLD_EXITED)

# pipes and select
rd, wr = os.pipe()
print("select", select.select([rd], [], [], 0)[0], end=" ")
os.write(wr, b"x")
print(select.select([rd], [], [], 1)[0] == [rd], os.read(rd, 1))

# a pseudoterminal: forkpty's child writes to its terminal
pid, master = pty.fork()
if pid == 0:
    os.write(1, b"from the child\n")
    os._exit(0)
out = b""
while True:
    try:
        chunk = os.read(master, 100)
    except OSError:
        break
    if not chunk:
        break
    out += chunk
os.waitpid(pid, 0)
print("pty", out.replace(b"\r", b"").decode().strip(), os.isatty(master))

# signals and timers
got = []
signal.signal(signal.SIGALRM, lambda s, f: got.append(s))
signal.setitimer(signal.ITIMER_REAL, 0.05)
time.sleep(0.5)
print("alarm", got == [signal.SIGALRM])
signal.signal(signal.SIGUSR1, lambda s, f: got.append(s))
os.kill(os.getpid(), signal.SIGUSR1)
print("kill", got[-1] == signal.SIGUSR1)
signal.pthread_sigmask(signal.SIG_BLOCK, [signal.SIGUSR2])
os.kill(os.getpid(), signal.SIGUSR2)
print("sigwait", signal.sigwait([signal.SIGUSR2]) == signal.SIGUSR2)

# files: locks, a shared mapping written back, times
path = "/tmp/data"
with open(path, "wb") as f:
    f.write(b"hello world")
fd = os.open(path, os.O_RDWR)
fcntl.flock(fd, fcntl.LOCK_EX)
fcntl.lockf(fd, fcntl.LOCK_EX)
m = mmap.mmap(fd, 0)
m[0:5] = b"HELLO"
m.flush()
print("mmap", os.pread(fd, 11, 0))
m.close()
os.utime(path, (1000000000, 1000000000))
print("utime", os.stat(path).st_mtime, os.path.getsize(path))
os.close(fd)

# the system: limits, users, scheduling, locale, clocks
print("rlimit", resource.getrlimit(resource.RLIMIT_NOFILE)[0] > 0, pwd.getpwuid(os.getuid()).pw_name)
print("sched", os.sched_getscheduler(0), os.getpriority(os.PRIO_PROCESS, 0))
print("locale", locale.setlocale(locale.LC_ALL, ""), locale.getpreferredencoding())
t = time.process_time()
sum(range(100000))
print("clock", time.process_time() > t, time.monotonic() > 0)
'''

EXPECTED = '''3.14.10 pizdos 68
ba7816bf8f01cfea 0.1382359792209653
547900196 0.3333333333333333 2.5000000000000001e-05 9a9999999999f13ffbffffffffffffff
{"x": [1.5, 1e+300, -0.0]} <hi> <there> ['a', 'b']
[(1, 0), (1, 2), (1, 3), (2, 0)]
syntax 1 4 '1 +\\n'
fork 7
sh 42
python 1024
waitid 3 True
select [] True b'x'
pty from the child True
alarm True
kill True
sigwait True
mmap b'HELLO world'
utime 1000000000.0 11
rlimit True user
sched 0 0
locale C.UTF-8;C;C;C;C;C UTF-8
clock True True
'''


def make_root(root, python, zipped, tarball, busybox, links):
    (root / "bin").mkdir(parents=True)
    shutil.copy(busybox, root / "bin" / "busybox")
    (root / "bin" / "busybox").chmod(0o755)

    for link in links.split():
        path = root / link.lstrip("/")

        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to("/bin/busybox")

    lib = root / "usr" / "lib"
    (lib / "python3.14" / "lib-dynload").mkdir(parents=True)
    (root / "usr" / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy(python, root / "usr" / "bin" / "python3")
    (root / "usr" / "bin" / "python3").chmod(0o755)
    shutil.copy(zipped, lib / "python314.zip")

    with tarfile.open(tarball) as archive:
        members = [m for m in archive.getmembers() if "/Lib/test/" in m.name]
        archive.extractall(root / "src", members=members, filter="data")

    shutil.move(next((root / "src").glob("Python-*")) / "Lib" / "test", lib / "python3.14" / "test")
    shutil.rmtree(root / "src")
    (root / "etc").mkdir()
    (root / "etc" / "passwd").write_text(f"root:x:0:0:root:/root:/bin/sh\nuser:x:{os.getuid()}:{os.getgid()}:User:/tmp:/bin/sh\n")
    (root / "etc" / "group").write_text(f"root:x:0:\nuser:x:{os.getgid()}:user\n")
    (root / "tmp").mkdir()


def python(vm, root, args, timeout):
    command = [vm, "run", "--root", str(root), "-a", "/usr/bin/python3", "--", str(root / "usr" / "bin" / "python3"), *args]
    environment = {"PATH": "/usr/bin:/bin", "HOME": "/tmp", "TZ": "UTC"}

    return subprocess.run(command, capture_output=True, text=True, cwd=root / "tmp", env=environment, timeout=timeout)


def main():
    vm, interpreter, zipped, tarball, busybox, links = sys.argv[1:7]
    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "root"
        make_root(root, interpreter, zipped, tarball, busybox, Path(links).read_text())
        (root / "tmp" / "check.py").write_text(SCRIPT)
        r = python(vm, root, ["/tmp/check.py"], 300)

        if r.returncode != 0 or r.stdout != EXPECTED:
            failures.append(f"check.py: exit {r.returncode}\n--- stdout\n{r.stdout}\n--- expected\n{EXPECTED}\n--- stderr\n{r.stderr[-3000:]}")

        r = python(vm, root, ["-m", "test", *TESTS], 1800)

        if r.returncode != 0 or "Result: SUCCESS" not in r.stdout:
            failures.append(f"regrtest: exit {r.returncode}\n{r.stdout[-5000:]}\n{r.stderr[-2000:]}")

    for f in failures:
        print(f"FAIL {f}")

    if failures:
        sys.exit(1)

    print(f"python: the check script and {len(TESTS)} of CPython's tests pass")


if __name__ == "__main__":
    main()
