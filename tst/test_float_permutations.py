#!/usr/bin/env python3
"""Repeated parallel permutations preserve every floating-point bit pattern.

    test_float_permutations.py <vm> <native-aot> <wazero>
"""
import math
import os
from pathlib import Path
import struct
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


def constant(value, bits=32):
    value &= (1 << bits) - 1
    return bytes([0x41 if bits == 32 else 0x42]) + leb(value - (1 << bits) if value >= 1 << (bits - 1) else value, True)


def get(index):
    return b"\x20" + leb(index)


def patterns(bits, count):
    special = ([0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00123] if bits == 32 else
               [0, 0x8000000000000000, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000123])
    return [special[i] if i < len(special) else int.from_bytes(struct.pack("<f" if bits == 32 else "<d", (i * 13 % 61 - 30) / 4), "little")
            for i in range(count)]


def program(bits, count, kind):
    if kind == "rotate":
        left, right = [(i + 1) % count for i in range(count)], [(i - 1) % count for i in range(count)]
    elif kind == "reverse":
        left, right = list(reversed(range(count))), [(i + 3) % count for i in range(count)]
    else:
        step = 3
        while math.gcd(step, count) != 1:
            step += 2
        left, right = [(i * step + 1) % count for i in range(count)], [(i * step - 1) % count for i in range(count)]

    def permute(order):
        # Read all old values before writing any destination, as a parallel
        # assignment. The interpreter and Python model retain the same order.
        return b"".join(get(2 + i) for i in order) + b"".join(b"\x21" + leb(2 + i) for i in reversed(range(count)))

    code = b"\x02\x40" + get(0) + b"\x45\x0d\0\x03\x40" + get(1) + constant(64) + b"\x71\x04\x40"
    code += permute(left) + b"\x05" + permute(right) + b"\x0b"
    code += get(1) + constant(1664525) + b"\x6c" + constant(1013904223) + b"\x6a\x21\1"
    code += get(0) + constant(1) + b"\x6b\x22\0\x0d\0\x0b\x0b" + constant(0, 64)
    for index in range(count):
        code += constant(33, 64) + b"\x7e" + get(index + 2) + (b"\xbc\xad" if bits == 32 else b"\xbd") + b"\x85"
    values, entry = patterns(bits, count), b""
    for rounds in (0, 1, 2, 3, 17):
        for seed in (0, 64, 0xffffffff):
            permuted, state = list(values), seed
            for _ in range(rounds):
                order = left if state & 64 else right
                permuted = [permuted[i] for i in order]
                state = (state * 1664525 + 1013904223) & 0xffffffff
            expected = 0
            for value in permuted:
                expected = ((expected * 33) ^ value) & MASK
            entry += constant(rounds) + constant(seed)
            entry += b"".join(bytes([0x43 if bits == 32 else 0x44]) + value.to_bytes(bits // 8, "little") for value in values)
            entry += b"\x10\0" + constant(expected, 64) + b"\x52\x04\x40\0\x0b"
    typ = 0x7d if bits == 32 else 0x7c
    signature = b"\x60" + leb(count + 2) + b"\x7f\x7f" + bytes([typ]) * count + b"\1\x7e"
    module = b"\0asm\1\0\0\0" + section(1, b"\2" + signature + b"\x60\0\0") + section(3, b"\2\0\1")
    module += section(7, b"\1\6_start\0\1")
    bodies = [b"\0" + body + b"\x0b" for body in (code, entry)]
    return module + section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))


def main():
    vm, native, wazero = sys.argv[1:]
    cases = [(bits, count, kind) for bits in (32, 64) for count in (2, 7, 15, 16, 17, 31, 32, 33, 64)
             for kind in ("rotate", "reverse", "affine")]
    with tempfile.TemporaryDirectory(prefix="pizdos-float-permutations-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for case in cases:
            data = program(*case)
            source.write_bytes(data)
            result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0 and result.stdout == b"", (case, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (case, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (case, result)
                result = subprocess.run([vm, "run", "--", str(source)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (case, strict, result)
    print(f"float permutations: {len(cases)} rotation, reversal and affine permutations preserve all bits across repeated branches with both AOTs and wazero")


if __name__ == "__main__":
    main()
