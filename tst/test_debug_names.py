#!/usr/bin/env python3
"""Debug names may be Unicode or malformed without changing guest traps.

    test_debug_names.py <vm> <native-aot>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def leb(value):
    result = bytearray()
    while value >= 128:
        result.append(value & 127 | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def names(entries):
    body = leb(len(entries)) + b"".join(leb(index) + text(value) for index, value in entries)
    return section(1, body)


def program(payload, alias=None):
    body = b"\0" + b"\1" * 64 + b"\0\x0b"
    module = b"\0asm\1\0\0\0" + section(1, b"\1\x60\0\0") + section(3, b"\1\0")
    exports = text("_start") + b"\0\0"
    if alias is not None:
        exports += text(alias) + b"\0\0"
    module += section(7, bytes([2 if alias is not None else 1]) + exports)
    module += section(10, b"\1" + leb(len(body)) + body)
    return module + section(0, text("name") + payload)


def main():
    vm, native = sys.argv[1:]
    malformed = [("short-size", b"\1\x80"), ("overflow-size", b"\1\xff\xff\xff\xff\x10"),
                 ("large-size", b"\1" + leb(10000)), ("short-count", b"\1\0"),
                 ("short-index", section(1, b"\1\x80")), ("short-name-size", section(1, b"\1\0\x80")),
                 ("large-name", section(1, b"\1\0\x7f")), ("invalid-UTF8", section(1, b"\1\0\1\xff"))]
    cases = [(label, program(payload), "f0") for label, payload in malformed]
    cases += [("partial-" + label, program(names([(0, "kept")]) + payload), "kept") for label, payload in malformed]
    cases += [("unicode", program(names([(0, "café.λ_🚀")]), alias="café.λ_🚀"), "café.λ_🚀"),
              ("unknown-subsection", program(section(42, b"arbitrary") + names([(0, "named")])), "named"),
              ("unknown-index", program(names([(99999, "ignored")])), "f0"),
              ("last-name-wins", program(names([(0, "old"), (0, "new")])), "new"),
              ("empty-name", program(names([(0, "")])), "f0"),
              ("empty-alias", program(names([(0, "named")]), alias=""), "named"),
              ("controls", program(names([(0, "ctrl\0\n\t\1\x7f")]), alias="alias\0\n\t"), "ctrl_00_0A_09_01_7F"),
              ("percent", program(names([(0, "percent%02X%%")])), "percent%02X%%")]
    long_name = "long-" + "n" * 40000
    cases.append(("cross-page-name", program(section(42, b"padding" * 5000) + names([(0, long_name)])), long_name))
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-debug-names-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        for label, data, name in cases:
            source.write_bytes(data)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (label, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (label, result)
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert result.returncode == 129 and result.stdout == b"" and result.stderr.startswith(b"wasm trap: unreachable\n"), (label, result)
                assert name.encode() + b"+0x" in result.stderr and b"panic:" not in result.stderr, (label, result)
    print(f"debug names: {len(cases)} malformed, partial and Unicode sections preserve traps and expected names with both AOTs")


if __name__ == "__main__":
    main()
