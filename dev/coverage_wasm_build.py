#!/usr/bin/env python3
"""Build an instrumented freestanding Wasm reactor without compiler-rt.

    coverage_wasm_build.py <clang> <wasm-ld> <aot.cpp> <shim.c> <output> <flags-json>

The VM extracts counters before unmapping each compiler instance. Keep the
coverage mapping custom sections; production --strip-all would erase them.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def build(clang, linker, source, shim, output, options):
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    flags = [flag for flag in options["flags"] if not flag.startswith("-Wl,")]
    variant = ["-DAOT_CHAOS"] if options.get("chaos") else []
    with tempfile.TemporaryDirectory(prefix="wasm-coverage-") as directory:
        obj, runtime = [str(Path(directory, name + ".o")) for name in ("aot", "profile")]
        subprocess.run([clang, *flags, *variant, "-fprofile-instr-generate", "-fcoverage-mapping",
                        "-mllvm", "-enable-value-profiling=false",
                        "-mllvm", "-enable-name-compression=false", "-c", source, "-o", obj], check=True)
        subprocess.run([clang, "--target=wasm32-unknown-unknown", "-ffreestanding", "-nostdinc",
                        "-O2", *variant, "-c", shim, "-o", runtime], check=True)
        subprocess.run([linker, "--no-entry", "--export-memory", "--stack-first",
                        "-z", "stack-size=4195328", obj, runtime, "-o", str(output)], check=True)


if __name__ == "__main__":
    build(*sys.argv[1:6], json.loads(sys.argv[6]))
