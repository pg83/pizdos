#!/usr/bin/env python3
"""Large global initializers must finish, in order, before the start function.

    test_initializers.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import struct
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


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def integer(value, wide=False):
    bits = 64 if wide else 32
    value &= (1 << bits) - 1
    if value >= 1 << (bits - 1):
        value -= 1 << bits
    return bytes([0x42 if wide else 0x41]) + leb(value, True)


def program(count, start):
    globals, checks = [], b""
    for index in range(count):
        typ = index % 6
        value = (index * 0x9e3779b9 + 7) & 0xffffffff
        code = b"\x23" + leb(index)
        if typ == 0:
            init = integer(value)
            code += integer(value) + b"\x47"
        elif typ == 1:
            value = (value << 32) | (value ^ 0xfeedbeef)
            init = integer(value, True)
            code += integer(value, True) + b"\x52"
        elif typ == 2:
            init = b"\x43" + struct.pack("<I", value)
            code += b"\xbc" + integer(value) + b"\x47"
        elif typ == 3:
            value = (value << 32) | (value ^ 0x12345678)
            init = b"\x44" + struct.pack("<Q", value)
            code += b"\xbd" + integer(value, True) + b"\x52"
        else:
            is_null = typ == 5 or index // 6 % 2 == 0
            init = bytes([0xd0, 0x6f if typ == 5 else 0x70]) if is_null else b"\xd2\0"
            code += b"\xd1" + (b"\x45" if is_null else b"")
        globals.append(bytes([(0x7f, 0x7e, 0x7d, 0x7c, 0x70, 0x6f)[typ], 1]) + init + b"\x0b")
        checks += code + b"\x04\x40\0\x0b"
    # The start function checks the last initializer too, then changes a
    # global. _start checks that mutation after all helper parts returned.
    initializer = checks + integer(0x12345678) + b"\x24\0"
    entry = b"\x23\0" + integer(0x12345678 if start else 7) + b"\x47\x04\x40\0\x0b"
    if not start:
        entry += checks
    module = b"\0asm\1\0\0\0" + section(1, b"\1\x60\0\0") + section(3, b"\2\0\0")
    module += section(6, leb(count) + b"".join(globals))
    module += section(7, b"\2\6_start\0\1\x0binitializer\0\0")
    if start:
        module += section(8, b"\0")
    bodies = [b"\0" + code + b"\x0b" for code in (initializer, entry)]
    return module + section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))


def main():
    vm, native, wazero = sys.argv[1:]
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-initializers-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for count in (127, 128, 129, 257, 1025):
            for start in (False, True):
                data = program(count, start)
                source.write_bytes(data)
                result = subprocess.run([wazero, "-interpreter", str(source)], capture_output=True, timeout=30)
                assert result.returncode == 0 and result.stdout == b"", (count, start, result)
                for strict in (False, True):
                    env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                    result = subprocess.run([native, *(["--strict"] if strict else [])], input=data, capture_output=True, env=env, timeout=60)
                    assert result.returncode == 0, (count, start, strict, result.stderr)
                    compiled.write_bytes(result.stdout)
                    result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                    assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (count, start, strict, result)
                    for obj in (compiled, embedded):
                        result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (count, start, strict, result)
                    checked += 1
    print(f"initializers: {checked} large numeric/reference global tables initialize before start, agreeing with wazero and both AOTs")


if __name__ == "__main__":
    main()
