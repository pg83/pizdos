#!/usr/bin/env python3
"""Guest calls under a real host descriptor limit must fail and recover.

    test_host_resources.py <vm> <native-aot> <fixture.wasm>
"""
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile


def bootstrap_program():
    # A kernel entry that checks getpid() == 1 without allocating or doing I/O.
    sections = [(1, b"\2\x60\0\1\x7f\x60\0\0"),
                (2, b"\1\6kernel\6getpid\0\0"), (3, b"\1\1"),
                (7, b"\1\6_start\0\1"),
                (10, b"\1\13\0\x10\0\x41\1\x47\x04\x40\0\x0b\x0b")]
    return b"\0asm\1\0\0\0" + b"".join(bytes([kind, len(body)]) + body for kind, body in sections)


def main():
    vm, native, fixture = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-host-resources-") as directory:
        work = Path(directory)
        compiled, embedded = work / "native.o", work / "embedded.o"
        result = subprocess.run([native, "--strict"], input=Path(fixture).read_bytes(), capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result
        compiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(embedded), fixture], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
        for soft in (8, 64):
            def restrict():
                resource.setrlimit(resource.RLIMIT_NOFILE, (soft, 64))
            for operation in ("objects", "fork", "vfork", "forkpty"):
                result = subprocess.run([vm, "run", "--root", str(work), "--", str(fixture), operation], capture_output=True,
                                        env=env, preexec_fn=restrict, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"host resource recovery: ok\n", b""), (soft, operation, result)
        source = work / "bootstrap.wasm"
        data = bootstrap_program()
        source.write_bytes(data)
        result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result
        compiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
        for limit in (8, 9, 10, 12):
            def restrict():
                resource.setrlimit(resource.RLIMIT_NOFILE, (limit, limit))
            result = subprocess.run([vm, "run", "--", str(source)], capture_output=True,
                                    env=env, preexec_fn=restrict, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (limit, result)
    print("host resources: pipes, sockets, PTYs and process creation recover from real descriptor exhaustion with both AOTs")


if __name__ == "__main__":
    main()
