#!/usr/bin/env python3
"""The embedded compiler, its WASI module, and relinked AOT agree with C++."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    wasm2o, native, reactor, command, aot, relinked, wazero, busybox, _links = sys.argv[1:]
    compilers = {
        "self-compiled AOT": [wasm2o, "run", aot, "--"],
        "relinked AOT": [wasm2o, "run", relinked, "--"],
        "WASI reactor": [wazero, "-aot", reactor],
        "WASI command": [wazero, command],
    }

    with tempfile.TemporaryDirectory(prefix="wasm2o-") as work:
        out = Path(work) / "out.o"
        isolated = Path(work) / "wasm2o"
        shutil.copyfile(wasm2o, isolated)
        isolated.chmod(0o755)
        env = dict(os.environ, PATH="")

        for source in [reactor, busybox]:
            data = Path(source).read_bytes()
            want = subprocess.run([native], input=data, capture_output=True, check=True).stdout
            subprocess.run([str(isolated), "-o", str(out), source], cwd=work, env=env, check=True)
            assert out.read_bytes() == want, f"embedded AOT: {source} differs from native C++"

            for name, entry in compilers.items():
                got = subprocess.run(entry, input=data, capture_output=True, check=True).stdout
                assert got == want, f"{name}: {source} differs from native C++"
                print(f"{name}: {Path(source).name} -> {len(want)} bytes, identical")

        bad = Path(work) / "bad.wasm"
        bad.write_bytes(b"not wasm")
        out.write_bytes(b"previous output")
        result = subprocess.run([str(isolated), "-o", str(out), str(bad)], cwd=work, env=env, capture_output=True)
        assert result.returncode != 0 and b"aot exited" in result.stderr
        assert out.read_bytes() == b"previous output", "failed compilation replaced the output"
        assert not list(Path(work).glob(".wasm2o-*")), "failed compilation retained temporary output"


if __name__ == "__main__":
    main()
