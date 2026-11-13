#!/usr/bin/env python3
"""Run large structured branch tables against an independent interpreter.

    test_large_switches.py <vm> <native-aot> <wazero>
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


def constant(value):
    if value >= 1 << 31:
        value -= 1 << 32
    return b"\x41" + leb(value, True)


def program(count, order):
    targets = [(i if order == "ordered" else (i * 17 + 13) % count) for i in range(count)]
    if order == "repeated":
        targets = [target % 129 for target in targets]
    # Every nested block is a case label. Its continuation returns its
    # index through the outer result block; the table's default is last.
    calculate = b"\x02\x7f" + b"\x02\x40" * count + b"\x20\0\x0e" + leb(count)
    calculate += b"".join(leb(target) for target in targets) + leb(count - 1)
    calculate += b"".join(b"\x0b" + constant(index) + b"\x0c" + leb(count - index - 1) for index in range(count))
    calculate += b"\x0b"
    entry = b""
    indices = (0, 1, 63, 64, 127, 128, 129, 1023, 4095, 8191, count - 2, count - 1, count, count + 1, 0xffffffff)
    for index in indices:
        expected = targets[index] if index < count else count - 1
        entry += constant(index) + b"\x10\0" + constant(expected) + b"\x47\x04\x40\0\x0b"
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\1\x7f\1\x7f\x60\0\0")
    module += section(3, b"\2\0\1") + section(7, b"\1\6_start\0\1")
    bodies = [b"\0" + code + b"\x0b" for code in (calculate, entry)]
    return module + section(10, b"\2" + b"".join(leb(len(code)) + code for code in bodies))


def main():
    vm, native, wazero = sys.argv[1:]
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-large-switches-") as directory:
        work = Path(directory)
        for count in (8193, 16385):
            for order in ("ordered", "permuted", "repeated"):
                data = program(count, order)
                source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
                source.write_bytes(data)
                reference = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=120)
                assert reference.returncode == 0 and reference.stdout == b"", (count, order, reference)
                env = dict(os.environ, W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native], input=data,
                                        capture_output=True, env=env, timeout=180)
                assert result.returncode == 0, (count, order, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=180)
                assert result.returncode == 0, (count, order, result.stderr)
                assert compiled.read_bytes() == embedded.read_bytes(), (count, order)
                result = subprocess.run([vm, "run", "--", str(source)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (count, order, result)
                checked += 1
    print(f"large switches: {checked} ordered/permuted/repeated tables agree with wazero and both AOTs")


if __name__ == "__main__":
    main()
