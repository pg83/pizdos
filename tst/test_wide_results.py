#!/usr/bin/env python3
"""Many-value results survive internal direct and indirect call chains.

    test_wide_results.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

TYPES = {"i32": 0x7f, "i64": 0x7e, "f32": 0x7d, "f64": 0x7c}
EXTEND = {"i32": b"\xad", "i64": b"", "f32": b"\xbc\xad", "f64": b"\xbd"}
MASK = (1 << 64) - 1


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


def integer(value, bits=64):
    value &= (1 << bits) - 1
    return bytes([0x41 if bits == 32 else 0x42]) + leb(value - (1 << bits) if value >= 1 << (bits - 1) else value, True)


def argument(kind, index):
    if kind[0] == "i":
        bits = 32 if kind == "i32" else 64
        raw = (index * 0x9e3779b185ebca87 + 19) & ((1 << bits) - 1)
        return integer(raw, bits), raw
    bits = 32 if kind == "f32" else 64
    special = ([0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00123] if bits == 32 else
               [0, 0x8000000000000000, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000123])
    raw = special[index % 7] if index % 7 < len(special) else int.from_bytes(struct.pack("<f" if bits == 32 else "<d", (index * 13 % 61 - 30) / 4), "little")
    return bytes([0x43 if bits == 32 else 0x44]) + raw.to_bytes(bits // 8, "little"), raw


def program(kinds, indirect):
    cookie = 0x1122334455667788
    results = list(reversed(kinds)) + ["i64"]
    signature = b"\x60" + leb(len(kinds)) + bytes(TYPES[kind] for kind in kinds)
    signature += leb(len(results)) + bytes(TYPES[kind] for kind in results)
    module = b"\0asm\1\0\0\0" + section(1, b"\2" + signature + b"\x60\0\0") + section(3, b"\3\0\0\1")
    if indirect:
        module += section(4, b"\1\x70\0\1")
    module += section(6, b"\1\x7e\0" + integer(cookie) + b"\x0b") + section(7, b"\1\6_start\0\2")
    if indirect:
        module += section(9, b"\1\0\x41\0\x0b\1\1")
    producer = b"\0" + b"".join(b"\x20" + leb(i) for i in reversed(range(len(kinds)))) + b"\x23\0\x0b"
    relay = b"\0" + b"".join(b"\x20" + leb(i) for i in range(len(kinds))) + b"\x10\0\x0b"
    entry = b""
    for sample in (0, 1, 3):
        arguments = [argument(kind, i + sample) for i, kind in enumerate(kinds)]
        bits = list(reversed([raw for _, raw in arguments])) + [cookie]
        expected = 0
        entry += b"".join(code for code, _ in arguments) + (b"\x41\0\x11\0\0" if indirect else b"\x10\1")
        entry += b"".join(b"\x21" + leb(i) for i in reversed(range(len(results)))) + integer(0)
        for i, (kind, raw) in enumerate(zip(results, bits)):
            expected = ((expected * 33) ^ raw) & MASK
            entry += integer(33) + b"\x7e\x20" + leb(i) + EXTEND[kind] + b"\x85"
        entry += integer(expected) + b"\x52\x04\x40\0\x0b"
    entry = leb(len(results)) + b"".join(b"\1" + bytes([TYPES[kind]]) for kind in results) + entry + b"\x0b"
    bodies = (producer, relay, entry)
    return module + section(10, b"\3" + b"".join(leb(len(body)) + body for body in bodies))


def main():
    vm, native, wazero = sys.argv[1:]
    patterns = [(kind,) for kind in TYPES] + [tuple(TYPES)]
    cases = [(pattern, count, indirect) for pattern in patterns for count in (0, 6, 7, 15, 16, 17, 31, 32, 40, 64)
             for indirect in (False, True)]
    with tempfile.TemporaryDirectory(prefix="pizdos-wide-results-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for pattern, count, indirect in cases:
            kinds = [pattern[i % len(pattern)] for i in range(count)]
            data = program(kinds, indirect)
            source.write_bytes(data)
            result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0 and result.stdout == b"", (pattern, count, indirect, result)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (pattern, count, indirect, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (pattern, count, indirect, result)
                for obj in (compiled, embedded):
                    result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (pattern, count, indirect, strict, result)
    print(f"wide results: {len(cases)} direct/indirect call chains preserve up to 65 mixed results with a model, wazero and both AOTs")


if __name__ == "__main__":
    main()
