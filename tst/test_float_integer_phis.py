#!/usr/bin/env python3
"""Compare floating values merged from constants and unsigned integers.

    test_float_integer_phis.py <vm> <native-aot> <wazero>
"""
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def leb(value, signed=False):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(out + bytes([byte]))
        out.append(byte | 128)


def section(kind, data):
    return bytes([kind]) + leb(len(data)) + data


def integer(value):
    value &= 0xffffffff
    return b"\x41" + leb(value - (1 << 32) if value >= 1 << 31 else value, True)


def check(actual, expected):
    return actual + integer(expected) + b"\x47\x04\x40\0\x0b"


def program():
    functions, entry, checks = [], bytearray(), 0
    pairs = ((0, 0), (1, 0), (0, 1), (123, 122), (123, 123), (123, 124), (0x7fffffff, 0x80000000), (0xffffffff, 0xffffffff))
    for constant in (-1.0, -0.0, 0.0, 2.5, 123.0, 4294967295.0, 4294967296.0, math.inf, math.nan):
        for relation in range(6):
            index = len(functions)
            # Different global writes keep the two branches observable while
            # their floating results meet at a block parameter.
            code = b"\x20\2" + integer(1) + b"\x71\x04\x40"
            code += b"\x44" + struct.pack("<d", constant) + b"\x21\3\x20\0\x24\0\x05"
            code += b"\x20\0\xb8\x21\3\x20\1\x24\0\x0b\x20\3\x20\1\xb8" + bytes([0x61 + relation])
            functions.append((0, b"\1\1\x7c" + code + b"\x0b"))
            for x, y in pairs:
                for flags in (0, 1, 2, 3):
                    left = constant if flags & 1 else float(x)
                    choices = (left == y, left != y, left < y, left > y, left <= y, left >= y)
                    entry += check(integer(x) + integer(y) + integer(flags) + b"\x10" + leb(index), choices[relation])
                    entry += check(b"\x23\0", x if flags & 1 else y)
                    checks += 1
    functions.append((1, b"\0" + bytes(entry) + b"\x0b"))
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\3\x7f\x7f\x7f\1\x7f\x60\0\0")
    module += section(3, leb(len(functions)) + b"".join(leb(kind) for kind, _ in functions))
    module += section(6, b"\1\x7f\1" + integer(1024) + b"\x0b")
    module += section(7, b"\1\6_start\0" + leb(len(functions) - 1))
    return module + section(10, leb(len(functions)) + b"".join(leb(len(body)) + body for _, body in functions)), checks


def main():
    vm, native, wazero = sys.argv[1:]
    data, checks = program()
    with tempfile.TemporaryDirectory(prefix="pizdos-float-integer-phis-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        source.write_bytes(data)
        result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=30)
        assert result.returncode == 0 and result.stdout == b"", result
        for strict in (False, True):
            env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
            result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, result.stderr
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
            result = subprocess.run([vm, "run", "--", str(source)], capture_output=True, env=env, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
    print(f"float integer phis: {checks} comparisons and branch effects agree with a Python model, wazero and both AOTs")


if __name__ == "__main__":
    main()
