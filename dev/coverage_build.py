#!/usr/bin/env python3
"""Build the unchanged AOT source with GCC/gcov and a small profiling runtime.

    coverage_build.py <aot.cpp> <gcov.c> <output-binary> <toolchain-json>

The host supplies GCC, gcov, gcov-tool and an assembler; production builds
continue using clang. The runtime is the one used by monorepo/str, with a
separate file per process and flushes on compiler errors as well as success.
"""
import json
from pathlib import Path
import subprocess
import sys


def build(source, runtime, output, tools):
    cc = tools["cc"]
    if not cc:
        sys.exit("coverage needs GCC: set COVERAGE_CXX and put its assembler on PATH")
    major = int(subprocess.check_output([cc, "-dumpversion"], text=True).split(".")[0])
    kinds = {12: 8, 13: 8, 14: 9, 15: 10}.get(major)
    if kinds is None:
        sys.exit(f"the profiling runtime does not support GCC {major}")
    cppflags = []
    source_flags = iter(tools["cppflags"])
    for flag in source_flags:
        if flag == "-isystem":
            include = next(source_flags)
            if "clang" not in include:
                cppflags.extend([flag, include])
        elif flag not in ("-nostdinc++", "-fcolor-diagnostics"):
            cppflags.append(flag)
    include = subprocess.check_output([cc, "-print-file-name=include"], text=True).strip()
    cppflags.extend(["-isystem", include])
    ldflags = [flag for flag in tools["ldflags"] if flag != "-nostdlib++"]
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    obj = output.with_suffix(".o")
    profiling = output.with_name("gcov.o")
    subprocess.run([cc, *cppflags, "-x", "c++", "-std=c++20", "-O0", "-g", "-fmax-errors=5", "-fno-exceptions", "-fno-rtti",
                    "-D__is_trivially_destructible(T)=__has_trivial_destructor(T)",
                    "-fno-builtin", "-fwrapv", "-ffp-contract=off", "-mpopcnt", "-Wno-init-list-lifetime",
                    "-fprofile-arcs", "-ftest-coverage", "-fprofile-update=atomic", "-c", source, "-o", str(obj)], check=True)
    subprocess.run([cc, *cppflags, "-x", "c", "-std=c11", "-O2", f"-DGCOV_COUNTERS={kinds}",
                    "-c", runtime, "-o", str(profiling)], check=True)
    subprocess.run([cc, "-o", str(output), str(obj), str(profiling), "-Wl,--wrap=_exit,--wrap=abort", *ldflags], check=True)


if __name__ == "__main__":
    build(*sys.argv[1:4], json.loads(sys.argv[4]))
