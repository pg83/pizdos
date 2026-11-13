#!/usr/bin/env python3
"""Preserve float selection bits between wrapping memory loads.

    test_interleaved_float_selects.py <vm> <native-aot> <wazero>
"""
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

MASK = (1 << 32) - 1
IMAGE = bytes((i * 13 + 5) % 251 for i in range(65536))


def leb(value, signed=False):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(out + bytes([byte]))
        out.append(byte | 128)


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value, bits=32):
    value &= (1 << bits) - 1
    if value >= 1 << (bits - 1):
        value -= 1 << bits
    return bytes([0x41 if bits == 32 else 0x42]) + leb(value, True)


def floating(value, bits):
    return bytes([0x43 if bits == 32 else 0x44]) + struct.pack("<f" if bits == 32 else "<d", value)


def selected(value, bits):
    return bytes([0x43 if bits == 32 else 0x44]) + value.to_bytes(bits // 8, "little")


def program(compare_bits, select_bits, trapped):
    compare_type, select_type = (0x7d if bits == 32 else 0x7c for bits in (compare_bits, select_bits))
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\5\x7f" + bytes([compare_type] * 2 + [select_type] * 2) + b"\1\x7e\x60\0\0")
    module += section(3, b"\7" + b"\0" * 6 + b"\1") + section(5, b"\1\0\1")
    module += section(7, b"\1\6_start\0\6")
    bodies = []
    for relation in range(6):
        code = b"\x20\0" + constant(2048) + b"\x6a\x31\0\0"
        code += b"\x20\3\x20\4\x20\1\x20\2" + bytes([(0x5b if compare_bits == 32 else 0x61) + relation]) + b"\x1b"
        code += b"\xbc\xad" if select_bits == 32 else b"\xbd"
        code += b"\x7c\x20\0" + constant(2112) + b"\x6a\x31\0\0\x7c"
        bodies.append(code)
    pairs = [(-1.0, 2.0), (2.0, -1.0), (-0.0, 0.0), (math.inf, math.inf),
             (-math.inf, math.inf), (math.nan, 1.0), (1.0, math.nan)]
    patterns = ([0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc12345, 0x3f800000] if select_bits == 32 else
                [0, 0x8000000000000000, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8123456789abc, 0x3ff0000000000000])
    entry = b""
    for base in (1024, 4096, 0xfffffff0, 0xffffffc0):
        for a, b in pairs:
            choices = (a == b, a != b, a < b, a > b, a <= b, a >= b)
            for relation, choice in enumerate(choices):
                yes, no = patterns[relation], patterns[(relation + 1) % len(patterns)]
                expected = (yes if choice else no) + sum(IMAGE[(base + offset) & MASK] for offset in (2048, 2112))
                entry += constant(base) + floating(a, compare_bits) + floating(b, compare_bits)
                entry += selected(yes, select_bits) + selected(no, select_bits) + b"\x10" + leb(relation)
                entry += constant(expected, 64) + b"\x52\x04\x40\0\x0b"
    if trapped:
        entry += constant(65536) + floating(1.0, compare_bits) + floating(2.0, compare_bits)
        entry += selected(patterns[0], select_bits) + selected(patterns[1], select_bits) + b"\x10\0\x1a"
    bodies.append(entry)
    bodies = [b"\0" + code + b"\x0b" for code in bodies]
    module += section(10, leb(len(bodies)) + b"".join(leb(len(body)) + body for body in bodies))
    return module + section(11, b"\1\0" + constant(1024) + b"\x0b" + leb(len(IMAGE) - 1024) + IMAGE[1024:])


def main():
    vm, native, wazero = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pizdos-interleaved-floats-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for compare_bits in (32, 64):
            for select_bits in (32, 64):
                for trapped in (False, True):
                    data = program(compare_bits, select_bits, trapped)
                    source.write_bytes(data)
                    result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=30)
                    assert result.stdout == b"" and (result.returncode != 0 if trapped else result.returncode == 0), result
                    if trapped:
                        assert b"out of bounds memory access" in result.stderr, result
                    for mode, flag in (("optimized", None), ("plain", "--plain"), ("strict", "--strict"), ("profile", "--tail-profile")):
                        env = dict(os.environ, W2X_MEMORY_TAIL="optimized" if mode == "profile" else mode,
                                   W2X_TAIL_PROFILE="1" if mode == "profile" else "")
                        result = subprocess.run([native, *([flag] if flag else [])], input=data, capture_output=True, env=env, timeout=60)
                        assert result.returncode == 0, result.stderr
                        compiled.write_bytes(result.stdout)
                        result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
                        profile = embedded.with_suffix(".jsonl")
                        profile.unlink(missing_ok=True)
                        result = subprocess.run([vm, "run", "--", str(source)], env=dict(env, W2O_TAIL_PROFILE=str(profile)),
                                                capture_output=True, timeout=30)
                        assert result.returncode == (130 if trapped else 0) and result.stdout == b"", (compare_bits, select_bits, mode, result)
                        assert (b"out of bounds memory access" in result.stderr) if trapped else result.stderr == b"", result
                        if mode == "profile":
                            functions = [function for row in profile.read_text().splitlines() for function in json.loads(row)["functions"]]
                            for counter in ("guards", "omitted_checks", *(("fallbacks",) if trapped else ())):
                                assert sum(function[counter] for function in functions) > 0, (counter, functions)
                        else:
                            assert not profile.exists(), mode
    print("interleaved floats: selection bits, wrapping loads and traps agree with a model, wazero and both AOTs in four modes")


if __name__ == "__main__":
    main()
