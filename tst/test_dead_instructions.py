#!/usr/bin/env python3
"""Parse valid dead instructions and reject bad encodings through the CLI.

    test_dead_instructions.py <vm> <native-aot> <wazero>
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


def program(instructions):
    module = b"\0asm\1\0\0\0" + section(1, b"\1\x60\0\0") + section(3, b"\2\0\0")
    module += section(4, b"\1\x70\1\2\4") + section(5, b"\1\1\1\2")
    module += section(6, b"\1\x7f\1\x41\x80\x20\x0b") + section(7, b"\1\6_start\0\0")
    # ref.func 1 is declared by the element segment; bulk-memory operands
    # refer to existing passive data and a funcref table.
    module += section(9, b"\1\3\0\1\1") + section(12, b"\1")
    bodies = [b"\1\1\x7f\0" + instructions + b"\x0b", b"\0\x0b"]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    return module + section(11, b"\1\1\3abc")


def groups():
    yield "selects", b"".join(b"\x1c\1" + bytes([typ]) + b"\x1a" for typ in (0x7f, 0x7e, 0x7d, 0x7c, 0x70, 0x6f))
    yield "references", b"\xd0\x70\x1a\xd0\x6f\x1a\xd2\1\x1a\xd1\x1a"
    bulk = b""
    for sub in range(18):
        operands = b"\0\0" if sub in (8, 10, 12, 14) else b"\0" if sub >= 9 else b""
        bulk += b"\xfc" + leb(sub) + operands
        if sub < 8 or sub in (15, 16):
            bulk += b"\x1a"
    yield "bulk-memory-and-tables", bulk
    memory = b"".join(bytes([op, 0]) + leb(32768) + (b"\x1a" if op < 54 else b"") for op in range(40, 63))
    memory += b"\x3f\0\x1a\x40\0\x1a"
    yield "memory-operands", memory
    yield "locals-globals-calls", b"\x20\0\x1a\x21\0\x22\0\x1a\x23\0\x1a\x24\0\x25\0\x1a\x26\0\x10\1\x11\0\0"
    yield "constants", b"\x41" + leb(-12345, True) + b"\x1a\x42" + leb(-987654321, True) + b"\x1a\x43" + struct.pack("<f", 2.5) + b"\x1a\x44" + struct.pack("<d", -0.0) + b"\x1a"
    yield "numeric", b"".join(bytes([op, 0x1a]) for op in range(69, 197))
    yield "nested-control", b"\x02\x40\0\x03\x40\0\x02\x40\0\x0d\0\x0e\3\0\1\2\2\x0c\0\x0b\x0b\x04\x40\0\x05\0\x0b\x0b"


def main():
    vm, native, wazero = sys.argv[1:]
    valid = list(groups())
    valid.append(("combined", b"".join(body for _, body in valid)))
    malformed = [("unknown-bulk-opcode", b"\xfc\x12", b"illegal opcode 0xfc"),
                 ("unknown-opcode", b"\xff", b"illegal opcode"),
                 ("bad-select-type", b"\x1c\1\0", b"value type")]
    with tempfile.TemporaryDirectory(prefix="pizdos-dead-instructions-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for name, body in valid:
            source.write_bytes(program(body))
            result = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=60)
            assert result.returncode == 0, (name, result.stderr)
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=source.read_bytes(), capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, (name, result.stderr)
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (name, result)
                for obj in (compiled, embedded):
                    result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                    assert result.returncode == 129 and result.stdout == b"" and b"wasm trap: unreachable" in result.stderr, (name, result)
        for name, body, diagnostic in malformed:
            source.write_bytes(program(body))
            result = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=60)
            assert result.returncode != 0, name
            result = subprocess.run([native], input=source.read_bytes(), capture_output=True, timeout=60)
            assert result.returncode != 0 and diagnostic in result.stderr, (name, result)
            embedded.write_bytes(b"previous object")
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, timeout=60)
            assert result.returncode == 1 and diagnostic in result.stderr and embedded.read_bytes() == b"previous object", (name, result)
    print(f"dead instructions: {len(valid)} valid groups and {len(malformed)} malformed encodings checked with wazero and both AOTs")


if __name__ == "__main__":
    main()
