#!/usr/bin/env python3
"""Branch tables with only a default, including value-carrying branches.

    test_table_defaults.py <vm> <native-aot> <wazero>
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


def integer(value):
    value &= 0xffffffff
    return b"\x41" + leb(value - (1 << 32) if value >= 1 << 31 else value, True)


def check(value, expected):
    return value + integer(expected) + b"\x47\x04\x40\0\x0b"


def program():
    functions, entry, checks = [], bytearray(), 0
    for target in ("block", "outer", "return"):
        for entries in (0, 1, 3):
            # Every index goes to the default label. The index remains a
            # function parameter so constant folding cannot choose an edge.
            depth = 1 if target == "outer" else 0
            code = integer(4096) + b"\x20\1\x36\2\0"
            if target != "return":
                code += b"\x02\x7f"
            if target == "outer":
                code += b"\x02\x40"
            code += b"\x20\1" + integer(17) + b"\x6a\x20\0\x0e"
            code += leb(entries) + leb(depth) * (entries + 1) + b"\0"
            if target == "outer":
                code += b"\x0b\0"
            if target != "return":
                code += b"\x0b" + integer(0x5a5a5a5a) + b"\x73"
            index = len(functions)
            functions.append(b"\0" + code + b"\x0b")
            for selector in (0, 1, 2, 127, 128, 0x7fffffff, 0x80000000, 0xffffffff):
                for seed in (0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678):
                    expected = (seed + 17) & 0xffffffff
                    if target != "return":
                        expected ^= 0x5a5a5a5a
                    entry += check(integer(selector) + integer(seed) + b"\x10" + leb(index), expected)
                    entry += check(integer(4096) + b"\x28\2\0", seed)
                    checks += 1
    functions.append(b"\0" + bytes(entry) + b"\x0b")
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\2\x7f\x7f\1\x7f\x60\0\0")
    module += section(3, leb(len(functions)) + b"\0" * (len(functions) - 1) + b"\1")
    module += section(5, b"\1\1\1\1") + section(7, b"\1\6_start\0" + leb(len(functions) - 1))
    return module + section(10, leb(len(functions)) + b"".join(leb(len(body)) + body for body in functions)), checks


def main():
    vm, native, wazero = sys.argv[1:]
    data, checks = program()
    with tempfile.TemporaryDirectory(prefix="pizdos-table-defaults-") as directory:
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
    print(f"table defaults: {checks} empty and repeated-target tables preserve branch values and memory with wazero and both AOTs")


if __name__ == "__main__":
    main()
