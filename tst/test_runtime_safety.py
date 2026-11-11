#!/usr/bin/env python3
"""Bad public kernel calls must trap in the guest and preserve the Go host.

    test_runtime_safety.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import re
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


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value, typ=0x7f):
    bits = 32 if typ == 0x7f else 64
    value &= (1 << bits) - 1
    if value >= 1 << (bits - 1):
        value -= 1 << bits
    return bytes([0x41 if typ == 0x7f else 0x42]) + leb(value, True)


def program(name, args, *, wide=False, returns=True, expected=None):
    params = bytes(0x7e if wide and i else 0x7f for i in range(len(args)))
    signature = b"\x60" + leb(len(params)) + params + (b"\1\x7f" if returns else b"\0")
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\0\0" + signature)
    module += section(2, b"\1" + text("kernel") + text(name) + b"\0\1")
    module += section(3, b"\1\0") + section(5, b"\1\1\1\1")
    module += section(7, b"\1" + text("_start") + b"\0\1")
    code = b"".join(constant(value, typ) for value, typ in zip(args, params)) + b"\x10\0"
    if expected is None:
        code += (b"\x1a" if returns else b"") + b"\0"
    else:
        code += constant(expected) + b"\x47\x04\x40\0\x0b"
    body = b"\0" + code + b"\x0b"
    module += section(10, b"\1" + leb(len(body)) + body)
    return module + section(11, b"\1\0" + constant(65520) + b"\x0b\x10" + b"A" * 16)


def main():
    vm, native, wazero = sys.argv[1:]
    cases = [("span-overflow", "write", [1, 65535, 2], {}, 139, "out of bounds memory access"),
             ("string-address", "getenv", [65536], {}, 139, "out of bounds memory access"),
             ("unterminated-string", "getenv", [65520], {}, 139, "out of bounds memory access"),
             ("bounded-address", "rawmemchr", [65537, 0], {}, 139, "out of bounds memory access"),
             ("bounded-search", "rawmemchr", [65520, 0], {}, 139, "out of bounds memory access"),
             ("bounded-length", "strnlen", [65520, 17], {}, 139, "out of bounds memory access"),
             ("wide-span", "wmemchr", [65535, 0, 1], {}, 139, "out of bounds memory access"),
             ("wide-address", "wcslen", [65534], {}, 139, "out of bounds memory access"),
             ("wide-length", "wcsnlen", [65532, 2], {}, 139, "out of bounds memory access"),
             ("wide-copy-source", "wmemcpy", [4096, 65535, 1], {}, 139, "out of bounds memory access"),
             ("wide-copy-dest", "wmemcpy", [65535, 4096, 1], {}, 139, "out of bounds memory access")]
    for name in ("div", "ldiv", "lldiv", "imaxdiv"):
        wide = name in ("lldiv", "imaxdiv")
        cases += [(name + "-zero", name, [4096, 1, 0], {"wide": wide, "returns": False}, 136, "integer divide by zero"),
                  (name + "-overflow", name, [4096, -(1 << (63 if wide else 31)), -1],
                   {"wide": wide, "returns": False}, 136, "integer overflow")]
    cases += [("empty-read-bound", "strnlen", [65536, 0], {"expected": 0}, 0, ""),
              ("bounded-early-match", "rawmemchr", [65520, 65], {"expected": 65520}, 0, ""),
              ("wide-empty-span", "wmemchr", [65536, 0, 0], {"expected": 0}, 0, ""),
              ("wide-early-match", "wmemchr", [65520, 0x41414141, 4], {"expected": 65520}, 0, ""),
              ("empty-write-bound", "write", [1, 65536, 0], {"expected": 0}, 0, "")]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-runtime-safety-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for label, name, args, options, status, diagnostic in cases:
            data = program(name, args, **options)
            source.write_bytes(data)
            result = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0, (label, result)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (label, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (label, result)
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "-a", "runtime-safety", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert result.returncode == status and result.stdout == b"" and b"panic:" not in result.stderr, (label, result)
                if status:
                    lines = result.stderr.decode().splitlines()
                    assert diagnostic in lines[0] and len(lines) > 1, (label, lines)
                    assert all(re.match(r"runtime-safety\[\d+\]: ", line) for line in lines), (label, lines)
                else:
                    assert result.stderr == b"", (label, result)
    print(f"runtime safety: {len(cases)} public memory/math calls trap safely or respect empty/early-match bounds with both AOTs")


if __name__ == "__main__":
    main()
