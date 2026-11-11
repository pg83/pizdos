#!/usr/bin/env python3
"""Check cross-module arguments/results through the public spec command.

    test_import_abi.py <vm> <native-aot>

A provider returns its arguments in reverse order and its own global. The
consumer hashes their bits; Python supplies the independent expected hash.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

TYPES = {"i32": 0x7f, "i64": 0x7e, "f32": 0x7d, "f64": 0x7c}
FORMATS = {"i32": "I", "i64": "Q", "f32": "f", "f64": "d"}
EXTEND = {"i32": b"\xad", "i64": b"", "f32": b"\xbc\xad", "f64": b"\xbd"}


def leb(value, signed=False):
    result = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(result + bytes([byte]))
        result.append(byte | 128)


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(kind, value):
    if kind[0] == "f":
        return bytes([0x43 if kind == "f32" else 0x44]) + struct.pack("<" + FORMATS[kind], value)
    width = 32 if kind == "i32" else 64
    value &= (1 << width) - 1
    if value >= 1 << (width - 1):
        value -= 1 << width
    return bytes([0x41 if width == 32 else 0x42]) + leb(value, True)


def value(kind, index):
    if kind[0] == "i":
        width = 32 if kind == "i32" else 64
        return (index * 0x9e3779b185ebca87 + 19) & ((1 << width) - 1)
    special = index % 7
    if special < 3:
        return (-0.0, float("inf"), -float("inf"))[special]
    if special == 3:
        bits = 0x7fc00123 if kind == "f32" else 0x7ff8000000000123
        return struct.unpack("<" + FORMATS[kind], bits.to_bytes(4 if kind == "f32" else 8, "little"))[0]
    return ((index * 13) % 61 - 30) / 4


def modules(kinds, cookie):
    results = list(reversed(kinds)) + ["i64"]
    signature = b"\x60" + leb(len(kinds)) + bytes(TYPES[k] for k in kinds)
    signature += leb(len(results)) + bytes(TYPES[k] for k in results)
    provider = b"\0asm\1\0\0\0" + section(1, b"\1" + signature) + section(3, b"\1\0")
    provider += section(6, b"\1\x7e\0" + constant("i64", cookie) + b"\x0b")
    provider += section(7, b"\1" + text("transform") + b"\0\0")
    body = b"\0" + b"".join(b"\x20" + leb(i) for i in reversed(range(len(kinds)))) + b"\x23\0\x0b"
    provider += section(10, b"\1" + leb(len(body)) + body)

    values = [value(kind, index) for index, kind in enumerate(kinds)]
    bits = [int.from_bytes(struct.pack("<" + FORMATS[kind], v), "little") for kind, v in zip(kinds, values)]
    bits = list(reversed(bits)) + [cookie]
    code = b"".join(constant(kind, v) for kind, v in zip(kinds, values)) + b"\x10\0"
    code += b"".join(b"\x21" + leb(i) for i in reversed(range(len(results)))) + constant("i64", 0)
    expected = 0
    for index, (kind, raw) in enumerate(zip(results, bits)):
        expected = ((expected * 33) ^ raw) & ((1 << 64) - 1)
        code += constant("i64", 33) + b"\x7e\x20" + leb(index) + EXTEND[kind] + b"\x85"
    consumer = b"\0asm\1\0\0\0" + section(1, b"\2" + signature + b"\x60\0\1\x7e")
    consumer += section(2, b"\1" + text("provider") + text("transform") + b"\0\0") + section(3, b"\1\1")
    consumer += section(7, b"\1" + text("run") + b"\0\1")
    body = leb(len(results)) + b"".join(b"\1" + bytes([TYPES[kind]]) for kind in results) + code + b"\x0b"
    consumer += section(10, b"\1" + leb(len(body)) + body)
    return provider, consumer, expected


def main():
    vm, native = sys.argv[1:]
    patterns = [(kind,) for kind in TYPES] + [tuple(TYPES)]
    cases = [(pattern, count) for pattern in patterns for count in (1, 8, 9, 10, 15, 16, 31, 32, 40)]
    cases.append((tuple(TYPES), 0))
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-import-abi-") as directory:
        work = Path(directory)
        for index, (pattern, count) in enumerate(cases):
            kinds = [pattern[i % len(pattern)] for i in range(count)]
            provider, consumer, expected = modules(kinds, 0x1122334400000000 + index * 0x010101)
            for name, data in (("provider", provider), ("consumer", consumer)):
                source, embedded = work / (name + ".wasm"), work / "embedded.o"
                source.write_bytes(data)
                result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (pattern, count, name, result.stderr)
                reference = result.stdout
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == reference, (pattern, count, name, result)
            commands = [{"type": "module", "name": "source", "filename": "provider.wasm"},
                        {"type": "register", "name": "source", "as": "provider"},
                        {"type": "module", "name": "sink", "filename": "consumer.wasm"}]
            commands += [{"type": "assert_return", "action": {"type": "invoke", "module": "sink", "field": "run", "args": []},
                          "expected": [{"type": "i64", "value": str(expected)}]}] * 3
            document = work / "commands.json"
            document.write_text(json.dumps({"commands": commands}))
            result = subprocess.run([vm, "spec", str(document)], capture_output=True, env=env, timeout=60)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"RESULT 5 0 0\n", b""), (pattern, count, result)
    print(f"import ABI: {len(cases)} cross-module signatures preserve argument/result bits, context and stack over repeated calls")


if __name__ == "__main__":
    main()
