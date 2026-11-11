#!/usr/bin/env python3
"""Check ASAN around static data, unused memory and the linear-memory end.

    test_asan_bounds.py <vm> <native-aot> <wazero>
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


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value, wide=False):
    return bytes([0x42 if wide else 0x41]) + leb(value, True)


def program(address, width, store, host=False):
    signature = b"\x60\3\x7f\x7f\x7f\1\x7f" if host else b"\x60\0\1\x7f"
    name = "write" if host else "getpid"
    module = b"\0asm\1\0\0\0" + section(1, b"\2" + signature + b"\x60\0\0")
    module += section(2, b"\1" + text("kernel") + text(name) + b"\0\0") + section(3, b"\1\1")
    module += section(5, b"\1\1\1\2")
    module += section(6, b"\1\x7f\0" + constant(32768) + b"\x0b")
    module += section(7, b"\2" + text("_start") + b"\0\1" + text("__heap_base") + b"\3\0")
    if host:
        # The kernel checks the whole range before writing any bytes.
        # Empty spans at the exact end remain valid.
        code = constant(1) + constant(address) + constant(width) + b"\x10\0\x1a"
    else:
        code = b"\x10\0\x1a" + constant(address)
        if store:
            code += constant(1, width == 8) + bytes([{1: 0x3a, 2: 0x3b, 4: 0x36, 8: 0x37}[width], 0, 0])
        else:
            code += bytes([{1: 0x2d, 2: 0x2f, 4: 0x28, 8: 0x29}[width], 0, 0])
            code += constant(0, width == 8) + bytes([0x52 if width == 8 else 0x47]) + b"\x04\x40\0\x0b"
    body = b"\0" + code + b"\x0b"
    return module + section(10, b"\1" + leb(len(body)) + body)


def main():
    vm, native, wazero = sys.argv[1:]
    cases = []
    for address, diagnostic in ((1024, ""), (32760, ""), (1023, "in the first KiB"),
                                (32768, "in memory never allocated"), (65536, "past the memory's end 0x10000")):
        for width in (1, 2, 4, 8):
            for store in (False, True):
                cases.append((f"scalar-{address}-{width}-{store}", program(address, width, store), 139 if diagnostic else 0, diagnostic))
    for width in (2, 4, 8):
        for store in (False, True):
            cases.append((f"partial-{width}-{store}", program(32767, width, store), 139,
                          "whose byte 0x8000 is in memory never allocated"))
    cases += [("kernel-range", program(32766, 4, False, host=True), 134, "access of a range at 0x8000: in memory never allocated"),
              ("empty-end-range", program(65536, 0, False, host=True), 0, "")]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-asan-bounds-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for label, data, status, diagnostic in cases:
            source.write_bytes(data)
            result = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0, (label, result)
            result = subprocess.run([native, "--asan"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (label, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "--asan", "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (label, result)
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--asan", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert result.returncode == status and result.stdout == b"" and diagnostic.encode() in result.stderr, (label, result)
                assert b"panic:" not in result.stderr, (label, result)
                if not status:
                    assert result.stderr == b"", (label, result)
    print(f"asan bounds: {len(cases)} scalar and kernel ranges enforce static/unused/end boundaries with both AOTs")


if __name__ == "__main__":
    main()
