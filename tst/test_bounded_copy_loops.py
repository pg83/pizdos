#!/usr/bin/env python3
"""Copy with bounded nonlinear addresses beside a moving linear stream.

    test_bounded_copy_loops.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = (1 << 32) - 1
SOURCE, DEST = 1 << 20, 3 << 19
LOW = bytes((i * 37 + 11) % 251 for i in range(65536))
HIGH = bytes((i * 13 + 7) % 251 for i in range(65536))


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


def check(code, value):
    return code + constant(value) + b"\x47\x04\x40\0\x0b"


def address(mode):
    p = get(3) + constant(255) + b"\x71"
    q = get(4) + constant(255) + b"\x71"
    if mode == "mul":
        return constant(3) + p + b"\x6c"
    if mode == "product":
        return p + q + b"\x6c"
    if mode == "difference":
        return constant(512) + p + b"\x6a" + q + b"\x6b"
    for _ in range(27):
        p += constant(1) + b"\x6a"
    return p


def offset(mode, p, factor):
    p &= 255
    if mode == "mul":
        return p * 3
    if mode == "product":
        return p * (factor & 255)
    if mode == "difference":
        return 512 + p - (factor & 255)
    return p + 27


def program(mode, count, start, factor, overlapping):
    destination, source = (8193, 8192) if overlapping else (32768, 8192)
    code = get(2) + b"\x45\x04\x40" + get(3) + b"\x0f\x0b\x03\x40"
    code += constant(DEST) + address(mode) + b"\x6a" + constant(SOURCE) + address(mode) + b"\x6a\x2d\0\0\x3a\0\0"
    code += get(0) + get(3) + b"\x6a" + get(1) + get(3) + b"\x6a\x2d\0\0\x3a\0\0"
    code += get(3) + constant(1) + b"\x6a\x21\3" + get(2) + constant(1) + b"\x6b\x22\2\x0d\0\x0b" + get(3)
    memory = bytearray(32 * 65536)
    memory[4096:65536] = LOW[4096:]
    memory[SOURCE:SOURCE + len(HIGH)] = HIGH
    touched = {4095, 65535, SOURCE - 1, SOURCE + 65536, DEST - 1, DEST + 65536}
    for iteration in range(count):
        p = start + iteration
        at = offset(mode, p, factor)
        memory[DEST + at] = memory[SOURCE + at]
        memory[destination + p] = memory[source + p]
        touched.update((DEST + at, SOURCE + at, destination + p, source + p))
    entry = check(constant(31) + b"\x40\0", 1)
    entry += constant(SOURCE) + constant(0) + constant(len(HIGH)) + b"\xfc\10\1\0\xfc\11\1"
    entry += check(b"".join(constant(value) for value in (destination, source, count, start, factor)) + b"\x10\0", start + count)
    for at in sorted(touched):
        entry += check(constant(at) + b"\x2d\0\0", memory[at])
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\5" + b"\x7f" * 5 + b"\1\x7f\x60\0\0")
    module += section(3, b"\2\0\1") + section(5, b"\1\1\1\x20") + section(7, b"\1\6_start\0\1")
    module += section(12, b"\2")
    bodies = [b"\0" + body + b"\x0b" for body in (code, entry)]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    return module + section(11, b"\2\0" + constant(4096) + b"\x0b" + leb(len(LOW) - 4096) + LOW[4096:] + b"\1" + leb(len(HIGH)) + HIGH)


def main():
    vm, native, wazero = sys.argv[1:]
    cases = [(mode, count, start, factor, overlapping) for mode in ("mul", "product", "difference", "deep")
             for count in (0, 1, 17, 97) for start, factor, overlapping in ((0, 3, False), (250, 17, True))]
    with tempfile.TemporaryDirectory(prefix="pizdos-bounded-copy-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for case in cases:
            data = program(*case)
            source.write_bytes(data)
            result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0 and result.stdout == b"", (case, result)
            for strict in ((False, True) if case[1] == 17 else (False,)):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (case, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (case, result)
                result = subprocess.run([vm, "run", "--", str(source)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (case, strict, result)
    print(f"bounded copy loops: {len(cases)} products, differences and deep expressions agree with a sequential model, wazero and both AOTs")


if __name__ == "__main__":
    main()
