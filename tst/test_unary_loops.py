#!/usr/bin/env python3
"""Memory loops with unary counter updates and runtime state.

    test_unary_loops.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
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


def integer(value, bits=32):
    value &= (1 << bits) - 1
    return bytes([0x41 if bits == 32 else 0x42]) + leb(value - (1 << bits) if value >= 1 << (bits - 1) else value, True)


def check(actual, expected):
    return actual + integer(expected) + b"\x47\x04\x40\0\x0b"


def program():
    functions, entry, checks = [], bytearray(), 0
    for bits in (32, 64):
        mask = (1 << bits) - 1
        counters = (("clz", 1 << (bits - 1), 27 if bits == 32 else 58),
                    ("ctz", 1 << (bits - 1), 5 if bits == 32 else 6),
                    ("popcnt", mask, 1), ("eqz", 1, 1), ("extend8", 0, 7), ("extend16", 0, 7))
        for kind, initial, stop in counters:
            index = len(functions)
            update = b"\x20\1"
            if kind in ("extend8", "extend16"):
                update += integer(1, bits) + bytes([0x6a if bits == 32 else 0x7c])
                update += bytes([(0xc0 if bits == 32 else 0xc2) + (kind == "extend16")])
            elif kind == "eqz":
                update += bytes([0x45 if bits == 32 else 0x50]) + (b"\xad" if bits == 64 else b"")
            else:
                update += bytes([(0x67 if bits == 32 else 0x79) + ("clz", "ctz", "popcnt").index(kind)])
            narrow = b"\xa7" if bits == 64 else b""
            code = integer(initial, bits) + b"\x21\1\x03\x40" + integer(4096)
            code += b"\x20\0" + integer(33) + b"\x6c\x20\1" + narrow + b"\x73\x69\x22\0\x36\2\0"
            code += update + b"\x22\1" + integer(stop, bits) + bytes([0x47 if bits == 32 else 0x52]) + b"\x0d\0\x0b"
            code += b"\x20\0\x20\1" + narrow + b"\x73"
            functions.append((0, b"\1\1" + bytes([0x7f if bits == 32 else 0x7e]) + code + b"\x0b"))
            for seed in (0, 1, 123, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff):
                counter, value, rounds = initial, seed, 0
                while True:
                    value = (((value * 33) ^ (counter & 0xffffffff)) & 0xffffffff).bit_count()
                    if kind == "clz":
                        counter = bits - counter.bit_length()
                    elif kind == "ctz":
                        counter = (counter & -counter).bit_length() - 1 if counter else bits
                    elif kind == "popcnt":
                        counter = counter.bit_count()
                    elif kind == "eqz":
                        counter = int(counter == 0)
                    else:
                        width = 8 if kind == "extend8" else 16
                        counter = (counter + 1) & ((1 << width) - 1)
                        if counter >= 1 << (width - 1):
                            counter = (counter - (1 << width)) & mask
                    rounds += 1
                    assert rounds < 16, (kind, bits, counter)
                    if counter == stop:
                        break
                entry += check(integer(seed) + b"\x10" + leb(index), value ^ (counter & 0xffffffff))
                entry += check(integer(4096) + b"\x28\2\0", value)
                checks += 1
    functions.append((1, b"\0" + bytes(entry) + b"\x0b"))
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\1\x7f\1\x7f\x60\0\0")
    module += section(3, leb(len(functions)) + b"".join(leb(kind) for kind, _ in functions))
    module += section(5, b"\1\1\1\1") + section(7, b"\1\6_start\0" + leb(len(functions) - 1))
    return module + section(10, leb(len(functions)) + b"".join(leb(len(body)) + body for _, body in functions)), checks


def main():
    vm, native, wazero = sys.argv[1:]
    data, checks = program()
    with tempfile.TemporaryDirectory(prefix="pizdos-unary-loops-") as directory:
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
    print(f"unary loops: {checks} 32/64-bit counter loops preserve their result and memory with a Python model, wazero and both AOTs")


if __name__ == "__main__":
    main()
