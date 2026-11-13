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

# A fixed McIlroy adversarial ordering of disjoint line sequences.
ROW_ORDER = (
    5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0,
    3, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 1,
    4, 112, 86, 67, 92, 120, 99, 124, 100, 74, 89, 118, 78, 113, 111, 96,
    95, 125, 114, 70, 116, 102, 73, 68, 77, 81, 119, 127, 82, 85, 123, 2,
    66, 71, 91, 122, 94, 103, 83, 115, 88, 126, 87, 69, 97, 117, 110, 93,
    72, 76, 80, 84, 90, 75, 98, 101, 105, 121, 79, 104, 109, 108, 107, 106,
)


def table(version, wide=False, path_form=8, index_form=15, define=False, absolute=False, empty_directory=False):
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
        header += b"\1\1" + leb(path_form) + b"\1" + string_value(b"" if empty_directory else b"/sources", 8 if empty_directory else 0)
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


def reordered_table(order):
    original = table(4)
    start = 10 + struct.unpack_from("<I", original, 6)[0]
    program = bytearray()
    last = max(index for index, rank in enumerate(order) if rank == 8)
    for index, rank in enumerate(order):
        # The sequence at 64 covers the trap's PC 67. Repeated rows must
        # keep the last source location written for that address.
        line = (43 if index == last else 17) if rank == 8 else rank + 100
        program += b"\0\5\2" + struct.pack("<I", rank * 8) + b"\4\2\3" + leb(line - 1, True)
        program += b"\1\2\4\0\1\1"
    result = bytearray(original[:start] + program)
    struct.pack_into("<I", result, 0, len(result) - 4)
    return result


def module(lines):
    body = b"\0" + b"\1" * 64 + b"\0\x0b"
    wasm = b"\0asm\1\0\0\0" + section(1, b"\1\x60\0\0") + section(3, b"\1\0")
    wasm += section(7, b"\1\6_start\0\0") + section(10, b"\1" + leb(len(body)) + body)
    return wasm + custom(".debug_line_str", STRINGS) + custom(".debug_str", STRINGS) + custom(".debug_line", lines)


def located_table(rows, end):
    # DWARF 4: each row's code offset and those after it, up to the next row,
    # name the row's line of fixture.c; the sequence ends at end.
    lengths = bytes([0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 2])
    header = b"\1\1\1\xfb\x0e\x0e" + lengths + b"/sources\0\0fixture.c\0\1\0\0\0"
    program, line = b"", 1
    for address, row in rows:
        program += b"\0\5\2" + struct.pack("<I", address) + b"\3" + leb(row - line, True) + b"\1"
        line = row
    program += b"\0\5\2" + struct.pack("<I", end) + b"\0\1\1"
    body = struct.pack("<H", 4) + struct.pack("<I", len(header)) + header + program
    return struct.pack("<I", len(body)) + body


def located(parts):
    """A module whose _start runs parts, (code, line) pairs, with a memory of
    a page and a global holding an address far past it, and the lines of the
    parts in its line table."""
    body = b"\1\2\x7f"
    rows = []
    for code, line in parts:
        # Code offsets count from the code section's contents: its function
        # count and body size, one byte each here, come first.
        rows.append((2 + len(body), line))
        body += code
    body += b"\x0b"
    assert len(body) < 128
    wasm = b"\0asm\1\0\0\0" + section(1, b"\1\x60\0\0") + section(3, b"\1\0") + section(5, b"\1\0\1")
    wasm += section(6, b"\1\x7f\1\x41" + leb(0x7ffffff0, True) + b"\x0b") + section(7, b"\1\6_start\0\0")
    wasm += section(10, b"\1" + leb(len(body)) + body)
    return wasm + custom(".debug_line", located_table(rows, 2 + len(body)))


# Traps at an access report its line: one past a loop, whose unchanged
# local's phi the frontend drops and renumbers the values after, and one of
# a load folded into a branch, after a store of an earlier line. The code's
# lines, in perf's jitdump, have the access's and the branch's.
LOCATED = (
    ("after-loop", located([(b"\x41\3\x21\0", 10), (b"\x23\0\x21\1", 11), (b"\x03\x40\x20\1\x1a", 12),
                            (b"\x20\0\x41\1\x6b\x22\0", 20), (b"\x0d\0\x0b", 21), (b"\x20\1", 29),
                            (b"\x28\2\0", 30), (b"\x1a", 31)]), 30, 21),
    ("folded-load", located([(b"\x41\x10\x41\1\x3a\0\0", 40), (b"\x02\x40\x23\0", 49), (b"\x28\2\0", 50),
                             (b"\x41\7\x47\x0d\0", 51), (b"\x41\x14\x41\2\x3a\0\0\x0b", 52)]), 50, 51),
)


def jitdump_lines(path):
    """The source lines a jitdump gives code addresses (its debug info)."""
    data = path.read_bytes()
    lines, at = set(), struct.unpack_from("<I", data, 8)[0]
    while at < len(data):
        kind, total = struct.unpack_from("<2I", data, at)
        if kind == 2:
            entry, count = at + 32, struct.unpack_from("<Q", data, at + 24)[0]
            for _ in range(count):
                lines.add(struct.unpack_from("<I", data, entry + 8)[0])
                entry = data.index(b"\0", entry + 16) + 1
        at += total
    return lines


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="")
    valid = [(f"v{version}-wide{wide}", table(version, wide)) for version in (2, 3, 4) for wide in (False, True)]
    valid += [(f"v5-{wide}-{path}-{index}", table(5, wide, path, index))
              for wide in (False, True) for path in (8, 31, 14) for index in (11, 5, 6, 7, 15)]
    valid.append(("empty-directory", table(5, empty_directory=True)))
    valid += [("define-file", table(4, define=True)), ("absolute-file", table(4, absolute=True))]
    valid += [("adversarial-rows", reordered_table(ROW_ORDER)),
              ("reversed-rows", reordered_table(tuple(reversed(ROW_ORDER)))),
              ("duplicate-rows", reordered_table(ROW_ORDER + ROW_ORDER[64:96] + (8,)))]
    malformed = [("truncated-" + str(n), table(4)[:n]) for n in range(len(table(4)))]
    for version in (0, 1, 6, 0xffff):
        data = bytearray(table(4))
        struct.pack_into("<H", data, 4, version)
        malformed.append((f"unsupported-version-{version}", bytes(data)))
    for length in (0x80000000, 0xffffffff):
        data = bytearray(table(4))
        struct.pack_into("<I", data, 6, length)
        malformed.append((f"header-length-{length}", bytes(data)))
    malformed.append(("unknown-entry-form", table(5, path_form=0x1234)))
    unfinished = bytearray(table(4))
    unfinished = unfinished[:unfinished.index(b"/sources") + len(b"/sources")]
    struct.pack_into("<I", unfinished, 0, len(unfinished) - 4)
    struct.pack_into("<I", unfinished, 6, len(unfinished) - 10)
    malformed.append(("unterminated-directory", bytes(unfinished)))
    outside = table(5, path_form=31)
    marker = b"\1\1\x1f\1" + bytes(4)
    assert marker in outside
    outside = outside.replace(marker, marker[:4] + struct.pack("<I", 0xffffffff), 1)
    malformed.append(("string-offset-outside-section", outside))
    original = table(4)
    start = 10 + struct.unpack_from("<I", original, 6)[0]
    empty = bytearray(original[:start] + b"\0\0" + original[start:])
    struct.pack_into("<I", empty, 0, len(empty) - 4)
    malformed.append(("zero-length-extended-opcode", bytes(empty)))
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
            result = subprocess.run([vm, "run", "--", str(source)], env=env, capture_output=True, timeout=30)
            assert result.returncode == 129 and result.stdout == b"" and b"wasm trap: unreachable" in result.stderr, (name, result)
            if (name, lines) in valid:
                assert b"fixture.c:43" in result.stderr, (name, result.stderr)
        for name, wasm, line, branch in LOCATED:
            source.write_bytes(wasm)
            result = subprocess.run([native], input=wasm, env=env, capture_output=True, timeout=30)
            assert result.returncode == 0, (name, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], env=env, capture_output=True, timeout=30)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (name, result)
            child = subprocess.Popen([vm, "run", "--", str(source)], env=dict(env, W2O_PERFMAP=str(work)),
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            out, err = child.communicate(timeout=30)
            assert child.returncode == 130 and out == b"" and b"out of bounds memory access" in err, (name, child.returncode, err)
            assert f"fixture.c:{line}\n".encode() in err, (name, err)
            dump = work / f"jit-{child.pid}.dump"
            lines = jitdump_lines(dump)
            assert {line, branch} <= lines, (name, lines)
            dump.unlink()
            (work / f"perf-{child.pid}.map").unlink()
    print(f"debug lines: {len(valid)} source locations, {len(malformed)} malformed tables and {len(LOCATED)} trapping accesses agree through native/Wasm AOT")


if __name__ == "__main__":
    main()
