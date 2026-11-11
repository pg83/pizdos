#!/usr/bin/env python3
"""Return the last indirect offset read by a byte-copy loop.

    test_invariant_results.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

IMAGE = bytes((i * 37 + 11) % 251 if i >= 4096 else 0 for i in range(65536))


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


def constant(value):
    return b"\x41" + leb(value, True)


def get(index):
    return b"\x20" + leb(index)


def check(actual, expected):
    return actual + constant(expected) + b"\x47\x04\x40\0\x0b"


def program(count, pointer, dest, do_while=False, subtract=True):
    source = 8192
    code = get(2) + constant(1) + b"\x72\x21\2" if do_while else get(2) + b"\x45\x04\x40" + constant(0) + b"\x0f\x0b"
    code += b"\x03\x40"
    code += get(3) + b"\x2d\0\0\x21\6"
    code += get(0) + get(4) + b"\x6a" + get(1) + get(5) + b"\x6a" + get(6) + bytes([0x6b if subtract else 0x6a]) + b"\x2d\0\0\x3a\0\0"
    for local in (4, 5):
        code += get(local) + constant(1) + b"\x6a\x21" + leb(local)
    code += get(2) + constant(1) + b"\x6b\x22\2\x0d\0\x0b" + get(6) + b"\x24\0" + get(6) + constant(1) + b"\x6a"
    memory, result = bytearray(IMAGE), 0
    iterations = count | 1 if do_while else count
    touched = {4095, 65535, dest - 1, dest + iterations, pointer}
    for index in range(iterations):
        offset = memory[pointer]
        at, to = source + index + (-offset if subtract else offset), dest + index
        memory[to] = memory[at]
        result = offset + 1
        touched.update((at, to))
    entry = check(b"".join(constant(value) for value in (dest, source, count, pointer)) + b"\x10\0", result)
    entry += check(b"\x23\0", result - 1 if iterations else 1024)
    for at in sorted(touched):
        entry += check(constant(at) + b"\x2d\0\0", memory[at])
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\4" + b"\x7f" * 4 + b"\1\x7f\x60\0\0")
    module += section(3, b"\2\0\1") + section(5, b"\1\1\1\1")
    module += section(6, b"\1\x7f\1" + constant(1024) + b"\x0b") + section(7, b"\1\6_start\0\1")
    bodies = [b"\1\3\x7f" + code + b"\x0b", b"\0" + entry + b"\x0b"]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    return module + section(11, b"\1\0" + constant(4096) + b"\x0b" + leb(len(IMAGE) - 4096) + IMAGE[4096:])


def main():
    vm, native, wazero = sys.argv[1:]
    cases = [(count, pointer, dest, do_while, True) for count in (0, 1, 16, 17, 97)
             for dest in (32768, 8191, 8193) for pointer in (4096, dest, dest + 1, dest + 65, 8192) for do_while in (False, True)]
    cases += [(count, pointer, dest, False, False) for count in (0, 1, 17, 97)
              for dest in (32768, 8191, 8193) for pointer in (4096, dest, dest + 1, dest + 65, 8192)]
    with tempfile.TemporaryDirectory(prefix="pizdos-invariant-results-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for case in cases:
            data = program(*case)
            source.write_bytes(data)
            result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0 and result.stdout == b"", (case, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (case, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (case, result)
                for obj in (compiled, embedded):
                    result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (case, strict, result)
    print(f"invariant results: {len(cases)} indirect, aliasing and overlapping copies preserve their final read with both AOTs and wazero")


if __name__ == "__main__":
    main()
