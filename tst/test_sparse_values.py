#!/usr/bin/env python3
"""Keep early call arguments live across a long stateful integer calculation.

    test_sparse_values.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = (1 << 64) - 1


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


def constant(value, bits=64):
    value &= (1 << bits) - 1
    return bytes([0x41 if bits == 32 else 0x42]) + leb(value - (1 << bits) if value >= 1 << (bits - 1) else value, True)


def get(index):
    return b"\x20" + leb(index)


def program(steps):
    code = get(0) + b"\x24\0"
    for index in range(steps):
        code += b"\x23\0" + constant(1664525) + b"\x7e\x23\0" + constant(17) + b"\x88\x85"
        code += constant(index * 37 + 11) + b"\x85\x24\0"
    code += b"\x23\0\x21\x11" + get(16) + constant(1, 32) + b"\x71\x04\x7e"
    code += get(17) + get(0) + b"\x7c\x22\x11\x24\0" + get(17) + b"\x05"
    code += get(17) + get(1) + b"\x85\x22\x11\x24\0" + get(17) + b"\x0b\x21\x11" + constant(0)
    for index in range(18):
        if index == 16:
            continue
        code += constant(33) + b"\x7e" + get(index) + b"\x85"
    entry = b""
    for seed in (0, 1, MASK):
        values = [((seed + index * 0x9e3779b185ebca87) ^ (index * 37)) & MASK for index in range(16)]
        state = values[0]
        for index in range(steps):
            state = ((state * 1664525) ^ (state >> 17) ^ (index * 37 + 11)) & MASK
        for flags in (0, 1, 2, 0xffffffff):
            last = (state + values[0]) & MASK if flags & 1 else state ^ values[1]
            expected = 0
            for value in (*values, last):
                expected = ((expected * 33) ^ value) & MASK
            entry += b"".join(constant(value) for value in values) + constant(flags, 32) + b"\x10\0"
            entry += constant(expected) + b"\x52\x04\x40\0\x0b"
            entry += b"\x23\0" + constant(last) + b"\x52\x04\x40\0\x0b"
    signature = b"\x60\x11" + b"\x7e" * 16 + b"\x7f\1\x7e"
    module = b"\0asm\1\0\0\0" + section(1, b"\2" + signature + b"\x60\0\0") + section(3, b"\2\0\1")
    module += section(6, b"\1\x7e\1" + constant(0) + b"\x0b") + section(7, b"\1\6_start\0\1")
    bodies = [b"\1\1\x7e" + code + b"\x0b", b"\0" + entry + b"\x0b"]
    return module + section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))


def main():
    vm, native, wazero = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pizdos-sparse-values-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for steps in (8193, 16385):
            data = program(steps)
            source.write_bytes(data)
            result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=60)
            assert result.returncode == 0 and result.stdout == b"", (steps, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=180)
                assert result.returncode == 0, (steps, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=180)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (steps, result)
                result = subprocess.run([vm, "run", "--", str(source)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (steps, strict, result)
    print("sparse values: early arguments survive stateful calculations through 16385 steps and agree with a model, wazero and both AOTs")


if __name__ == "__main__":
    main()
