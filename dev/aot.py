#!/usr/bin/env python3
"""Compile a wasm input through the native AOT's stdin/stdout interface."""
import subprocess
import sys
from pathlib import Path


def main():
    compiler, source, output = sys.argv[1:]
    Path(output).parent.mkdir(parents=True, exist_ok=True)

    with open(source, "rb") as inp, open(output, "wb") as out:
        subprocess.run([compiler], stdin=inp, stdout=out, check=True)


if __name__ == "__main__":
    main()
