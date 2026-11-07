#!/usr/bin/env python3
"""busybox under `wasm2o run`: the kernel of lib/c runs its applets inside a
root made like the system's, /bin/busybox the object and every path of
busybox.links a symbolic link to it. The shell's errors, exit and SIGINT
are longjmps, one of them out of a signal handler; its commands, pipes,
substitutions and jobs are forks, which a terminal stops and continues;
xargs, find -exec and awk's pipes are vforks.

    test_busybox.py <wasm2o> <busybox.o> <busybox.wasm> <busybox.links>
"""
import gzip
import hashlib
import os
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DATA = "banana\napple\ncherry\napple\n10\n9\n"

# argv, standard input, expected standard output, expected exit code
CASES = [
    (["echo", "hello", "world"], "", "hello world\n", 0),
    (["sort"], DATA, "10\n9\napple\napple\nbanana\ncherry\n", 0),
    # lines that are not numbers are all equal as numbers
    (["sort", "-n", "-u"], DATA, "apple\n9\n10\n", 0),
    (["uniq", "-c"], "a\na\nb\n", "      2 a\n      1 b\n", 0),
    (["grep", "-n", "apple"], DATA, "2:apple\n4:apple\n", 0),
    (["grep", "-E", "^(ch|ba)"], DATA, "banana\ncherry\n", 0),
    (["grep", "-c", "kiwi"], DATA, "0\n", 1),
    (["sed", "-e", "s/a\\(p*\\)/[\\1]/g", "-e", "3d"], DATA, "b[]n[]n[]\n[pp]le\n[pp]le\n10\n9\n", 0),
    (["awk", "{n += length($0)} END {printf \"%d %d %.2f\\n\", NR, n, n / NR}"], DATA, "6 25 4.17\n", 0),
    (["tr", "a-z", "A-Z"], "abc\n", "ABC\n", 0),
    (["cut", "-d", "a", "-f", "2"], "xay\n", "y\n", 0),
    (["wc", "-l"], DATA, "6\n", 0),
    (["md5sum"], DATA, hashlib.md5(DATA.encode()).hexdigest() + "  -\n", 0),
    (["sha256sum"], DATA, hashlib.sha256(DATA.encode()).hexdigest() + "  -\n", 0),
    (["seq", "2", "4"], "", "2\n3\n4\n", 0),
    (["expr", "3", "*", "4", "+", "1"], "", "13\n", 0),
    (["printf", "%s-%03d\\n", "a", "7"], "", "a-007\n", 0),
    (["date", "-u", "-d", "@1700000000", "+%F %T %z %j %a"], "", "2023-11-14 22:13:20 +0000 318 Tue\n", 0),
    (["date", "-d", "@0", "+%H"], "", "03\n", 0, {"TZ": "Europe/Moscow"}),
    (["basename", "/a/b/c.txt", ".txt"], "", "c\n", 0),
    (["dirname", "/a/b/c.txt"], "", "/a/b\n", 0),
    (["false"], "", "", 1),
    # exec inside the root: env runs /usr/bin/env, a link to the object
    (["env", "-i", "A=1", "env"], "", "A=1\n", 0),
    # a #! script runs its interpreter
    (["env", "/tmp/script.sh", "x"], "", "from /tmp/script.sh x\n", 0),
    # a symbolic link to / is the root's
    (["ls", "/tmp/up/bin/busybox"], "", "/tmp/up/bin/busybox\n", 0),
    (["readlink", "-f", "/tmp/up/.."], "", "/\n", 0),
    # the shell raises its errors and exit by longjmp
    (["sh", "-c", "echo ${u?unset}; echo no"], "", "", 2),
    (["sh", "-c", "echo $((1 / 0)); echo no"], "", "", 2),
    (["sh", "-c", "cd /missing || echo failed; echo next"], "", "failed\nnext\n", 0),
    (["sh", "-c", "set -e; false; echo no"], "", "", 1),
    (["sh", "-c", "f() { echo in $1; exit 7; }; f a; echo no"], "", "in a\n", 7),
    (["sh", "-c", "for i in 1 2 3; do [ $i = 2 ] && continue; echo $i; done"], "", "1\n3\n", 0),
    (["sh", "-s"], "x=4\necho $((x * x))\nexit 3\n", "16\n", 3),
    (["env", "/tmp/shell.sh", "a", "b"], "", "2 a b\n", 2),
    # fork: commands, pipes, substitutions, subshells, jobs
    (["sh", "-c", "echo $(echo sub) $(echo $(echo deep))"], "", "sub deep\n", 0),
    (["sh", "-c", "x=$(seq 3 | wc -l); echo x=$x"], "", "x=3\n", 0),
    (["sh", "-c", "echo a b c | tr a-z A-Z | sed s/B/-/"], "", "A - C\n", 0),
    (["sh", "-c", "seq 1 20000 | sort -rn | head -2; seq 1 100 | md5sum"], "",
     "20000\n19999\n" + hashlib.md5("".join(f"{i}\n" for i in range(1, 101)).encode()).hexdigest() + "  -\n", 0),
    (["sh", "-c", "(cd /bin && pwd); pwd"], "", "/bin\n/tmp\n", 0),
    (["sh", "-c", "f() { echo $1 | sed s/a/b/; }; for i in a aa; do f $i; done | sort"], "", "b\nba\n", 0),
    (["sh", "-c", "sh -c 'exit 4'; echo $?; (exit 3); echo $?"], "", "4\n3\n", 0),
    (["sh", "-c", "cat /tmp/script.sh > /tmp/bg.out & wait $!; echo rc=$?; cat /tmp/bg.out"], "", "rc=0\n#!/bin/echo from\n", 0),
    (["sh", "-c", "sleep 2 & p=$!; kill -STOP $p; kill -TERM $p; kill -CONT $p; wait $p; echo rc=$?"], "", "rc=143\n", 0),
    (["sh", "-c", "trap 'echo chld' CHLD; true & wait; echo done"], "", "chld\ndone\n", 0),
    # vfork
    (["xargs", "-n1", "echo", "one"], "a b\n", "one a\none b\n", 0),
    (["xargs", "missing"], "x\n", "", 127),
    (["find", "/tmp", "-maxdepth", "1", "-name", "shell.sh", "-exec", "echo", "found", "{}", ";"], "", "found /tmp/shell.sh\n", 0),
    (["awk", "{ print $0 | \"sort -r\" }"], "a\nc\nb\n", "c\nb\na\n", 0),
    (["awk", "BEGIN { while ((\"echo x; echo y\" | getline l) > 0) print \"l=\" l }"], "", "l=x\nl=y\n", 0),
]

# An interactive shell gets SIGINT while it reads a line, and while a loop
# of builtins blocks writing to a full pipe: there the handler longjmps out
# of the write. Each time it takes the next line after a new prompt.
INTERRUPTS = [
    ("", "$ ^C\n$ after\n"),
    ("while :; do echo 0123456789; done", "0123456789\n$ after\n"),
]


def make_root(root, obj, links):
    (root / "bin").mkdir(parents=True)
    shutil.copy(obj, root / "bin" / "busybox")
    (root / "bin" / "busybox").chmod(0o755)

    for link in links.split():
        path = root / link.lstrip("/")

        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to("/bin/busybox")

    tmp = root / "tmp"
    tmp.mkdir()
    script = tmp / "script.sh"
    script.write_text("#!/bin/echo from\n")
    script.chmod(0o755)
    shell = tmp / "shell.sh"
    shell.write_text('#!/bin/sh\necho $# "$@"\n${missing?gone}\necho no\n')
    shell.chmod(0o755)
    (tmp / "up").symlink_to("/")


def run(wasm2o, root, argv, stdin, env=None):
    command = [wasm2o, "run", "--root", str(root), "-a", argv[0], str(root / "bin" / "busybox"), "--", *argv[1:]]
    environment = {"PATH": "/bin:/usr/bin", "TZ": "UTC", **(env or {})}

    return subprocess.run(command, input=stdin.encode(), capture_output=True, cwd=root / "tmp", env=environment, timeout=60)


def interrupt(wasm2o, root, first):
    command = [wasm2o, "run", "--root", str(root), "-a", "sh", str(root / "bin" / "busybox"), "--", "-i"]
    p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=root / "tmp",
                         env={"PATH": "/bin:/usr/bin", "PS1": "$ "})

    if first:
        p.stdin.write(first.encode() + b"\n")
        p.stdin.flush()

    time.sleep(0.5)
    p.send_signal(signal.SIGINT)
    time.sleep(0.3)
    p.stdin.write(b"echo after; exit 5\n")
    p.stdin.flush()

    try:
        out, _ = p.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()

    return out.decode(errors="replace"), p.returncode


def job_control(wasm2o, root):
    """Ctrl-Z stops a command on a terminal, jobs lists it, fg continues it
    and Ctrl-C ends it."""
    pid, fd = pty.fork()

    if pid == 0:
        os.chdir(root / "tmp")
        os.execve(wasm2o, [wasm2o, "run", "--root", str(root), "-a", "sh", str(root / "bin" / "busybox"), "--", "-i"],
                  {"PATH": "/bin:/usr/bin", "PS1": "$ ", "TERM": "dumb"})

    out = b""

    def pump(seconds):
        nonlocal out
        end = time.time() + seconds

        while time.time() < end:
            if select.select([fd], [], [], 0.05)[0]:
                try:
                    out += os.read(fd, 4096)
                except OSError:
                    return

    pump(0.8)

    for keys in [b"cat\r", b"\x1a", b"jobs\r", b"fg\r", b"\x03", b"echo after $?\r", b"exit 6\r"]:
        os.write(fd, keys)
        pump(0.6)

    _, status = os.waitpid(pid, 0)
    os.close(fd)

    return out.decode(errors="replace").replace("\r", ""), os.waitstatus_to_exitcode(status)


def main():
    wasm2o, obj, _, links = sys.argv[1:5]
    failed = []

    with tempfile.TemporaryDirectory(prefix="busybox-") as work:
        root = Path(work) / "root"
        make_root(root, obj, Path(links).read_text())

        for case in CASES:
            argv, stdin, want, code = case[:4]
            env = case[4] if len(case) > 4 else None
            r = run(wasm2o, root, argv, stdin, env)
            got = r.stdout.decode(errors="replace")

            if got != want or r.returncode != code:
                failed.append(f"{argv}: exit {r.returncode}, stdout {got!r}, stderr {r.stderr.decode(errors='replace')!r}; want exit {code}, stdout {want!r}")

        # files: a tree made, archived, compressed and read back
        steps = [
            ["mkdir", "-p", "/tmp/d/e"],
            ["cp", "/tmp/script.sh", "/tmp/d/e/copy"],
            ["ln", "-s", "e/copy", "/tmp/d/link"],
            ["tar", "cf", "/tmp/d.tar", "-C", "/tmp", "d"],
            ["gzip", "-k", "/tmp/d.tar"],
            ["mv", "/tmp/d", "/tmp/old"],
            ["tar", "xf", "/tmp/d.tar", "-C", "/tmp"],
        ]

        for argv in steps:
            r = run(wasm2o, root, argv, "")

            if r.returncode != 0:
                failed.append(f"{argv}: exit {r.returncode}, stderr {r.stderr.decode(errors='replace')!r}")

        tmp = root / "tmp"

        if gzip.decompress((tmp / "d.tar.gz").read_bytes()) != (tmp / "d.tar").read_bytes():
            failed.append("gzip -k: the .gz does not decompress to the tar")

        if os.readlink(tmp / "d" / "link") != "e/copy" or (tmp / "d" / "e" / "copy").read_text() != "#!/bin/echo from\n":
            failed.append("tar xf: the tree differs")

        r = run(wasm2o, root, ["find", "/tmp/d", "-type", "f"], "")

        if r.stdout.decode() != "/tmp/d/e/copy\n":
            failed.append(f"find: {r.stdout!r}")

        r = run(wasm2o, root, ["cat", "/tmp/missing"], "")

        if r.returncode != 1 or b"No such file or directory" not in r.stderr:
            failed.append(f"cat of a missing file: exit {r.returncode}, stderr {r.stderr!r}")

        for first, tail in INTERRUPTS:
            out, code = interrupt(wasm2o, root, first)

            if not out.endswith(tail) or code != 5:
                failed.append(f"SIGINT to sh -i after {first!r}: exit {code}, stdout ends {out[-60:]!r}; want exit 5, {tail!r}")

        out, code = job_control(wasm2o, root)

        if out.count("Stopped") != 2 or "after 130" not in out or code != 6:
            failed.append(f"job control: exit {code}, terminal {out!r}")

    for f in failed:
        print(f)

    assert not failed, f"{len(failed)} of the busybox cases failed"
    print(f"{len(CASES) + len(steps) + 4 + len(INTERRUPTS)} cases passed")


if __name__ == "__main__":
    main()
