#!/usr/bin/env python3
"""Check repeated indirect reads and invariant switches in memory loops.

    test_invariant_loops.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = (1 << 32) - 1
IMAGE = bytes((i * 37 + 11) % 251 if i >= 4096 else 0 for i in range(65536))
DEST, SOURCE = 32768, 8192


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
    if value >= 1 << 31:
        value -= 1 << 32
    return b"\x41" + leb(value, True)


def get(index):
    return b"\x20" + leb(index)


def set_local(index):
    return b"\x21" + leb(index)


def check(actual, expected):
    return actual + constant(expected) + b"\x47\x04\x40\0\x0b"


def switch(byte):
    def store(case):
        # Reload the offset for each write. A preceding write may have
        # changed that byte, including between the two switches.
        return (get(5) + get(6) + get(3) + b"\x2d\0\0\x6a\x2d\0" + leb(byte)
                + constant(case + 1 + byte * 10) + b"\x6a\x3a\0" + leb(byte))

    code = b"\x02\x40" * 6 + get(4) + b"\x0e\4\0\1\2\3\4"
    for case in range(4):
        code += b"\x0b" + store(case) + b"\x0c" + leb(4 - case)
    return code + b"\x0b" + store(4) + b"\x0b"


def program(count, pointer, mode):
    p, q, n = 5, 6, 7
    code = get(2) + b"\x45\x04\x40" + get(0) + get(1) + b"\x6a\x0f\x0b"
    code += get(0) + set_local(p) + get(1) + set_local(q) + get(2) + set_local(n) + b"\x03\x40"
    code += switch(0) + switch(1)
    for local in (p, q):
        code += get(local) + constant(2) + b"\x6a" + set_local(local)
    code += get(n) + constant(1) + b"\x6b\x22" + leb(n) + b"\x0d\0\x0b"
    code += get(p) + get(q) + b"\x6a"

    image = bytearray(IMAGE)
    if pointer < len(image):
        image[pointer] = 2
    initial = bytes(image[4096:])
    trapping = count > 0 and pointer >= len(image)
    touched = {4095, DEST - 1, DEST + count * 2, 65535}
    if pointer < len(image):
        touched.add(pointer)
    if not trapping:
        for iteration in range(count):
            for byte in range(2):
                to = DEST + iteration * 2 + byte
                at = SOURCE + iteration * 2 + image[pointer] + byte
                image[to] = (image[at] + min(mode, 4) + 1 + byte * 10) & 255
                touched.update((to, at))
    call = b"".join(constant(x) for x in (DEST, SOURCE, count, pointer, mode)) + b"\x10\0"
    if trapping:
        entry = call + b"\x1a\0"
    else:
        entry = check(call, DEST + SOURCE + count * 4)
        for at in sorted(touched):
            entry += check(constant(at) + b"\x2d\0\0", image[at])

    # With no imports, a compiler can move repeated memory reads out of
    # the loop only after proving they cannot overlap any of its writes.
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\5" + b"\x7f" * 5 + b"\1\x7f\x60\0\0")
    module += section(3, b"\2\0\1") + section(5, b"\1\1\1\1") + section(7, b"\1\6_start\0\1")
    bodies = [b"\1\3\x7f" + code + b"\x0b", b"\0" + entry + b"\x0b"]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    return module + section(11, b"\1\0" + constant(4096) + b"\x0b" + leb(len(initial)) + initial), trapping


def main():
    vm, native, wazero = sys.argv[1:]
    cases = [(count, 4096, mode) for count in (0, 1, 15, 16, 17, 33, 97) for mode in (0, 1, 3, 4, MASK)]
    cases += [(count, pointer, mode) for count in (1, 17, 33) for pointer in (DEST, DEST + 1, DEST + 65, 65535) for mode in (0, 3)]
    cases += [(count, 65537, 0) for count in (0, 1, 33)]
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-invariant-loops-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for case in cases:
            data, trapping = program(*case)
            source.write_bytes(data)
            reference = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=30)
            if trapping:
                assert reference.returncode != 0 and b"out of bounds memory access" in reference.stderr, (case, reference)
            else:
                assert reference.returncode == 0 and reference.stdout == b"", (case, reference)
            for strict in ((False, True) if case[0] == 17 or case[1] != 4096 else (False,)):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (case, strict, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (case, strict, result)
                result = subprocess.run([vm, "run", "--", str(source)], capture_output=True, env=env, timeout=30)
                assert result.returncode == (130 if trapping else 0) and result.stdout == b"", (case, strict, result)
                assert b"out of bounds memory access" in result.stderr if trapping else result.stderr == b"", (case, strict, result)
                checked += 1
    print(f"invariant loops: {checked} indirect/aliased reads and repeated switches agree with the sequential model, wazero and both AOTs")


if __name__ == "__main__":
    main()
