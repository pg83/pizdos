#!/usr/bin/env python3
"""Copy loops with inclusive limits, variable steps and biased counters.

    test_loop_limits.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = (1 << 32) - 1
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
    value &= MASK
    return b"\x41" + leb(value - (1 << 32) if value >= 1 << 31 else value, True)


def get(index):
    return b"\x20" + leb(index)


def check(actual, expected):
    return actual + constant(expected) + b"\x47\x04\x40\0\x0b"


def program(kind, variable, start, limit, stride, overlapping):
    dest, source = (8193, 8192) if overlapping else (32768, 8192)
    biased = kind.startswith("biased")
    stop = 1 if kind == "biased-limit" else 0
    if biased:
        condition = get(2) + get(3) + b"\x6a" + constant(stop) + b"\x4b"
    else:
        condition = get(2) + get(3) + (b"\x4d" if kind == "inclusive" else b"\x49")
    code = condition + b"\x45\x04\x40" + get(2) + b"\x0f\x0b\x03\x40"
    index = 5 if biased else 2
    for byte in range(2):
        code += get(0) + get(index) + b"\x6a" + get(1) + get(index) + b"\x6a\x2d\0" + leb(byte) + b"\x3a\0" + leb(byte)
    if biased:
        code += get(5) + constant(1) + b"\x6a\x21\5" + get(2) + constant(1) + b"\x6b\x21\2"
    else:
        code += get(2) + (get(4) if variable else constant(stride)) + b"\x6a\x21\2"
    code += condition + b"\x0d\0\x0b" + get(2)
    image = bytearray(IMAGE)
    touched = {dest - 1, source - 1, 65535}
    position, index = start, 0 if biased else start

    def continues(position):
        if biased:
            return ((position + limit) & MASK) > stop
        return position <= limit if kind == "inclusive" else position < limit

    while continues(position):
        for byte in range(2):
            image[dest + index + byte] = image[source + index + byte]
            touched.update((dest + index + byte, source + index + byte))
        position = (position - 1) & MASK if biased else position + stride
        index = index + 1 if biased else position
        assert len(touched) < 1000, (kind, start, limit, stride)
    entry = check(b"".join(constant(value) for value in (dest, source, start, limit, stride)) + b"\x10\0", position)
    for at in sorted(touched):
        entry += check(constant(at) + b"\x2d\0\0", image[at])
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\5" + b"\x7f" * 5 + b"\1\x7f\x60\0\0")
    module += section(3, b"\2\0\1") + section(5, b"\1\1\1\1") + section(7, b"\1\6_start\0\1")
    bodies = [b"\1\1\x7f" + code + b"\x0b", b"\0" + entry + b"\x0b"]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    return module + section(11, b"\1\0" + constant(4096) + b"\x0b" + leb(len(IMAGE) - 4096) + IMAGE[4096:])


def main():
    vm, native, wazero = sys.argv[1:]
    cases = [(kind, variable, start, start + distance, stride, overlapping)
             for kind in ("exclusive", "inclusive") for variable, stride in ((False, 1), (False, 3), (True, 3), (True, 65537))
             for start, overlapping in ((1, False), (250, True)) for distance in (-1, 0, 1, 17, 97)]
    cases += [(kind, False, (count + stop - bias) & MASK, bias, 1, overlapping)
              for kind, stop in (("biased-zero", 0), ("biased-limit", 1))
              for count in (0, 1, 17, 97) for bias in (1, 31, 0xfffffff0) for overlapping in (False, True)]
    with tempfile.TemporaryDirectory(prefix="pizdos-loop-limits-") as directory:
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
    print(f"loop limits: {len(cases)} inclusive, variable-step and biased copies agree with a bytewise model, wazero and both AOTs")


if __name__ == "__main__":
    main()
