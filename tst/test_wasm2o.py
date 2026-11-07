#!/usr/bin/env python3
"""wasm2o's fixed point: wasm2o.wasm, run as wasm2o compiled it by `wasm2o
run` and run under wazero, compiles wasm2o.wasm and busybox.wasm to the bytes
the native wasm2o produces.

    test_wasm2o.py <wasm2o> <wasm2o.wasm> <wasm2o.o> <wazero> <busybox.wasm> <busybox.links>

wasm2o.o is wasm2o(wasm2o.wasm).
"""
import subprocess
import sys
import tempfile
from pathlib import Path


def compile_(command, wasm, work):
    """The object the compiler run by `command` makes of wasm."""
    obj = Path(work) / "out.o"
    subprocess.run([*command, "-o", str(obj), wasm], check=True)

    return obj.read_bytes()


def main():
    wasm2o, wasm2o_wasm, wasm2o_o, wazero, busybox = sys.argv[1:6]
    compilers = {
        "wasm2o run wasm2o.o": [wasm2o, "run", wasm2o_o, "--"],
        "wazero wasm2o.wasm": [wazero, wasm2o_wasm],
    }

    with tempfile.TemporaryDirectory(prefix="wasm2o-") as work:
        expected = {
            wasm2o_wasm: Path(wasm2o_o).read_bytes(),
            busybox: compile_([wasm2o], busybox, work),
        }

        for name, command in compilers.items():
            for wasm, want in expected.items():
                got = compile_(command, wasm, work)
                assert got == want, f"{name} compiles {Path(wasm).name} to other bytes than wasm2o"
                print(f"{name}: {Path(wasm).name} -> {len(want)} bytes, identical")


if __name__ == "__main__":
    main()
