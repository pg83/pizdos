#!/usr/bin/env python3
"""Build the stand's native CPython from the release tarball python.wasm
is built from.

    python.py TARBALL OUTPUT CONFIG_JSON

CPython's own configure and make, with the stand's native flags (-O3, no
vectorization, AVX or FMA): no computed gotos, which wasm cannot express,
so both interpreters dispatch through the same switch; no SIMD BLAKE2;
pymalloc without mimalloc; and the built-in modules of python.wasm, static
through Setup.local but for _hmac, so the native interpreter runs from the
same standard library zip in the same layout.
"""
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "bin" / "python"))
sys.path.insert(0, str(HERE.parent))

import make  # noqa: E402

NATIVE = ["-O3", "-march=x86-64-v3", "-mno-avx", "-mno-avx2", "-mno-fma", "-fno-vectorize", "-fno-slp-vectorize", "-fomit-frame-pointer"]


def main():
    tarball, output, config = sys.argv[1:]
    config = json.loads(config)
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="python-", dir=output.parent) as tmp:
        with tarfile.open(tarball) as archive:
            archive.extractall(tmp, filter="data")

        tree = next(Path(tmp).glob("Python-*"))
        # _hmac's static archive carries every hash of HACL again: it stays
        # shared, and the stand imports no module of lib-dynload.
        setup = ["*static*"] + [f"{name} {' '.join(sources)}" for name, sources in make.MODULES.items() if name != "_hmac"]
        (tree / "Modules" / "Setup.local").write_text("\n".join(setup) + "\n")
        env = dict(os.environ,
                   CC=config["cc"],
                   AR=config["ar"],
                   RANLIB=config["ar"] + " s",
                   CFLAGS=" ".join(config["cflags"]),
                   CPPFLAGS=" ".join(config["cppflags"]),
                   LDFLAGS=" ".join(config["ldflags"]),
                   OPT="-DNDEBUG -fwrapv " + " ".join(NATIVE),
                   # HACL's SIMD BLAKE2, which python.wasm has not either
                   ax_cv_check_cflags__Werror__msse__msse2__msse3__msse4_1__msse4_2="no",
                   ax_cv_check_cflags__Werror__mavx2="no")
        subprocess.run(["./configure", "--prefix=/usr", "--disable-shared", "--without-ensurepip", "--without-mimalloc",
                        "--without-computed-gotos", "--with-pymalloc", "--disable-test-modules"],
                       cwd=tree, env=env, check=True, stdout=subprocess.DEVNULL)
        jobs = os.environ.get("BUILD_JOBS") or str(os.cpu_count() or 4)
        subprocess.run(["make", "-j", jobs, "python"], cwd=tree, env=env, check=True, stdout=subprocess.DEVNULL)
        shutil.copy(tree / "python", output)


if __name__ == "__main__":
    main()
