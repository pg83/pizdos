#!/usr/bin/env python3
"""Check counted byte-copy loops against a sequential memory model.

    test_copy_loops.py <vm> <native-aot>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = (1 << 32) - 1
LOAD_OFFSET, STORE_OFFSET = 17, 23
IMAGE = bytes((i * 37 + 11) % 251 if i >= 4096 else 0 for i in range(65536))


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


def program(width, step, count, dest, source, wrapped, signed, subtract=False, kernel=False):
    arguments = [(dest - STORE_OFFSET) // width, (source - LOAD_OFFSET) // width, count,
                 (dest - STORE_OFFSET) % width, (source - LOAD_OFFSET) % width]
    bias_sign = -1 if subtract else 1
    arguments[3] *= bias_sign
    arguments[4] *= bias_sign
    if wrapped:
        arguments[0] += (1 << 32) // width
        arguments[1] += (1 << 32) // width
    p, q, remaining = 5, 6, 7
    code = get(2) + constant(step) + bytes([0x48 if signed else 0x49]) + b"\x04\x40"
    code += get(0) + get(1) + b"\x6a" + get(2) + b"\x6a\x0f\x0b"
    code += get(0) + set_local(p) + get(1) + set_local(q) + get(2) + set_local(remaining) + b"\x03\x40"

    def address(local, bias):
        return get(local) + constant(width.bit_length() - 1) + b"\x74" + get(bias) + (b"\x6b" if subtract else b"\x6a")

    for byte in range(width):
        code += address(p, 3) + address(q, 4) + b"\x2d\0" + leb(LOAD_OFFSET + byte)
        code += b"\x3a\0" + leb(STORE_OFFSET + byte)
    code += get(p) + constant(1) + b"\x6a" + set_local(p) + get(q) + constant(1) + b"\x6a" + set_local(q)
    code += get(remaining) + constant(step) + b"\x6b\x22" + leb(remaining) + constant(step - 1)
    code += bytes([0x4a if signed else 0x4b]) + b"\x0d\0\x0b"
    code += get(p) + get(q) + b"\x6a" + get(remaining) + b"\x6a"

    # Model the actual byte-at-a-time dependencies, including overlapping
    # source/destination windows, modulo addresses and the unconsumed count.
    iterations_count = count - (1 << 32) if signed and count >= 1 << 31 else count
    iterations = max(0, iterations_count // step)
    rest = count if iterations_count < step else count % step
    image = bytearray(IMAGE)
    locations = {4095, 4096, 8191, 8192, 32767, 32768, 65535}
    for iteration in range(iterations):
        for byte in range(width):
            to = ((arguments[0] + iteration) * width + bias_sign * arguments[3] + STORE_OFFSET + byte) & MASK
            at = ((arguments[1] + iteration) * width + bias_sign * arguments[4] + LOAD_OFFSET + byte) & MASK
            image[to] = image[at]
            locations.update(pos for start in (to, at) for pos in (start - 1, start, start + 1) if 0 <= pos < 65536)
    expected = (arguments[0] + arguments[1] + iterations * 2 + rest) & MASK
    entry = check(b"".join(constant(arg) for arg in arguments) + b"\x10" + leb(int(kernel)), expected)
    for at in sorted(locations):
        entry += check(constant(at) + b"\x2d\0\0", image[at])
    if kernel:
        entry = b"\x10\0\x45\x04\x40\0\x0b" + entry

    types = b"\x60\5" + b"\x7f" * 5 + b"\1\x7f\x60\0\0"
    module = b"\0asm\1\0\0\0" + section(1, bytes([3 if kernel else 2]) + types + (b"\x60\0\1\x7f" if kernel else b""))
    if kernel:
        module += section(2, b"\1\6kernel\6getpid\0\2")
    module += section(3, b"\2\0\1") + section(5, b"\1\1\1\1")
    exports = b"\6_start\0" + leb(1 + int(kernel))
    if kernel:
        module += section(6, b"\2\x7f\0" + constant(65536) + b"\x0b\x7f\1" + constant(32768) + b"\x0b")
        exports += b"\x0b__heap_base\3\0\x0f__stack_pointer\3\1"
    module += section(7, bytes([3 if kernel else 1]) + exports)
    bodies = [b"\1\3\x7f" + code + b"\x0b", b"\0" + entry + b"\x0b"]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    return module + section(11, b"\1\0" + constant(4096) + b"\x0b" + leb(len(IMAGE) - 4096) + IMAGE[4096:])


def main():
    vm, native = sys.argv[1:]
    cases = [(width, step, count, 32768, 8192, False, False)
             for width in (1, 2, 4, 8) for step in (1, 2, 3, 4, 5, 8) for count in (0, 1, 7, 16, 17, 33, 97)]
    cases += [(width, 1, 97, dest, 8192, False, False) for width in (1, 2, 4, 8) for dest in (8191, 8193)]
    cases += [(width, 1, 17, 32768, 8192, True, False) for width in (2, 4, 8)]
    cases += [(4, 1, count, 32768, 8192, False, True) for count in (0, 1, 17, 97, MASK)]
    cases += [(width, step, count, 32768, 8192, False, False, True)
              for width in (2, 4, 8) for step in (1, 2) for count in (17, 97)]
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-copy-loops-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for case in cases:
            modes = ["optimized"]
            if case[2] == 17 or case[3] != 32768 or case[5] or case[6]:
                modes.append("strict")
            if case[0] in (1, 4, 8) and case[1] in (1, 3) and case[2] == 97 and case[3] == 32768 and not case[5] and not case[6]:
                modes.append("asan")
            for mode in modes:
                strict, asan = mode == "strict", mode == "asan"
                data = program(*case, kernel=asan)
                source.write_bytes(data)
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                flag = ["--strict"] if strict else ["--asan"] if asan else []
                result = subprocess.run([native, *flag], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (case, mode, result.stderr)
                compiled.write_bytes(result.stdout)
                run_flags = ["--asan"] if asan else []
                result = subprocess.run([vm, *run_flags, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (case, mode, result)
                result = subprocess.run([vm, "run", *run_flags, "--", str(source)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (case, mode, result)
                checked += 1
    print(f"copy loops: {checked} counted, overlapping, wrapping and ASAN programs agree with the bytewise model and both AOTs")


if __name__ == "__main__":
    main()
