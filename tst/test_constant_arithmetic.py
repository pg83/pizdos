#!/usr/bin/env python3
"""Evaluate raw constant expressions against Python integers and wazero.

    test_constant_arithmetic.py <vm> <native-aot> <wazero>
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


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >= 1 << (bits - 1) else value


def constant(value, bits):
    return bytes([0x41 if bits == 32 else 0x42]) + leb(signed(value, bits), True)


def quotient(a, b):
    value = abs(a) // abs(b)
    return -value if (a < 0) != (b < 0) else value


def binary(kind, a, b, bits):
    mask = (1 << bits) - 1
    sa, sb, amount = signed(a, bits), signed(b, bits), b % bits
    if kind == "add":
        value = a + b
    elif kind == "sub":
        value = a - b
    elif kind == "mul":
        value = a * b
    elif kind == "divs":
        value = quotient(sa, sb)
    elif kind == "divu":
        value = a // b
    elif kind == "rems":
        value = sa - quotient(sa, sb) * sb
    elif kind == "remu":
        value = a % b
    elif kind == "and":
        value = a & b
    elif kind == "or":
        value = a | b
    elif kind == "xor":
        value = a ^ b
    elif kind == "shl":
        value = a << amount
    elif kind == "shrs":
        value = sa >> amount
    elif kind == "shru":
        value = a >> amount
    elif kind == "rotl":
        value = (a << amount) | (a >> ((bits - amount) % bits))
    elif kind == "rotr":
        value = (a >> amount) | (a << ((bits - amount) % bits))
    return value & mask


def module(functions, entry):
    types = b"\3\x60\0\1\x7f\x60\0\1\x7e\x60\0\0"
    functions = [(0 if bits == 32 else 1, b"\0" + code + b"\x0b") for bits, code in functions]
    functions.append((2, b"\0" + entry + b"\x0b"))
    result = b"\0asm\1\0\0\0" + section(1, types)
    result += section(3, leb(len(functions)) + b"".join(leb(typ) for typ, _ in functions))
    result += section(7, b"\1\6_start\0" + leb(len(functions) - 1))
    return result + section(10, leb(len(functions)) + b"".join(leb(len(body)) + body for _, body in functions))


def program():
    functions, entry = [], bytearray()

    def add(bits, code, expected):
        entry.extend(b"\x10" + leb(len(functions)) + constant(expected, bits))
        entry.extend(bytes([0x47 if bits == 32 else 0x52]) + b"\x04\x40\0\x0b")
        functions.append((bits, code))

    for bits in (32, 64):
        mask = (1 << bits) - 1
        values = (0, 1, 2, 17, bits, (1 << (bits - 1)) - 1, 1 << (bits - 1), mask - 1, mask)
        kinds = ("add", "sub", "mul", "divs", "divu", "rems", "remu", "and", "or", "xor", "shl", "shrs", "shru", "rotl", "rotr")
        for condition in (0, 1):
            code = constant(15, bits) + constant(27, bits) + constant(condition, 32) + b"\x1c\1" + bytes([0x7f if bits == 32 else 0x7e])
            add(bits, code, 15 if condition else 27)
        for a in values:
            for b in values:
                for offset, kind in enumerate(kinds):
                    if kind in ("divs", "divu", "rems", "remu") and b == 0:
                        continue
                    if kind == "divs" and a == 1 << (bits - 1) and b == mask:
                        continue
                    code = constant(a, bits) + constant(b, bits) + bytes([(0x6a if bits == 32 else 0x7c) + offset])
                    add(bits, code, binary(kind, a, b, bits))
                predicates = (a == b, a != b, signed(a, bits) < signed(b, bits), a < b,
                              signed(a, bits) > signed(b, bits), a > b, signed(a, bits) <= signed(b, bits),
                              a <= b, signed(a, bits) >= signed(b, bits), a >= b)
                for offset, expected in enumerate(predicates):
                    code = constant(a, bits) + constant(b, bits) + bytes([(0x46 if bits == 32 else 0x51) + offset])
                    add(32, code, int(expected))
            for offset, expected in enumerate((bits - a.bit_length(), (a & -a).bit_length() - 1 if a else bits, a.bit_count())):
                add(bits, constant(a, bits) + bytes([(0x67 if bits == 32 else 0x79) + offset]), expected)
            add(32, constant(a, bits) + bytes([0x45 if bits == 32 else 0x50]), int(a == 0))
            for width, opcode in ((8, 0xc0 if bits == 32 else 0xc2), (16, 0xc1 if bits == 32 else 0xc3)):
                add(bits, constant(a, bits) + bytes([opcode]), signed(a, width))
            if bits == 64:
                add(64, constant(a, bits) + b"\xc4", signed(a, 32))
                add(32, constant(a, bits) + b"\xa7", a & 0xffffffff)
            else:
                add(64, constant(a, bits) + b"\xac", signed(a, 32))
                add(64, constant(a, bits) + b"\xad", a)
    return module(functions, bytes(entry)), len(functions)


def main():
    vm, native, wazero = sys.argv[1:]
    data, checks = program()
    cases = [("values", data, 0, b"")]
    for bits in (32, 64):
        mask = (1 << bits) - 1
        for offset in (3, 4, 5, 6):
            code = constant(123, bits) + constant(0, bits) + bytes([(0x6a if bits == 32 else 0x7c) + offset])
            cases.append((f"zero-{bits}-{offset}", module([(bits, code)], b"\x10\0\x1a"), 132, b"integer divide by zero"))
        code = constant(1 << (bits - 1), bits) + constant(mask, bits) + bytes([0x6d if bits == 32 else 0x7f])
        cases.append((f"overflow-{bits}", module([(bits, code)], b"\x10\0\x1a"), 131, b"integer overflow"))
    with tempfile.TemporaryDirectory(prefix="pizdos-constant-arithmetic-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for label, data, status, diagnostic in cases:
            source.write_bytes(data)
            result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=60)
            assert bool(result.returncode) == bool(status) and result.stdout == b"", (label, result)
            if status:
                assert diagnostic in result.stderr, (label, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (label, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (label, result)
                for obj in (compiled, embedded):
                    result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                    assert result.returncode == status and result.stdout == b"" and diagnostic in result.stderr, (label, result)
                    if not status:
                        assert result.stderr == b"", result
    print(f"constant arithmetic: {checks} integer expressions and {len(cases) - 1} constant traps agree with Python, wazero and both AOTs")


if __name__ == "__main__":
    main()
