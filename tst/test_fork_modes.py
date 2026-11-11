#!/usr/bin/env python3
"""Fork through the public kernel ABI with bounds checks, ASAN and profiling.

    test_fork_modes.py <vm> <native-aot> <wazero>
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def leb(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value):
    result = bytearray(b"\x41")
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and byte < 64:
            return bytes(result + bytes([byte]))
        result.append(byte | 128)


def probes(count):
    # Function 4 performs one checked read per invocation.
    return (constant(count) + b"\x21\1\x03\x40" + constant(40960) + b"\x10\4" + constant(66)
            + b"\x47\x04\x40\0\x0b\x20\1" + constant(1) + b"\x6b\x22\1\x0d\0\x0b")


def program():
    code = probes(20) + b"\x10\0\x22\0\x45\x04\x40"
    code += probes(3) + constant(17) + b"\x10\1\0\x05" + probes(1)
    code += b"\x20\0" + constant(49152) + constant(0) + b"\x10\2\x20\0\x47\x04\x40\0\x0b"
    code += constant(49152) + b"\x28\2\0" + constant(17 << 8) + b"\x47\x04\x40\0\x0b"
    code += constant(0) + b"\x10\1\0\x0b"
    types = [b"\x60\0\1\x7f", b"\x60\1\x7f\0", b"\x60\3\x7f\x7f\x7f\1\x7f", b"\x60\0\0", b"\x60\1\x7f\1\x7f"]
    module = b"\0asm\1\0\0\0" + section(1, leb(len(types)) + b"".join(types))
    imports = [("fork", 0), ("_exit", 1), ("waitpid", 2)]
    module += section(2, leb(len(imports)) + b"".join(text("kernel") + text(name) + b"\0" + leb(typ) for name, typ in imports))
    module += section(3, b"\2\3\4") + section(5, b"\1\1\1\2")
    module += section(6, b"\2\x7f\1" + constant(32768) + b"\x0b\x7f\0" + constant(65536) + b"\x0b")
    exports = [("_start", 0, 3), ("probe", 0, 4), ("__stack_pointer", 3, 0), ("__heap_base", 3, 1)]
    module += section(7, leb(len(exports)) + b"".join(text(name) + bytes([kind]) + leb(index) for name, kind, index in exports))
    bodies = [b"\1\2\x7f" + code + b"\x0b", b"\0\x20\0\x2d\0\0\x0b"]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    # Data and the wait status live above the stack pointer, following the
    # guest ABI. No allocator is initialized before fork.
    return module + section(11, b"\1\0" + constant(40960) + b"\x0b\1\x42")


def main():
    vm, native, wazero = sys.argv[1:]
    data = program()
    with tempfile.TemporaryDirectory(prefix="pizdos-fork-modes-") as directory:
        work = Path(directory)
        source = work / "input.wasm"
        source.write_bytes(data)
        result = subprocess.run([wazero, "-compile", str(source)], capture_output=True, timeout=60)
        assert result.returncode == 0, result.stderr
        for mode, flag in (("optimized", None), ("strict", "--strict"), ("asan", "--asan"), ("profile", "--tail-profile")):
            env = dict(os.environ, W2X_MEMORY_TAIL="strict" if mode == "strict" else "optimized", W2X_TAIL_PROFILE="1" if mode == "profile" else "")
            compiled, embedded = work / f"{mode}-native.o", work / f"{mode}-embedded.o"
            result = subprocess.run([native, *([flag] if flag else [])], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (mode, result.stderr)
            compiled.write_bytes(result.stdout)
            asan = ["--asan"] if mode == "asan" else []
            result = subprocess.run([vm, *asan, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (mode, result)
            for obj in (compiled, embedded):
                profile = obj.with_suffix(".jsonl")
                result = subprocess.run([vm, "run", *asan, "--", str(obj)], capture_output=True, env=dict(env, W2O_TAIL_PROFILE=str(profile)), timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (mode, result)
                if mode == "profile":
                    rows = [json.loads(line) for line in profile.read_text().splitlines()]
                    assert len(rows) == 2, rows
                    probes_by_process = [[fn for fn in row["functions"] if fn["function"] == 4] for row in rows]
                    assert all(len(functions) == 1 for functions in probes_by_process), rows
                    # The parent reads 20 times before fork and once after;
                    # the child's three reads must not include that history.
                    assert sorted(functions[0]["ordinary_checks"] for functions in probes_by_process) == [3, 21], rows
                else:
                    assert not profile.exists(), mode
    print("fork modes: native/Wasm objects agree; no-heap fork, ASAN memory and independent parent/child profiles passed")


if __name__ == "__main__":
    main()
