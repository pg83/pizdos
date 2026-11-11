#!/usr/bin/env python3
"""Run state machines with many paths returning to one dispatch table.

    test_dispatch_loops.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = (1 << 32) - 1


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
    value &= MASK
    return b"\x41" + leb(value - (1 << 32) if value >= 1 << 31 else value, True)


def get(index):
    return b"\x20" + leb(index)


def program(count, bounded=False):
    # Each case independently tests its remaining iterations. Its back
    # edge carries the next key to the same dispatch, with no shared latch.
    code = get(1) + b"\x45\x04\x40\x23\0\x0f\x0b"
    code += b"\x02\x40\x03\x40" + b"\x02\x40" * count
    code += get(0) + (constant(count - 2) + b"\x71" if bounded else b"") + b"\x0e" + leb(count) + b"".join(leb(i) for i in range(count)) + leb(count - 1)
    for index in range(count):
        depth = count - index - 1
        code += b"\x0b\x23\0" + constant(index + 1) + b"\x6a\x24\0"
        code += get(0) + get(2) + constant(index + 3) + b"\x6c\x6a" + constant(count + 7) + b"\x73\x21\0"
        code += get(1) + constant(1) + b"\x6b\x22\1\x0d" + leb(depth)
        code += b"\x0c" + leb(depth + 1)
    code += b"\x0b\x0b\x23\0"
    entry, checks = bytearray(), 0
    for key in (0, 1, count - 1, count, MASK):
        for seed in (0, 1, 17, 0x80000000, MASK):
            for rounds in (0, 1, 2, 17, 65):
                result, current = 0, key
                for _ in range(rounds):
                    index = (current & (count - 2)) if bounded else min(current, count - 1)
                    result = (result + index + 1) & MASK
                    current = (((current + seed * (index + 3)) & MASK) ^ (count + 7)) & MASK
                entry += constant(0) + b"\x24\0" + constant(key) + constant(rounds) + constant(seed) + b"\x10\0"
                entry += constant(result) + b"\x47\x04\x40\0\x0b"
                checks += 1
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\3\x7f\x7f\x7f\1\x7f\x60\0\0")
    module += section(3, b"\2\0\1") + section(6, b"\1\x7f\1" + constant(0) + b"\x0b")
    module += section(7, b"\1\6_start\0\1")
    bodies = [b"\0" + body + b"\x0b" for body in (code, bytes(entry))]
    return module + section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies)), checks


def main():
    vm, native, wazero = sys.argv[1:]
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-dispatch-loops-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for count, bounded in ((count, bounded) for count in (17, 33) for bounded in (False, True)):
            data, checks = program(count, bounded)
            source.write_bytes(data)
            result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=60)
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
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (count, result)
            checked += checks
    print(f"dispatch loops: {checked} shared-dispatch state machines agree with the sequential model, wazero and both AOTs")


if __name__ == "__main__":
    main()
