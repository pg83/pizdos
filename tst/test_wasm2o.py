#!/usr/bin/env python3
"""wasm2o's fixed point: wasm2o.wasm, run as wasm2o compiled it and run under
wazero, compiles wasm2o.wasm and busybox.wasm to the bytes the native wasm2o
produces.

    test_wasm2o.py <wasm2o> <wasm2o.wasm> <prog.o> <prog.h> <wasm2o.self> <wazero> <busybox.wasm> <busybox.links>

prog.o and prog.h are wasm2o(wasm2o.wasm) with prefix prog, what wasm2o.self
is linked from.
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
    wasm2o, wasm2o_wasm, prog_o, prog_h, wasm2o_self, wazero, busybox = sys.argv[1:8]
    compilers = {
        "wasm2o.self": [wasm2o_self],
        "wazero wasm2o.wasm": [wazero, wasm2o_wasm],
    }

    with tempfile.TemporaryDirectory(prefix="wasm2o-") as work:
        expected = {
            ("prog", wasm2o_wasm): (Path(prog_o).read_bytes(), Path(prog_h).read_bytes()),
            ("busybox", busybox): compile_([wasm2o], busybox, "busybox", work),
        }

        for name, command in compilers.items():
            for (prefix, wasm), want in expected.items():
                got = compile_(command, wasm, prefix, work)
                assert got == want, f"{name} compiles {Path(wasm).name} to other bytes than wasm2o"
                print(f"{name}: {Path(wasm).name} -> {len(want[0])} bytes, identical")


if __name__ == "__main__":
    main()
