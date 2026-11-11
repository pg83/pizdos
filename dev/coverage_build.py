#!/usr/bin/env python3
"""Build native AOT with Clang's source coverage and compiler-rt.

    coverage_build.py <aot.cpp> <flush.c> <output-binary> <toolchain-json>

Compiler diagnostics use _exit/abort, so the coverage-only shim flushes
their counters before preserving the original exit behaviour.
"""
import json
from pathlib import Path
import subprocess
import sys


def build(source, runtime, output, tools):
    cc = tools["cc"]
    if not cc:
        sys.exit("coverage needs Clang on PATH")
    version = subprocess.check_output([cc, "--version"], text=True)
    if "clang" not in version.lower():
        sys.exit("coverage requires Clang and matching llvm-profdata/llvm-cov")
    profile = tools.get("profile_runtime") or subprocess.check_output([cc, "-print-file-name=libclang_rt.profile-x86_64.a"], text=True).strip()
    if not Path(profile).is_file():
        sys.exit("Clang's profiling runtime is missing: install libclang-rt for this Clang version")
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    obj = output.with_suffix(".o")
    profiling = output.with_name("flush.o")
    variant = ["-DAOT_CHAOS"] if tools.get("chaos") else []
    subprocess.run([cc, *tools["cppflags"], *tools["cxxflags"], *tools["flags"], "-O3", "-g",
                    "-fprofile-instr-generate", "-fcoverage-mapping", "-mllvm", "-enable-name-compression=false",
                    *variant, "-c", source, "-o", str(obj)], check=True)
    subprocess.run([cc, "-x", "c", "-std=c11", "-O2",
                    "-c", runtime, "-o", str(profiling)], check=True)
    # Explicitly link compiler-rt too when the host uses -nostdlib for musl.
    subprocess.run([cc, "-o", str(output), str(obj), str(profiling), "-Wl,--wrap=_exit,--wrap=abort",
                    "-Wl,--whole-archive", profile, "-Wl,--no-whole-archive", *tools["ldflags"]], check=True)


if __name__ == "__main__":
    build(*sys.argv[1:4], json.loads(sys.argv[4]))
