#!/usr/bin/env python3
"""Exercise compiler stream failures with real host descriptors.

    test_host_io.py <vm> <native-aot>
"""
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


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    payload = bytes(i % 251 for i in range(131072))
    data = b"\0asm\1\0\0\0" + section(1, b"\1\x60\0\0") + section(3, b"\1\0")
    data += section(5, b"\1\1\3\3") + section(7, b"\1\6_start\0\0") + section(10, b"\1\2\0\x0b")
    data += section(11, b"\1\0\x41\x80\x20\x0b" + leb(len(payload)) + payload)
    with tempfile.TemporaryDirectory(prefix="pizdos-host-io-") as directory:
        work = Path(directory)
        source, embedded, compiled = [work / name for name in ("input.wasm", "embedded.o", "native.o")]
        source.write_bytes(data)
        reference = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
        assert reference.returncode == 0 and len(reference.stdout) > 65536, reference
        compiled.write_bytes(reference.stdout)
        result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == reference.stdout, result
        for obj in (compiled, embedded):
            result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result

        with source.open("rb") as stream:
            result = subprocess.run([native, "--strict"], stdin=stream, capture_output=True, env=env,
                                    preexec_fn=lambda: os.close(0), timeout=60)
        assert result.returncode != 0 and result.stdout == b"" and b"stdin read failed" in result.stderr, result
        result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env,
                                preexec_fn=lambda: os.close(1), timeout=60)
        assert result.returncode != 0 and result.stdout == b"", result
        with open("/dev/full", "wb", buffering=0) as full:
            result = subprocess.run([native, "--strict"], input=data, stdout=full, stderr=subprocess.PIPE, env=env, timeout=60)
        assert result.returncode != 0, result

        reader, writer = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
        try:
            result = subprocess.run([native, "--strict"], input=data, stdout=writer, stderr=subprocess.PIPE, env=env, timeout=60)
            os.close(writer)
            writer = -1
            os.set_blocking(reader, True)
            with os.fdopen(reader, "rb", closefd=False) as stream:
                partial = stream.read()
            assert result.returncode != 0 and 0 < len(partial) < len(reference.stdout), (result, len(partial))
            assert reference.stdout.startswith(partial), "partial output was not the object prefix"
        finally:
            os.close(reader)
            if writer >= 0:
                os.close(writer)
        # Descriptor failures are confined to the failed command.
        result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and result.stdout == reference.stdout, result
    print("host I/O: closed input/output, full device, partial nonblocking output and later recovery passed")


if __name__ == "__main__":
    main()
