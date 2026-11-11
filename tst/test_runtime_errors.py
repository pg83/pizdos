#!/usr/bin/env python3
"""Run bad guest operations through the VM and check controlled diagnostics.

    test_runtime_errors.py <vm> <native-aot>

Each input is a valid Wasm program using the public kernel imports. These
cases deliberately violate the allocator or nonlocal-jump contract; they
must terminate the guest rather than crash the Go host.
"""
import os
from pathlib import Path
import re
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


def vector(values):
    return leb(len(values)) + b"".join(values)


def string(value):
    value = value.encode()
    return leb(len(value)) + value


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value):
    return b"\x41" + leb(value, True)


def call(index):
    return b"\x10" + leb(index)


def program(imports, body, *, helper=None, stack=True, heap=True, table=False):
    types = [b"\x60\0\0", b"\x60\1\x7f\1\x7f", b"\x60\2\x7f\x7f\0",
             b"\x60\1\x7f\0", b"\x60\2\x7f\x7f\1\x7f", b"\x60\0\1\x7f"]
    module = b"\0asm\1\0\0\0" + section(1, vector(types))
    module += section(2, vector([string("kernel") + string(name) + b"\0" + leb(typ) for name, typ in imports]))
    bodies = [body] + ([] if helper is None else [helper])
    module += section(3, vector([b"\0"] * len(bodies)))
    if table:
        module += section(4, b"\1\x70\0\1")
    module += section(5, b"\1\1\1\x80\1")
    module += section(6, b"\2\x7f\0" + constant(65536) + b"\x0b\x7f\1" + constant(32768) + b"\x0b")
    exports = [string("_start") + b"\0" + leb(len(imports))]
    if heap:
        exports.append(string("__heap_base") + b"\3\0")
    if stack:
        exports.append(string("__stack_pointer") + b"\3\1")
    module += section(7, vector(exports))
    if table:
        module += section(9, b"\1\0" + constant(0) + b"\x0b\1\0")
    # A scratch local retains the allocated pointer between operations.
    bodies = [b"\1\1\x7f" + code + b"\x0b" for code in bodies]
    return module + section(10, vector([leb(len(code)) + code for code in bodies]))


def main():
    vm, native = sys.argv[1:]
    cases = []
    for name in ("longjmp", "_longjmp", "siglongjmp"):
        cases.append(("unset-" + name, program([(name, 2)], constant(128) + constant(1) + call(0)),
                      "longjmp: the jmp_buf was not set"))
    for name in ("setjmp", "_setjmp"):
        cases.append(("missing-stack-" + name, program([(name, 1)], constant(128) + call(0) + b"\x1a", stack=False),
                      "setjmp: no __stack_pointer"))
    cases.append(("indirect-setjmp", program([("setjmp", 1)], constant(128) + constant(0) + b"\x11\1\0\x1a", table=True),
                  "setjmp: called through a pointer"))
    for name in ("fork", "vfork"):
        cases.append(("indirect-" + name, program([(name, 5)], constant(0) + b"\x11\5\0\x1a", table=True),
                      name + ": called through a pointer"))
    cases.append(("returned-setjmp", program([("setjmp", 1), ("longjmp", 2)],
                  call(3) + constant(128) + constant(1) + call(1), helper=constant(128) + call(0) + b"\x1a"),
                  "longjmp: the function that set the jmp_buf has returned"))
    cases.append(("missing-heap", program([("malloc", 1)], constant(16) + call(0) + b"\x1a", heap=False), "no __heap_base"))
    cases.append(("missing-heap-memalign", program([("memalign", 4)], constant(4096) + constant(16) + call(0) + b"\x1a", heap=False), "no __heap_base"))
    imports = [("malloc", 1), ("free", 3), ("realloc", 4), ("malloc_usable_size", 1)]
    allocate = constant(64) + call(0) + b"\x21\0"
    pointer = b"\x20\0"
    cases.append(("free-unallocated", program(imports, constant(1) + call(1)), "free(): invalid pointer"))
    cases.append(("free-outside-heap", program(imports, allocate + constant(-16) + call(1)), "free(): invalid pointer"))
    cases.append(("free-interior-slot", program(imports, allocate + pointer + constant(1) + b"\x6a" + call(1)), "free(): invalid pointer"))
    cases.append(("free-never-allocated-slot", program(imports, allocate + pointer + constant(64) + b"\x6a" + call(1)), "free(): invalid pointer"))
    cases.append(("free-double-slot", program(imports, allocate + pointer + call(1) + pointer + call(1)), "free(): invalid pointer"))
    run = constant(65537) + call(0) + b"\x21\0"
    cases.append(("free-run-tail", program(imports, run + pointer + constant(65536) + b"\x6a" + call(1)), "free(): invalid pointer"))
    cases.append(("free-run-interior", program(imports, run + pointer + constant(16) + b"\x6a" + call(1)), "free(): invalid pointer"))
    cases.append(("free-double-run", program(imports, run + pointer + call(1) + pointer + call(1)), "free(): invalid pointer"))
    cases.append(("realloc-interior", program(imports, allocate + pointer + constant(1) + b"\x6a" + constant(80) + call(2) + b"\x1a"), "realloc(): invalid pointer"))
    cases.append(("usable-size-interior", program(imports, allocate + pointer + constant(1) + b"\x6a" + call(3) + b"\x1a"), "malloc_usable_size(): invalid pointer"))

    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-runtime-errors-") as directory:
        work = Path(directory)
        for name, module, diagnostic in cases:
            source, compiled, embedded = [work / (name + suffix) for suffix in (".wasm", ".native.o", ".embedded.o")]
            source.write_bytes(module)
            result = subprocess.run([native, "--strict"], input=module, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (name, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(source)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (name, result.stderr)
            assert compiled.read_bytes() == embedded.read_bytes(), name
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "-a", "runtime-error", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert result.returncode == 134 and result.stdout == b"", (name, result)
                lines = result.stderr.decode().splitlines()
                assert lines and diagnostic in lines[0], (name, result.stderr)
                assert all(re.match(r"runtime-error\[\d+\]: ", line) for line in lines), (name, result.stderr)
                assert len(lines) > 1, (name, "missing guest stack trace")
    print(f"runtime errors: {len(cases)} controlled allocator and nonlocal-jump failures passed with both AOTs")


if __name__ == "__main__":
    main()
