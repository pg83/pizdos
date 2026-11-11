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


def leb(value):
    result = bytearray()
    while value >= 128:
        result.append(value & 127 | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def section(kind, data):
    return bytes([kind]) + leb(len(data)) + data


def wasi_clock(name):
    signature = b"\x60\3\x7f\x7e\x7f\1\x7f" if name == "clock_time_get" else b"\x60\2\x7f\x7f\1\x7f"
    owner, function = b"wasi_snapshot_preview1", name.encode()
    module = b"\0asm\1\0\0\0" + section(1, b"\2" + signature + b"\x60\0\0")
    module += section(2, b"\1" + leb(len(owner)) + owner + leb(len(function)) + function + b"\0\0")
    module += section(3, b"\1\1") + section(5, b"\1\0\1") + section(7, b"\1\6_start\0\1")
    body = b"\0\x41\0" + (b"\x42\0" if name == "clock_time_get" else b"")
    body += b"\x41\x80\10\x10\0\x41\x1d\x47\x04\x40\0\x0b\x0b"
    return module + section(10, b"\1" + leb(len(body)) + body)


def wasi_descriptor(name):
    owner, function = b"wasi_snapshot_preview1", name.encode()
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\2\x7f\x7f\1\x7f\x60\0\0")
    module += section(2, b"\1" + leb(len(owner)) + owner + leb(len(function)) + function + b"\0\0")
    module += section(3, b"\1\1") + section(5, b"\1\0\1") + section(7, b"\1\6_start\0\1")
    body = b"\0\x41\0\x41\x80\10\x10\0\x41\x1d\x47\x04\x40\0\x0b\x0b"
    return module + section(10, b"\1" + leb(len(body)) + body)


def main():
    vm, native, fixture, wrapper = sys.argv[1:]
    # Linux amd64 syscall numbers: uname, sysinfo, getrusage, getgroups;
    # fstatfs, utimensat, syncfs, sync, pwrite64; prlimit64, getrlimit.
    cases = [("system", errno.EIO, (63, 99, 98, 115, 100)), ("filesystem", errno.EIO, (138, 280, 306, 162, 18)),
             ("limits", errno.EIO, (302, 97)), ("network", errno.EIO, (41,)), ("retry", errno.EAGAIN, (41,)),
             ("socket-names", errno.EIO, (51, 52)), ("connect-status", errno.EIO, (55,)),
             ("locks", errno.EIO, ("fcntl",)), ("poll", errno.EIO, (7,)), ("connect-poll", errno.EIO, (7,))]
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
                expected = b"" if operation in ("poll", "connect-poll") else b"host syscall errors: ok\n"
                assert (result.returncode, result.stdout, result.stderr) == (0, expected, b""), (operation, result)
        for name, number in (("clock_time_get", 228), ("clock_res_get", 229)):
            source = work / (name + ".wasm")
            data = wasi_clock(name)
            source.write_bytes(data)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, result
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
            for obj in (compiled, embedded):
                result = subprocess.run([wrapper, f"{errno.EIO}/{number}", "--", vm, "run", "--", str(obj)],
                                        capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (name, result)
        # ENOTTY has no WASI preview1 equivalent and must become generic I/O error.
        for preset, error, names in (("stdin-stat", errno.EIO, ("fd_fdstat_get", "fd_filestat_get")),
                                     ("stdin-flags", errno.ENOTTY, ("fd_fdstat_get", "fd_fdstat_set_flags"))):
            for name in names:
                source = work / (name + ".wasm")
                data = wasi_descriptor(name)
                source.write_bytes(data)
                result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, result
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
                for obj in (compiled, embedded):
                    result = subprocess.run([wrapper, f"{error}/{preset}", "--", vm, "run", "--", str(obj)],
                                            capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (name, preset, result)
    print("host syscalls: kernel and WASI system, file, polling, DNS, socket, clock and limit errors propagate through both AOTs")


if __name__ == "__main__":
    main()
