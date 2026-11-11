#!/usr/bin/env python3
"""Propagate real Linux syscall failures through guest libc calls.

    test_host_syscalls.py <vm> <native-aot> <fixture.wasm> <filter>

A seccomp filter restricts only each child VM process. It returns selected error numbers for
selected syscalls, leaving the parent and later commands unaffected.
"""
import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    vm, native, fixture, wrapper = sys.argv[1:]
    # Linux amd64 syscall numbers: uname, sysinfo, getrusage, getgroups;
    # fstatfs, utimensat, syncfs, sync, pwrite64; prlimit64, getrlimit.
    cases = [("system", errno.EIO, (63, 99, 98, 115)), ("filesystem", errno.EIO, (138, 280, 306, 162, 18)),
             ("limits", errno.EIO, (302, 97)), ("network", errno.EIO, (41,)), ("retry", errno.EAGAIN, (41,)),
             ("socket-names", errno.EIO, (51, 52)), ("connect-status", errno.EIO, (55,))]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-host-syscalls-") as directory:
        work = Path(directory)
        (work / "tmp").mkdir()
        compiled, embedded = work / "native.o", work / "embedded.o"
        result = subprocess.run([native, "--strict"], input=Path(fixture).read_bytes(), capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result
        compiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(embedded), fixture], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
        for obj in (compiled, embedded):
            for operation, error, numbers in cases:
                result = subprocess.run([wrapper, str(error) + "/" + ",".join(map(str, numbers)), "--", vm, "run", "--root", str(work), "--", str(obj), operation], capture_output=True,
                                        env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"host syscall errors: ok\n", b""), (operation, result)
    print("host syscalls: system information, files, DNS, sockets and resource-limit failures propagate through both AOTs")


if __name__ == "__main__":
    main()
