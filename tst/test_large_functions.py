#!/usr/bin/env python3
"""Compile and run large checksums and calls with thousands of arguments.

    test_large_functions.py <vm> <native-aot>

Python computes the expected results independently. Both compiler builds
must emit identical objects, and both objects must produce those results.
"""
import os
from pathlib import Path
import struct
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


# kind: Wasm type, memory load, add, not-equal, alignment, integer width.
KINDS = {
    "i32": (0x7f, 0x28, 0x6a, 0x47, 2, 32),
    "i64": (0x7e, 0x29, 0x7c, 0x52, 3, 64),
    "f32": (0x7d, 0x2a, 0x92, 0x5c, 2, 0),
    "f64": (0x7c, 0x2b, 0xa0, 0x62, 3, 0),
}
FORMATS = {"i32": "I", "i64": "Q", "f32": "f", "f64": "d"}


def constant(kind, value):
    width = KINDS[kind][5]
    if width:
        value &= (1 << width) - 1
        if value >= 1 << (width - 1):
            value -= 1 << width
        return bytes([0x41 if width == 32 else 0x42]) + leb(value, True)
    return bytes([0x43 if kind == "f32" else 0x44]) + struct.pack("<" + FORMATS[kind], value)


def value(kind, index):
    width = KINDS[kind][5]
    if width:
        return ((index * 0x9e3779b185ebca87) ^ (index * 37 + 19)) & ((1 << width) - 1)
    # Quarter integers have exact sums here, for both floating-point types.
    return ((index * 13) % 61 - 30) / 4


def module(parameters, result, calculate, entry, image=b""):
    types = b"\2\x60" + leb(len(parameters)) + bytes(parameters) + b"\1" + bytes([result]) + b"\x60\0\0"
    out = b"\0asm\1\0\0\0" + section(1, types) + section(3, b"\2\0\1")
    if image:
        out += section(5, b"\1\0" + leb((2048 + len(image) + 65535) // 65536))
    out += section(7, b"\1\6_start\0\1")
    bodies = [b"\0" + code + b"\x0b" for code in (calculate, entry)]
    out += section(10, b"\2" + b"".join(leb(len(code)) + code for code in bodies))
    if image:
        out += section(11, b"\1\0" + constant("i32", 2048) + b"\x0b" + leb(len(image)) + image)
    return out


def checksum(kind, count):
    typ, load, add, unequal, alignment, _ = KINDS[kind]
    values = [value(kind, i) for i in range(count + 1)]
    image = struct.pack("<" + FORMATS[kind] * len(values), *values)
    calculate = b"".join(b"\x20\0" + bytes([load, alignment]) + leb(i << alignment)
                         + (bytes([add]) if i else b"") for i in range(count))
    entry = b""
    for begin in (0, 1):
        expected = sum(values[begin:begin + count])
        entry += constant("i32", 2048 + (begin << alignment)) + b"\x10\0" + constant(kind, expected)
        entry += bytes([unequal]) + b"\x04\x40\0\x0b"
    return module([0x7f], typ, calculate, entry, image)


def wide_call(count):
    parameters, arguments, operations = [], [], [constant("i64", 0)]
    expected = 0
    for index in range(count):
        kind = tuple(KINDS)[index % 4]
        v = value(kind, index)
        bits = int.from_bytes(struct.pack("<" + FORMATS[kind], v), "little")
        expected = ((expected * 33) ^ bits) & ((1 << 64) - 1)
        parameters.append(KINDS[kind][0])
        arguments.append(constant(kind, v))
        operations.append(constant("i64", 33) + b"\x7e\x20" + leb(index))
        if kind == "i32":
            operations.append(b"\xad")
        elif kind == "f32":
            operations.append(b"\xbc\xad")
        elif kind == "f64":
            operations.append(b"\xbd")
        operations.append(b"\x85")
    entry = b"".join(arguments) + b"\x10\0" + constant("i64", expected) + b"\x52\x04\x40\0\x0b"
    return module(parameters, 0x7e, b"".join(operations), entry)


def main():
    vm, native = sys.argv[1:]
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-large-functions-") as directory:
        work = Path(directory)
        cases = [(kind, count) for count in (16383, 16385, 32769) for kind in KINDS]
        cases += [("mixed-call", count) for count in (28, 31, 32, 33, 64, 96, 1000)]
        for name, count in cases:
            data = wide_call(count) if name == "mixed-call" else checksum(name, count)
            source = work / "input.wasm"
            source.write_bytes(data)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                compiled, embedded = work / "native.o", work / "embedded.o"
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data,
                                        capture_output=True, env=env, timeout=180)
                assert result.returncode == 0, (name, count, strict, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=180)
                assert result.returncode == 0, (name, count, strict, result.stderr)
                assert compiled.read_bytes() == embedded.read_bytes(), (name, count, strict)
                result = subprocess.run([vm, "run", "--", str(source)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (name, count, strict, result)
                checked += 1
    print(f"large functions: {checked} checksum/mixed-call programs agree with independent results and both AOTs")


if __name__ == "__main__":
    main()
