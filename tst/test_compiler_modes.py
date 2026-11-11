#!/usr/bin/env python3
"""Compare public compiler modes on wrapping, interleaved memory accesses.

    test_compiler_modes.py <vm> <native-aot>
"""
import json
import os
from pathlib import Path
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


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value):
    if value >= 1 << 31:
        value -= 1 << 32
    return b"\x41" + leb(value, True)


def program():
    image = bytes((i * 13 + 5) % 251 for i in range(65536))
    loads = [(parameter, offset) for offset in (2048, 2112, 2176) for parameter in (0, 1)]
    calculate = b""
    for i, (parameter, offset) in enumerate(loads):
        calculate += b"\x20" + leb(parameter) + constant(offset) + b"\x6a\x2d\0\0"
        if i:
            calculate += b"\x6a"
    entry = b""
    for bases in ((1024, 2048), (4096, 8192), (0xfffffff0, 2048), (1024, 0xffffffc0), (0xfffffff0, 0xffffffc0)):
        expected = sum(image[(bases[parameter] + offset) & 0xffffffff] for parameter, offset in loads)
        entry += constant(bases[0]) + constant(bases[1]) + b"\x10\0"
        entry += constant(expected) + b"\x47\x04\x40\0\x0b"
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\2\x7f\x7f\1\x7f\x60\0\0")
    module += section(3, b"\2\0\1") + section(5, b"\1\0\1") + section(7, b"\1\6_start\0\1")
    bodies = [b"\0" + body + b"\x0b" for body in (calculate, entry)]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    return module + section(11, b"\1\0" + constant(1024) + b"\x0b" + leb(len(image) - 1024) + image[1024:])


def main():
    vm, native = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pizdos-modes-") as directory:
        work = Path(directory)
        source = work / "input.wasm"
        source.write_bytes(program())
        for mode, flag in (("optimized", None), ("plain", "--plain"), ("strict", "--strict"), ("profile", "--tail-profile")):
            env = dict(os.environ, W2X_MEMORY_TAIL="optimized" if mode == "profile" else mode,
                       W2X_TAIL_PROFILE="1" if mode == "profile" else "")
            compiled, embedded = work / f"{mode}-native.o", work / f"{mode}-embedded.o"
            result = subprocess.run([native, *([flag] if flag else [])], input=source.read_bytes(), env=env,
                                    capture_output=True, timeout=60)
            assert result.returncode == 0, (mode, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], env=env, capture_output=True, timeout=60)
            assert result.returncode == 0 and compiled.read_bytes() == embedded.read_bytes(), (mode, result)
            for obj in (compiled, embedded):
                profile = obj.with_suffix(".jsonl")
                result = subprocess.run([vm, "run", "--", str(obj)], env=dict(env, W2O_TAIL_PROFILE=str(profile)),
                                        capture_output=True, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (mode, result)
                if mode == "profile":
                    rows = [json.loads(line) for line in profile.read_text().splitlines()]
                    functions = [function for row in rows for function in row["functions"]]
                    for counter in ("guards", "fallbacks", "omitted_checks"):
                        assert sum(function[counter] for function in functions) > 0, (counter, rows)
                else:
                    assert not profile.exists(), mode
    print("compiler modes: identical native/Wasm objects, wrapping loads and fast/fallback profile counters passed")


if __name__ == "__main__":
    main()
