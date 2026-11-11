#!/usr/bin/env python3
"""Build lib/str: pg83/str's string and memory functions in the convention of
the code vm compiles, the object vm's loader links.

    make.py <str-tarball> <libstr.o>

str's own compiler (dev/strcc.py, Python) compiles the library with
--abi wasm2o; the object passes str's check of that contract (tst/wasm2o.py)
before it is kept.
"""
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path


def main():
    tarball, output = sys.argv[1:3]
    work = Path(os.environ.get("TMPDIR") or "/tmp") / "libstr"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    with tarfile.open(tarball) as archive:
        archive.extractall(work, filter="data")

    tree = next(work.glob("str-*"))
    out = work / "out"
    subprocess.run([sys.executable, str(tree / "dev" / "strcc.py"), str(tree), "--abi", "wasm2o",
                    "-o", str(out / "libstr.a"), "--out", str(out)], check=True)
    subprocess.run([sys.executable, str(tree / "tst" / "wasm2o.py"), "--object", str(out / "libstr.o")], check=True)
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(out / "libstr.o", output)


if __name__ == "__main__":
    main()
