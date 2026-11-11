#!/usr/bin/env python3
"""Run generated Wasm programs whose data initializers overlap and are dropped.

    test_data_segments.py <vm> <native-aot>
"""
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
    return b"\x41" + leb(value, True)


def equal(actual, expected):
    # An unexpected value traps; success is an ordinary process exit.
    return actual + constant(expected) + b"\x47\x04\x40\x00\x0b"


def program(count, start, trap=False, boundary=False):
    image = bytearray(65536)
    segments = [b"\x01\x07passive"]
    for i in range(count):
        at = (i * 37) % 1024
        value = bytes((i + j * 11) & 255 for j in range(19))
        if boundary and i == count - 1:
            at, value = 65536, b""
        segments.append(b"\0" + constant(at) + b"\x0b" + leb(len(value)) + value)
        image[at:at + len(value)] = value
    segments.append(b"\x01\x04last")
    checks = b"".join(equal(constant(i) + b"\x2d\0\0", value) for i, value in enumerate(image[:1042]))
    # Every active segment is already dropped, including the empty one at
    # the boundary. A zero-length memory.init still succeeds at source 0.
    if count:
        checks += constant(65536) + constant(0) + constant(0) + b"\xfc\x08\x01\0"
        checks += b"\xfc\x09\x01\xfc\x09\x01"
    # Passive data stays available until explicitly dropped.
    checks += constant(2048) + constant(0) + constant(7) + b"\xfc\x08\0\0"
    checks += equal(constant(2048) + b"\x28\x02\0", int.from_bytes(b"pass", "little"))
    checks += constant(2055) + constant(0) + constant(4) + b"\xfc\x08" + leb(count + 1) + b"\0"
    checks += equal(constant(2055) + b"\x28\x02\0", int.from_bytes(b"last", "little"))
    checks += b"\xfc\x09\0\xfc\x09" + leb(count + 1)
    if start:
        initializer = checks + constant(3000) + constant(91) + b"\x3a\0\0"
        entry = equal(constant(3000) + b"\x2d\0\0", 91)
        bodies = [initializer, entry]
    else:
        bodies = [checks]
    if trap:
        bodies[-1] += constant(0) + constant(0) + constant(1) + b"\xfc\x08\x01\0"
    module = b"\0asm\1\0\0\0" + section(1, b"\1\x60\0\0")
    module += section(3, leb(len(bodies)) + b"\0" * len(bodies)) + section(5, b"\1\0\1")
    module += section(7, b"\1\6_start\0" + leb(len(bodies) - 1))
    if start:
        module += section(8, b"\0")
    module += section(12, leb(len(segments)))
    code = [b"\0" + body + b"\x0b" for body in bodies]
    module += section(10, leb(len(code)) + b"".join(leb(len(body)) + body for body in code))
    return module + section(11, leb(len(segments)) + b"".join(segments))


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-segments-") as directory:
        work = Path(directory)
        source, embedded, compiled = (work / name for name in ("input.wasm", "embedded.o", "native.o"))
        for count in (0, 1, 127, 128, 129, 257):
            for start in (False, True):
                for boundary in (False, True):
                    for trap in ((False, True) if count else (False,)):
                        source.write_bytes(program(count, start, trap, boundary))
                        result = subprocess.run([native, "--strict"], input=source.read_bytes(), env=env,
                                                capture_output=True, timeout=60)
                        assert result.returncode == 0, result.stderr
                        compiled.write_bytes(result.stdout)
                        result = subprocess.run([vm, "-o", str(embedded), str(source)], env=env,
                                                capture_output=True, timeout=60)
                        assert result.returncode == 0, result.stderr
                        assert embedded.read_bytes() == compiled.read_bytes(), (count, start, trap, boundary)
                        for obj in (compiled, embedded):
                            result = subprocess.run([vm, "run", "--", str(obj)], env=env, capture_output=True, timeout=30)
                            expected = 130 if trap else 0
                            assert result.returncode == expected and result.stdout == b"", (count, start, trap, boundary, result)
                            assert (b"out of bounds memory access" in result.stderr) if trap else result.stderr == b"", result
                        checked += 1
    print(f"data segments: {checked} programs agree through native/Wasm AOT and execute as expected")


if __name__ == "__main__":
    main()
