#!/usr/bin/env python3
"""Build the module test_aot.py runs: ops.c alone, no libc.

    make.py <cc> <wasm-ld> <ops.wasm>
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "dev"))

import wasm  # noqa: E402


def main():
    cc, wasm_ld, output = sys.argv[1:4]
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "aot-ops"
    work.mkdir(parents=True, exist_ok=True)
    wasm.compile_all(cc, [(HERE / "ops.c", work / "ops.o", ["-O2"])])
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [wasm_ld, "--no-entry", "-z", "stack-size=65536", "--max-memory=16777216", str(work / "ops.o"), "-o", output],
        check=True,
    )


if __name__ == "__main__":
    main()
