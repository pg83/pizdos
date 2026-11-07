#!/usr/bin/env python3
"""aot's fixed point: aot.wasm, run as aot compiled it and run under wazero,
compiles aot.wasm and busybox.wasm to the bytes the native aot produces.

    test_aot.py <aot> <aot.wasm> <prog.o> <prog.h> <aot.self> <wazero> <busybox.wasm> <busybox.links>

prog.o and prog.h are aot(aot.wasm) with prefix prog, what aot.self is
linked from.
"""
import subprocess
import sys
import tempfile
from pathlib import Path


def compile_(command, wasm, prefix, work):
    """The object and the header the compiler run by `command` makes of wasm."""
    obj, header = Path(work) / f"{prefix}.o", Path(work) / f"{prefix}.h"
    subprocess.run([*command, "-p", prefix, "-H", str(header), "-o", str(obj), wasm], check=True)

    return obj.read_bytes(), header.read_bytes()


def main():
    aot, aot_wasm, prog_o, prog_h, aot_self, wazero, busybox = sys.argv[1:8]
    compilers = {
        "aot.self": [aot_self],
        "wazero aot.wasm": [wazero, aot_wasm],
    }

    with tempfile.TemporaryDirectory(prefix="aot-") as work:
        expected = {
            ("prog", aot_wasm): (Path(prog_o).read_bytes(), Path(prog_h).read_bytes()),
            ("busybox", busybox): compile_([aot], busybox, "busybox", work),
        }

        for name, command in compilers.items():
            for (prefix, wasm), want in expected.items():
                got = compile_(command, wasm, prefix, work)
                assert got == want, f"{name} compiles {Path(wasm).name} to other bytes than aot"
                print(f"{name}: {Path(wasm).name} -> {len(want[0])} bytes, identical")


if __name__ == "__main__":
    main()
