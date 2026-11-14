#!/usr/bin/env python3
"""Startup and static libc results fail cleanly without guest heap space.

    test_kernel_exhaustion.py <vm> <native-aot> <wazero>
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dev"))

from exe import pack  # noqa: E402


def leb(value, signed=False):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(out + bytes([byte]))
        out.append(byte | 128)


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, data):
    return bytes([kind]) + leb(len(data)) + data


def constant(value):
    return b"\x41" + leb(value, True)


def program(name, args, returns, reserve, error=None):
    signature = b"\x60" + leb(len(args)) + b"\x7f" * len(args) + (b"\1\x7f" if returns else b"\0")
    module = b"\0asm\1\0\0\0" + section(1, b"\3" + signature + b"\x60\1\x7f\1\x7f\x60\0\0")
    module += section(2, b"\2" + text("kernel") + text(name) + b"\0\0" + text("kernel") + text("malloc") + b"\0\1")
    resizing = name == "realloc"
    module += section(3, b"\1\2") + section(5, b"\1\1\1" + leb(33 if resizing or reserve else 1))
    module += section(6, b"\1\x7f\0" + constant(65536) + b"\x0b")
    module += section(7, b"\2" + text("_start") + b"\0\2" + text("__heap_base") + b"\3\0")
    code = b""
    if reserve:
        # Leave one page for the argv pointer array, but no two-page run
        # for its long first string. The allocator cannot grow past 33 pages.
        code += constant(31 * 65536) + b"\x10\1\x45\x04\x40\0\x0b"
    if resizing:
        code += constant(65536) + b"\x10\1\x22\0\x45\x04\x40\0\x0b"
        code += b"\x20\0" + constant(90) + b"\x3a\0\0"
    code += b"".join(b"\x20\0" if resizing and i == 0 else constant(arg) for i, arg in enumerate(args)) + b"\x10\0"
    if error is None:
        code += (b"\x1a" if returns else b"") + b"\0"
    else:
        code += b"\x04\x40\0\x0b" + constant(0) + b"\x28\2\0" + constant(error) + b"\x47\x04\x40\0\x0b"
    if resizing:
        code += b"\x20\0\x2d\0\0" + constant(90) + b"\x47\x04\x40\0\x0b"
    body = (b"\1\1\x7f" if resizing else b"\0") + code + b"\x0b"
    return module + section(10, b"\1" + leb(len(body)) + body)


def main():
    vm, native, wazero = sys.argv[1:]
    startup = ("__kernel_start", list(range(1024, 1056, 4)), False)
    cases = [("startup-array", *startup, False, None), ("startup-string", *startup, True, None),
             # localeconv takes the size of its struct lconv.
             ("locale-structure", "localeconv", [56], True, False, None), ("constant-string", "nl_langinfo", [14], True, False, None),
             ("working-directory", "get_current_dir_name", [], True, False, 12),
             ("large-allocation", "malloc", [4194304], True, False, 12),
             ("growing-allocation", "realloc", [0, 4194304], True, False, 12)]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-kernel-exhaustion-") as directory:
        work = Path(directory)
        source, executable, compiled, embedded = [work / name for name in ("input.wasm", "input", "native.o", "embedded.o")]
        for label, name, args, returns, reserve, error in cases:
            data = program(name, args, returns, reserve, error)
            source.write_bytes(data)
            executable.write_bytes(pack(data))
            result = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0, (label, result)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (label, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (label, result)
            result = subprocess.run([vm, "run", "-a", "A" * 70000 if reserve else label, "--", str(executable)],
                                    capture_output=True, env=env, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (134 if error is None else 0, b"", b""), (label, result)
    print("kernel exhaustion: startup and static results abort cleanly; allocation failures preserve errno and existing data with both AOTs")


if __name__ == "__main__":
    main()
