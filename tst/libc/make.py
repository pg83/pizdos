#!/usr/bin/env python3
"""Build a test's program against lib/c: test_libc's, and test_asan's with
`-g`, whose DWARF line table the object carries into the traces.

    make.py <cc> <wasm-ld> <check.c> <libc-include> <libc.a> <check.wasm> [cflag...]
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
    cflags = sys.argv[7:]

    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / "check.o"
        wasm.compile_all(cc, [(Path(source), obj, ["-O2", "-std=c11", "-Werror=implicit-function-declaration",
                                                   "-isystem", str(Path(include).resolve()), *cflags])])
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([ld, *LINK, "-o", output, str(obj), libc], check=True)


if __name__ == "__main__":
    main()
