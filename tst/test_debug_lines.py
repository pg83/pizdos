#!/usr/bin/env python3
"""Check trace locations from DWARF line programs carried by Wasm modules.

    test_debug_lines.py <vm> <native-aot>
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def leb(value, signed=False):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(out + bytes([byte]))
        out.append(byte | 128)


def text(value):
    encoded = value.encode()
    return leb(len(encoded)) + encoded


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def custom(name, data):
    return section(0, text(name) + data)


STRINGS = b"/sources\0unused.c\0fixture.c\0"


def table(version, wide=False, path_form=8, index_form=15, define=False, absolute=False):
    # DWARF 4/5, sections 6.2 and 7.22: a row at code offset 67 names line 43.
    width = 8 if wide else 4
    fixed = lambda n: n.to_bytes(width, "little")
    opcode_base = 14
    lengths = bytes([0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 2])
    header = b"\1" + (b"\1" if version >= 4 else b"") + b"\1\xfb\x0e" + bytes([opcode_base]) + lengths
    if version <= 4:
        name = b"/elsewhere/fixture.c" if absolute else b"fixture.c"
        header += b"/sources\0\0unused.c\0\1\0\0"
        if not define:
            header += name + b"\0\1\0\0"
        header += b"\0"
        file_index = 2
    else:
        def string_value(value, offset):
            return value + b"\0" if path_form == 8 else fixed(offset)
        header += b"\1\1" + leb(path_form) + b"\1" + string_value(b"/sources", 0)
        formats = [(1, path_form), (2, index_form), (3, 7), (4, 15), (5, 30), (0x2000, 9)]
        header += bytes([len(formats)]) + b"".join(leb(content) + leb(form) for content, form in formats) + b"\2"
        for name, offset in ((b"unused.c", 9), (b"fixture.c", 18)):
            index = leb(0) if index_form == 15 else bytes({11: 1, 5: 2, 6: 4, 7: 8}[index_form])
            header += string_value(name, offset) + index + bytes(8) + leb(123) + bytes(16) + b"\3abc"
        file_index = 1
    program = b"\5\10\6\7\10\11\0\0\x0a\x0b\x0c\1\x0d\1\2"
    if define:
        payload = b"\3fixture.c\0\1\0\0"
        program += b"\0" + leb(len(payload)) + payload
    program += b"\0\2\x80\0\0\2\4\0"  # Unknown extended opcode and discriminator.
    program += b"\0\5\2" + struct.pack("<I", 3) + b"\4" + leb(file_index)
    program += b"\3" + leb(-9, True) + b"\3" + leb(50, True) + b"\1\2\x40"
    program += bytes([opcode_base + 6]) + b"\2\1\0\1\1"
    body = struct.pack("<H", version) + (b"\4\0" if version >= 5 else b"") + fixed(len(header)) + header + program
    return (b"\xff" * 4 + struct.pack("<Q", len(body)) if wide else struct.pack("<I", len(body))) + body


def module(lines):
    body = b"\0" + b"\1" * 64 + b"\0\x0b"
    wasm = b"\0asm\1\0\0\0" + section(1, b"\1\x60\0\0") + section(3, b"\1\0")
    wasm += section(7, b"\1\6_start\0\0") + section(10, b"\1" + leb(len(body)) + body)
    return wasm + custom(".debug_line_str", STRINGS) + custom(".debug_str", STRINGS) + custom(".debug_line", lines)


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="")
    valid = [(f"v{version}-wide{wide}", table(version, wide)) for version in (2, 3, 4) for wide in (False, True)]
    valid += [(f"v5-{wide}-{path}-{index}", table(5, wide, path, index))
              for wide in (False, True) for path in (8, 31, 14) for index in (11, 5, 6, 7, 15)]
    valid += [("define-file", table(4, define=True)), ("absolute-file", table(4, absolute=True))]
    malformed = [("truncated-" + str(n), table(4)[:n]) for n in range(len(table(4)))]
    for length in (0x80000000, 0xffffffff):
        data = bytearray(table(4))
        struct.pack_into("<I", data, 6, length)
        malformed.append((f"header-length-{length}", bytes(data)))
    original = table(4)
    start = 10 + struct.unpack_from("<I", original, 6)[0]
    for name, program in (("extended-length", b"\0" + leb(1 << 31) + b"\2"),
                          ("address-width", b"\0\x11\2" + bytes(16))):
        data = bytearray(original[:start] + program)
        struct.pack_into("<I", data, 0, len(data) - 4)
        malformed.append((name, bytes(data)))
    data = bytearray(table(5).replace(b"\3abc", leb(1 << 31), 1))
    struct.pack_into("<I", data, 0, len(data) - 4)
    malformed.append(("block-length", bytes(data)))
    with tempfile.TemporaryDirectory(prefix="pizdos-lines-") as directory:
        work = Path(directory)
        source, embedded, compiled = (work / name for name in ("input.wasm", "embedded.o", "native.o"))
        for name, lines in [*valid, *malformed]:
            source.write_bytes(module(lines))
            result = subprocess.run([native], input=source.read_bytes(), env=env, capture_output=True, timeout=30)
            assert result.returncode == 0, (name, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], env=env, capture_output=True, timeout=30)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (name, result)
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], env=env, capture_output=True, timeout=30)
                assert result.returncode == 129 and result.stdout == b"" and b"wasm trap: unreachable" in result.stderr, (name, result)
                if (name, lines) in valid:
                    assert b"fixture.c:43" in result.stderr, (name, result.stderr)
    print(f"debug lines: {len(valid)} source locations and {len(malformed)} malformed tables agree through native/Wasm AOT")


if __name__ == "__main__":
    main()
