#!/usr/bin/env python3
"""Deep sequences of stateful branches must retain their observable effects.

    test_deep_control.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
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


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value):
    value &= 0xffffffff
    if value >= 1 << 31:
        value -= 1 << 32
    return b"\x41" + leb(value, True)


def program(count):
    calculate = b""
    for branch in range(count):
        calculate += b"\x20\0" + constant(1 << (branch % 32)) + b"\x71\x04\x40"
        calculate += b"\x23\0" + constant(branch + 1) + b"\x6a\x24\0\x0b"
    entry = b""
    for mask in (0, 1, 0xaaaaaaaa, 0xffffffff):
        expected = sum(branch + 1 for branch in range(count) if mask & (1 << (branch % 32)))
        entry += constant(0) + b"\x24\0" + constant(mask) + b"\x10\0\x23\0"
        entry += constant(expected) + b"\x47\x04\x40\0\x0b"
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\1\x7f\0\x60\0\0") + section(3, b"\2\0\1")
    module += section(6, b"\1\x7f\1\x41\0\x0b") + section(7, b"\1\6_start\0\1")
    bodies = [b"\0" + body + b"\x0b" for body in (calculate, entry)]
    return module + section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))


def main():
    vm, native, wazero = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pizdos-deep-control-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for count in (2049, 4097):
            data = program(count)
            source.write_bytes(data)
            result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=60)
            assert result.returncode == 0 and result.stdout == b"", (count, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=120)
                assert result.returncode == 0, (count, strict, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=120)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (count, strict, result)
                for obj in (compiled, embedded):
                    result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (count, strict, result)
    print("deep control: stateful branch sequences through 4097 decisions agree with the independent checksum, wazero and both AOTs")


if __name__ == "__main__":
    main()
