#!/usr/bin/env python3
"""doom under `vm run`: doomgeneric's frame is ANSI half blocks on a
terminal the kernel owns, its keys the terminal's in raw mode, its clock
the kernel's monotonic time. Headless it plays the shareware demo from the
wad and hashes each second of frames; on a pty it draws truecolor cells,
and Ctrl-C, which the raw terminal no longer turns into a signal, still
ends it with the terminal restored.

    test_doom.py <vm> <doom.o> <doom1.wad>
"""
import os
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

IWAD = "/usr/share/games/doom/doom1.wad"


def make_root(root, obj, wad):
    (root / "bin").mkdir(parents=True)
    shutil.copy(obj, root / "bin" / "doom")
    (root / "bin" / "doom").chmod(0o755)
    (root / "usr" / "share" / "games" / "doom").mkdir(parents=True)
    shutil.copy(wad, root / IWAD.lstrip("/"))
    (root / "tmp").mkdir()


def timedemo(vm, root, work):
    """The shareware demo plays twice, headless: the frames hash the same."""
    hashes = []

    for _ in range(2):
        r = subprocess.run(
            [vm, "run", "--root", str(root), "-a", "doom", str(root / "bin" / "doom"),
             "--", "-iwad", IWAD, "-nosound", "-timedemo", "demo1"],
            capture_output=True, cwd=work, timeout=300,
            env={"PATH": "/bin:/usr/bin", "DG_HEADLESS": "1", "HOME": "/tmp"},
        )
        out = r.stdout.decode(errors="replace")
        err = r.stderr.decode(errors="replace")

        if r.returncode != 255 or "timed" not in err or "fps" not in err:
            return None, f"timedemo: exit {r.returncode}, stdout {out[-500:]!r}, stderr {err[-500:]!r}"

        hashes.append([line for line in out.splitlines() if line.startswith("frame ")])

    if not hashes[0] or hashes[0] != hashes[1]:
        return None, f"the demo's frames differ between runs: {hashes[0][-3:]} vs {hashes[1][-3:]}"

    return hashes[0], None


def terminal(vm, root):
    """On a pty doom takes the alternate screen, draws truecolor cells and
    leaves the terminal as it found it."""
    pid, fd = pty.fork()

    if pid == 0:
        os.chdir(root / "tmp")
        os.execve(vm, [vm, "run", "--root", str(root), "-a", "doom", str(root / "bin" / "doom"),
                           "--", "-iwad", IWAD, "-nosound"],
                  {"PATH": "/bin:/usr/bin", "TERM": "xterm-256color", "HOME": "/tmp",
                   "DG_COLS": "80", "DG_ROWS": "50"})

    out = b""

    def pump(seconds):
        nonlocal out
        end = time.time() + seconds

        while time.time() < end:
            if select.select([fd], [], [], 0.05)[0]:
                try:
                    out += os.read(fd, 65536)
                except OSError:
                    return

    try:
        pump(8)
        os.write(fd, b"\x03")
        pump(3)
    finally:
        os.close(fd)

    _, status = os.waitpid(pid, 0)

    return out, os.waitstatus_to_exitcode(status)


def main():
    vm, obj, wad = sys.argv[1:4]
    failed = []

    with tempfile.TemporaryDirectory(prefix="doom-") as work:
        root = Path(work) / "root"
        make_root(root, obj, Path(wad))

        frames, error = timedemo(vm, root, work)

        if error:
            failed.append(error)
        elif len(frames) < 10:
            failed.append(f"timedemo hashed only {len(frames)} seconds of frames")

        out, code = terminal(vm, root)
        colors = out.count(b"\x1b[38;2;")

        if b"\x1b[?1049h" not in out:
            failed.append(f"the alternate screen is not taken: {out[:120]!r}")
        if colors < 10:
            failed.append(f"too few truecolor cells drawn: {colors}")
        if out.count(b"\xe2\x96\x80") < 1000:
            failed.append("too few half blocks drawn")
        if b"\x1b[?1049l" not in out:
            failed.append("the alternate screen is not given back")
        if code != 0:
            failed.append(f"Ctrl-C: exit {code}, not 0")

    for f in failed:
        print(f)

    assert not failed, f"{len(failed)} of the doom cases failed"
    print(f"{len(frames or [])} seconds of frames, identical runs passed")


if __name__ == "__main__":
    main()
