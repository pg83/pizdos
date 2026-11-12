#!/usr/bin/env python3
"""Build unchanged VM sources with incompatible runtime identity or layout."""
import importlib.util
import os
from pathlib import Path
import runpy
import shutil
import sys
import tempfile


def main():
    go, source, mode, compiler, strings, output = sys.argv[1:]
    source = Path(source).resolve()
    spec = importlib.util.spec_from_file_location("go_builder", source / "dev/go.py")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    # Each fixture is one build job, including its standard library build.
    os.environ["GOMAXPROCS"] = "1"
    with tempfile.TemporaryDirectory(prefix="pizdos-runtime-abi-build-") as directory:
        work = Path(directory)
        fork = work / "fork"
        shutil.copytree(source / "lib/goruntime", fork)
        flags = []
        if mode == "layout":
            edits = runpy.run_path(str(fork / "edits.py"))["EDITS"]
            name, anchor, replacement = edits[0]
            field = "\tinstance  uintptr"
            assert replacement.count(field) == 1
            edits[0] = (name, anchor, replacement.replace(field, "\tabiPadding uintptr\n" + field))
            (fork / "edits.py").write_text("EDITS = " + repr(edits) + "\n")
        else:
            assert mode == "version"
            # Use the Go linker's supported runtime version override. The
            # executable must reject this identity before entering native code.
            flags = ["--ldflags", "-X=runtime.buildVersion=go1.24.99"]
        build = work / "build"
        build.mkdir()
        builder.build(go, str(source / "bin/vm"), output, "--cover", "--runtime", str(fork),
                      "--embed", compiler, "--embed", strings, *flags, work=build)


if __name__ == "__main__":
    main()
