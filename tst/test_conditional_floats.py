#!/usr/bin/env python3
"""Select whether to divide, take a square root or compare floating values.

    test_conditional_floats.py <vm> <native-aot> <wazero>
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


def floating(value, bits):
    return bytes([0x43 if bits == 32 else 0x44]) + struct.pack("<f" if bits == 32 else "<d", value)


def calculate(operation, a, b):
    if math.isnan(a) or operation != "sqrt" and math.isnan(b):
        return math.nan
    if operation == "sqrt":
        return math.sqrt(a) if a >= 0 else math.nan
    if operation == "div":
        if b == 0:
            return math.nan if a == 0 else math.copysign(math.inf, math.copysign(1, a) * math.copysign(1, b))
        return a / b
    if a == 0 and b == 0:
        negative = (math.copysign(1, a) < 0, math.copysign(1, b) < 0)
        return math.copysign(0, -1 if (any(negative) if operation == "min" else all(negative)) else 1)
    return min(a, b) if operation == "min" else max(a, b)


def program():
    functions, entry, checks = [], bytearray(), 0
    samples = [(0.0, 0.0), (-0.0, 0.0), (0.0, -0.0), (-0.0, -0.0), (1.0, 0.0), (1.0, -0.0),
               (-3.0, 2.0), (2.5, 0.5), (4.0, 7.0), (math.inf, math.inf), (-math.inf, 1.0),
               (math.nan, 1.0), (1.0, math.nan)]
    for bits in (32, 64):
        typ, local = (0x7d, 0) if bits == 32 else (0x7c, 1)
        operations = (0x95, 0x91, 0x96, 0x97) if bits == 32 else (0xa3, 0x9f, 0xa4, 0xa5)
        for operation, opcode in zip(("div", "sqrt", "min", "max"), operations):
            index = len(functions)
            code = b"\x20\0\x04" + bytes([typ]) + b"\x20\1" + (b"" if operation == "sqrt" else b"\x20\2")
            code += bytes([opcode]) + b"\x05\x20\1\x0b"
            functions.append((0 if bits == 32 else 1, b"\0" + code + b"\x0b"))
            for a, b in samples:
                for enabled in (False, True):
                    expected = calculate(operation, a, b) if enabled else a
                    entry += b"\x41" + bytes([int(enabled)]) + floating(a, bits) + floating(b, bits) + b"\x10" + leb(index)
                    entry += b"\x21" + leb(local) + b"\x20" + leb(local)
                    if math.isnan(expected):
                        entry += b"\x20" + leb(local) + bytes([0x5c if bits == 32 else 0x62]) + b"\x45"
                    else:
                        raw = int.from_bytes(struct.pack("<f" if bits == 32 else "<d", expected), "little")
                        if raw >= 1 << (bits - 1):
                            raw -= 1 << bits
                        entry += bytes([0xbc if bits == 32 else 0xbd, 0x41 if bits == 32 else 0x42]) + leb(raw, True)
                        entry += bytes([0x47 if bits == 32 else 0x52])
                    entry += b"\x04\x40\0\x0b"
                    checks += 1
    functions.append((2, b"\2\1\x7d\1\x7c" + bytes(entry) + b"\x0b"))
    types = b"\3\x60\3\x7f\x7d\x7d\1\x7d\x60\3\x7f\x7c\x7c\1\x7c\x60\0\0"
    module = b"\0asm\1\0\0\0" + section(1, types)
    module += section(3, leb(len(functions)) + b"".join(leb(typ) for typ, _ in functions))
    module += section(7, b"\1\6_start\0" + leb(len(functions) - 1))
    return module + section(10, leb(len(functions)) + b"".join(leb(len(body)) + body for _, body in functions)), checks


def main():
    vm, native, wazero = sys.argv[1:]
    data, checks = program()
    with tempfile.TemporaryDirectory(prefix="pizdos-conditional-floats-") as directory:
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
    print(f"conditional floats: {checks} divisions, roots and min/max operations agree with a model, wazero and both AOTs")


if __name__ == "__main__":
    main()
