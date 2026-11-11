#!/usr/bin/env python3
"""Propagate real Linux syscall failures through guest libc calls.

    test_host_syscalls.py <vm> <native-aot> <fixture.wasm> <filter>

A seccomp filter restricts only each child VM process. It returns selected error numbers for
selected syscalls, leaving the parent and later commands unaffected.
"""
import errno
import contextlib
import os
from pathlib import Path
import resource
import socket
import stat
import subprocess
import sys
import tempfile


def leb(value, signed=False):
    result = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(result + bytes([byte]))
        result.append(byte | 128)


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


def wasi_descriptor(name, error=29, filetype=None):
    owner, function = b"wasi_snapshot_preview1", name.encode()
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\2\x7f\x7f\1\x7f\x60\0\0")
    module += section(2, b"\1" + leb(len(owner)) + owner + leb(len(function)) + function + b"\0\0")
    module += section(3, b"\1\1") + section(5, b"\1\0\1") + section(7, b"\1\6_start\0\1")
    body = b"\0\x41\0\x41\x80\10\x10\0\x41" + leb(error) + b"\x47\x04\x40\0\x0b"
    if filetype is not None:
        offset = 0 if name == "fd_fdstat_get" else 16
        body += b"\x41\x80\10\x2d\0" + leb(offset) + b"\x41" + leb(filetype) + b"\x47\x04\x40\0\x0b"
    body += b"\x0b"
    return module + section(10, b"\1" + leb(len(body)) + body)


def wasi_directory(path, name, filetype):
    owner, function = b"wasi_snapshot_preview1", b"fd_readdir"
    module = b"\0asm\1\0\0\0" + section(1, b"\3\x60\5\x7f\x7f\x7f\x7e\x7f\1\x7f"
                                              b"\x60\11\x7f\x7f\x7f\x7f\x7f\x7e\x7e\x7f\x7f\1\x7f\x60\0\0")
    module += section(2, b"\2" + leb(len(owner)) + owner + leb(len(function)) + function + b"\0\0"
                      + leb(len(owner)) + owner + b"\11path_open\0\1")
    module += section(3, b"\1\2") + section(5, b"\1\0\1") + section(7, b"\1\6_start\0\2")
    integer = lambda n: b"\x41" + leb(n, True)
    path = str(path).encode()
    code = b"".join(integer(n) for n in (3, 0, 1024, len(path), 2)) + b"\x42" + leb(1 << 14, True) + b"\x42\0"
    code += integer(0) + integer(3072) + b"\x10\1\x04\x40\0\x0b" + integer(3072) + b"\x28\2\0\x21\3"
    code += b"\x20\3" + integer(4096) + integer(60000) + b"\x42\0" + integer(2048) + b"\x10\0\x04\x40\0\x0b"
    code += integer(4096) + b"\x21\0" + integer(4096) + integer(2048) + b"\x28\2\0\x6a\x21\1"
    code += b"\x02\x40\x03\x40\x20\0\x20\1\x4f\x0d\1"
    code += b"\x20\0\x28\2\x10" + integer(4) + b"\x46\x04\x40\x20\0\x28\2\x18"
    code += integer(int.from_bytes(name, "little")) + b"\x46\x04\x40\x20\0\x2d\0\x14" + integer(filetype)
    code += b"\x47\x04\x40\0\x0b" + integer(1) + b"\x21\2\x0b\x0b"
    code += b"\x20\0\x20\0\x28\2\x10" + integer(24) + b"\x6a\x6a\x21\0\x0c\0\x0b\x0b"
    code += b"\x20\2\x45\x04\x40\0\x0b\x0b"
    body = b"\1\4\x7f" + code
    return module + section(10, b"\1" + leb(len(body)) + body) + section(11, b"\1\0" + integer(1024) + b"\x0b" + leb(len(path)) + path)


def main():
    vm, native, fixture, wrapper = sys.argv[1:]
    # Linux amd64 syscall numbers: uname, sysinfo, getrusage, getgroups;
    # fstatfs, utimensat, syncfs, sync, pwrite64; prlimit64, getrlimit.
    cases = [("system", errno.EIO, (63, 99, 98, 115, 100)), ("filesystem", errno.EIO, (138, 280, 306, 162, 18)),
             ("limits", errno.EIO, (302, 97)), ("network", errno.EIO, (41,)), ("retry", errno.EAGAIN, (41,)),
             ("socket-names", errno.EIO, (51, 52)), ("connect-status", errno.EIO, (55,)),
             ("locks", errno.EIO, ("fcntl",)), ("poll", errno.EIO, (7,)), ("connect-poll", errno.EIO, (7,)),
             ("descriptor-flags", errno.EIO, ("getfl",)), ("access", errno.EIO, (439,)),
             ("pty-attributes", errno.EIO, ("pty-attributes",)),
             ("temp-files", errno.EEXIST, (437,)), ("temp-directories", errno.EEXIST, (258,))]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-host-syscalls-") as directory:
        work = Path(directory)
        (work / "tmp").mkdir()
        (work / "directory").mkdir()
        os.mkfifo(work / "directory/pipe")
        compiled, embedded = work / "native.o", work / "embedded.o"
        result = subprocess.run([native, "--strict"], input=Path(fixture).read_bytes(), capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result
        compiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(embedded), fixture], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
        for obj in (compiled, embedded):
            for operation in ("host-abstract", "host-pathname"):
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as inherited:
                    if operation == "host-abstract":
                        inherited.bind("\0pizdos-host-" + work.name)
                    else:
                        with contextlib.chdir(work):
                            inherited.bind("host.socket")
                    result = subprocess.run([vm, "run", "--root", str(work), "--", str(obj), operation],
                                            stdin=inherited, capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"host syscall errors: ok\n", b""), (operation, result)
                if operation == "host-pathname":
                    (work / "host.socket").unlink()
            reader, writer = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
            try:
                # Keep the writer open and empty: reading this inherited
                # nonblocking input must report EAGAIN rather than EOF.
                result = subprocess.run([vm, "run", "--root", str(work), "--", str(obj), "nonblocking-input"],
                                        stdin=reader, capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"host syscall errors: ok\n", b""), result
            finally:
                os.close(reader)
                os.close(writer)
            for operation, error, numbers in cases:
                result = subprocess.run([wrapper, str(error) + "/" + ",".join(map(str, numbers)), "--", vm, "run", "--root", str(work), "--", str(obj), operation], capture_output=True,
                                        env=env, timeout=30,
                                        preexec_fn=(lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))) if operation == "pty-attributes" else None)
                expected = b"" if operation in ("poll", "connect-poll") else b"host syscall errors: ok\n"
                assert (result.returncode, result.stdout, result.stderr) == (0, expected, b""), (operation, result)
            # Startup reads the kernel object using fstat and pread. Returning
            # zero from a suppressed pread models EOF after its recorded size.
            for number, error, diagnostic in ((5, errno.EIO, b"input/output error"),
                                               (17, errno.EIO, b"input/output error"), (17, 0, b"exec format error")):
                result = subprocess.run([wrapper, f"{error}/{number}", "--", vm, "run", "--", str(obj)],
                                        capture_output=True, env=env, timeout=30)
                assert result.returncode == 1 and result.stdout == b"" and diagnostic in result.stderr and b"panic:" not in result.stderr, (number, error, result)
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
        for name in ("fd_fdstat_get", "fd_filestat_get"):
            source = work / (name + ".wasm")
            data = wasi_descriptor(name, error=0, filetype=6)
            source.write_bytes(data)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, result
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
            for obj in (compiled, embedded):
                reader, writer = socket.socketpair()
                with reader, writer:
                    result = subprocess.run([vm, "run", "--", str(obj)], stdin=reader, capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (name, result)
        devices = [path for path in sorted(Path("/dev").iterdir()) if stat.S_ISBLK(path.lstat().st_mode)]
        if devices:
            for name in ("fd_fdstat_get", "fd_filestat_get"):
                source = work / (name + "-block.wasm")
                data = wasi_descriptor(name, error=0, filetype=1)
                source.write_bytes(data)
                result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, result
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
                # O_PATH permits fstat without opening the device for I/O.
                fd = os.open(devices[0], os.O_PATH | os.O_CLOEXEC)
                try:
                    for obj in (compiled, embedded):
                        result = subprocess.run([vm, "run", "--", str(obj)], stdin=fd, capture_output=True, env=env, timeout=30)
                        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (name, result)
                finally:
                    os.close(fd)
        for directory, name, filetype in ((Path("/dev"), b"null", 2), (work / "directory", b"pipe", 0)):
            source = work / "directory.wasm"
            data = wasi_directory(directory, name, filetype)
            source.write_bytes(data)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, result
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (directory, result)
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
