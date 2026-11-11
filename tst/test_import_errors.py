#!/usr/bin/env python3
"""Import providers must reject unsupported entities and wrong signatures.

    test_import_errors.py <vm> <native-aot> <wazero>
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
    value = value.encode()
    return leb(len(value)) + value


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def program(kind, owner, name="value", *, kernel=False, parameter=False):
    types = b"\3\x60\0\0\x60\0\1\x7f\x60\1\x7f\1\x7f"
    descriptor = {0: bytes([2 if parameter else 1]), 1: b"\x70\1\0\1", 2: b"\1\1\1", 3: b"\x7f\0"}[kind]
    imports = [text(owner) + text(name) + bytes([kind]) + descriptor]
    if kernel:
        imports.append(text("kernel") + text("getpid") + b"\0\1")
    module = b"\0asm\1\0\0\0" + section(1, types) + section(2, leb(len(imports)) + b"".join(imports))
    module += section(3, b"\1\0")
    function = int(kind == 0) + int(kernel)
    exports = [text("_start") + b"\0" + leb(function)]
    if kind:
        exports.append(text("alias") + bytes([kind]) + b"\0")
    module += section(7, leb(len(exports)) + b"".join(exports))
    code = {0: (b"\x41\0" if parameter else b"") + b"\x10\0\x1a", 1: b"\xfc\x10\0\x1a",
            2: b"\x3f\0\x1a", 3: b"\x23\0\x1a"}[kind]
    if kernel:
        code += b"\x10" + leb(int(kind == 0)) + b"\x1a"
    body = b"\0" + code + b"\x0b"
    return module + section(10, b"\1" + leb(len(body)) + body)


def main():
    vm, native, wazero = sys.argv[1:]
    cases = [("kernel-" + str(kind), program(kind, "kernel"), False, "unresolved import kernel.value") for kind in (1, 2, 3)]
    cases += [("wasi-" + str(kind), program(kind, "wasi_snapshot_preview1"), False, "unresolved import wasi_snapshot_preview1.value") for kind in (1, 2, 3)]
    cases += [("stand-" + str(kind), program(kind, "foreign"), True, "unresolved import foreign.value") for kind in range(4)]
    cases += [("wasi-foreign-function", program(0, "foreign"), False, "unresolved import foreign.value"),
              ("kernel-foreign-function", program(0, "foreign", kernel=True), False, "unresolved import foreign.value"),
              ("kernel-signature", program(0, "kernel", "getpid", parameter=True), False, "import kernel.getpid is"),
              ("wasi-signature", program(0, "wasi_snapshot_preview1", "fd_write"), False, "import wasi_snapshot_preview1.fd_write is")]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-import-errors-") as directory:
        work = Path(directory)
        source, compiled, embedded = [work / name for name in ("input.wasm", "native.o", "embedded.o")]
        fixture = work / "fixture"
        fixture.write_bytes(b"input")
        for label, data, stand, diagnostic in cases:
            source.write_bytes(data)
            result = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=30)
            assert result.returncode == 0, (label, result)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (label, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (label, result)
            for obj in (compiled, embedded):
                args = ["stand", "magic", str(obj), str(fixture)] if stand else ["run", "--", str(obj)]
                result = subprocess.run([vm, *args], capture_output=True, env=env, timeout=30)
                assert result.returncode == 1 and result.stdout == b"" and diagnostic.encode() in result.stderr, (label, result)
                assert b"panic:" not in result.stderr, (label, result)
    print(f"import errors: {len(cases)} kernel/WASI/stand import kinds, foreign modules and signature mismatches rejected with both AOTs")


if __name__ == "__main__":
    main()
