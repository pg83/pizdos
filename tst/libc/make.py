#!/usr/bin/env python3
"""Build test_libc's program against lib/c.

    make.py <cc> <wasm-ld> <check.c> <libc-include> <libc.a> <check.wasm>
"""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "dev"))

import wasm  # noqa: E402

LINK = ["--stack-first", "-z", "stack-size=1049600", "--max-memory=4294967296", "--export=__heap_base",
        "--export=__stack_pointer", "--gc-sections"]


def main():
    cc, ld, source, include, libc, output = sys.argv[1:7]

    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / "check.o"
        wasm.compile_all(cc, [(Path(source), obj, ["-O2", "-std=c11", "-Werror=implicit-function-declaration",
                                                   "-isystem", str(Path(include).resolve())])])
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([ld, *LINK, "-o", output, str(obj), libc], check=True)


if __name__ == "__main__":
    main()
