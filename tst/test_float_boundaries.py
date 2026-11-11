#!/usr/bin/env python3
"""Float guards around zero, subnormals, infinity and NaN before truncation.

    test_float_boundaries.py <vm> <native-aot> <wazero>
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
    return b"\x41" + leb(value, True)


def floating(value, bits):
    return bytes([0x43 if bits == 32 else 0x44]) + struct.pack("<f" if bits == 32 else "<d", value)


def program():
    functions, entry, checks = [], bytearray(), 0
    for bits in (32, 64):
        tiny = 2.0 ** (-149 if bits == 32 else -1074)
        boundaries = (-math.inf, -1.0, -0.0, 0.0, tiny, 1.0, 2.5, math.inf, math.nan)
        samples = (-math.inf, -17.0, -16.0, -2.75, -1.0, -tiny, -0.0, 0.0, tiny, 1.0, 2.5, 15.75, 16.0, 17.0, math.inf, math.nan)
        for boundary in boundaries:
            for relation in range(6):
                index = len(functions)
                op = (0x5b if bits == 32 else 0x61) + relation
                code = b"\x20\0" + floating(boundary, bits) + bytes([op]) + b"\x04\x7f"
                code += b"\x20\0" + floating(-16.0, bits) + bytes([0x60 if bits == 32 else 0x66])
                code += b"\x20\0" + floating(16.0, bits) + bytes([0x5f if bits == 32 else 0x65]) + b"\x71\x04\x7f"
                code += b"\x20\0" + bytes([0xa8 if bits == 32 else 0xaa]) + b"\x05" + integer(1337) + b"\x0b\x05" + integer(1337) + b"\x0b"
                functions.append((0 if bits == 32 else 1, b"\0" + code + b"\x0b"))
                for value in samples:
                    choices = (value == boundary, value != boundary, value < boundary, value > boundary,
                               value <= boundary, value >= boundary)
                    expected = int(value) if choices[relation] and -16 <= value <= 16 else 1337
                    entry += floating(value, bits) + b"\x10" + leb(index) + integer(expected) + b"\x47\x04\x40\0\x0b"
                    checks += 1
    functions.append((2, b"\0" + bytes(entry) + b"\x0b"))
    module = b"\0asm\1\0\0\0" + section(1, b"\3\x60\1\x7d\1\x7f\x60\1\x7c\1\x7f\x60\0\0")
    module += section(3, leb(len(functions)) + b"".join(leb(typ) for typ, _ in functions))
    module += section(7, b"\1\6_start\0" + leb(len(functions) - 1))
    return module + section(10, leb(len(functions)) + b"".join(leb(len(body)) + body for _, body in functions)), checks


def main():
    vm, native, wazero = sys.argv[1:]
    data, checks = program()
    with tempfile.TemporaryDirectory(prefix="pizdos-float-boundaries-") as directory:
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
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
    print(f"float boundaries: {checks} guarded f32/f64 truncations agree with a Python model, wazero and both AOTs")


if __name__ == "__main__":
    main()
