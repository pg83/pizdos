#!/usr/bin/env python3
"""Build test_cxx's program against lib/cxx and lib/c.

    make.py <cc> <wasm-ld> <check.cpp> <libc-include> <libc.a> <cxx-include.tar> <libc++.a> <check.wasm>
"""
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "dev"))

import wasm  # noqa: E402

LINK = ["--stack-first", "-z", "stack-size=1049600", "--max-memory=4294967296", "--export=__heap_base",
        "--export=__stack_pointer", "--gc-sections"]


def main():
    cc, ld, source, include, libc, cxx_include, libcxx, output = sys.argv[1:9]

    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(cxx_include) as archive:
            archive.extractall(tmp, filter="data")

        obj = Path(tmp) / "check.o"
        wasm.compile_all(cc, [(Path(source), obj, ["-O2", "-std=c++23", "-fno-exceptions", "-nostdinc++",
                                                   "-isystem", str(Path(tmp) / "include/c++/v1"),
                                                   "-isystem", str(Path(include).resolve())])])
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([ld, *LINK, "-o", output, str(obj), libcxx, libc], check=True)


if __name__ == "__main__":
    main()
