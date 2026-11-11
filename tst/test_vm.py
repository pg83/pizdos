#!/usr/bin/env python3
"""The embedded compiler, its WASI module, and relinked AOT agree with C++."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    vm, native, reactor, command, aot, relinked, wazero, busybox, _links = sys.argv[1:]
    compilers = {
        "self-compiled AOT": [vm, "run", "--", aot],
        "relinked AOT": [vm, "run", "--", relinked],
        "WASI reactor": [wazero, "-aot", reactor],
        "WASI command": [wazero, command],
    }

    with tempfile.TemporaryDirectory(prefix="vm-") as work:
        out = Path(work) / "out.o"
        isolated = Path(work) / "vm"
        shutil.copyfile(vm, isolated)
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

        # An ignored custom section makes input end exactly on a page boundary.
        # Supplying it through a pipe also exercises partial stdin reads.
        empty = b"\0asm\1\0\0\0"
        want = subprocess.run([native], input=empty, capture_output=True, check=True).stdout
        payload_size = 2 * 65536 - len(empty) - 4
        length = bytearray()
        remaining = payload_size
        while remaining >= 128:
            length.append((remaining & 127) | 128)
            remaining >>= 7
        length.append(remaining)
        padded = empty + b"\0" + length + bytes(payload_size)
        assert len(padded) == 2 * 65536
        for name, entry in {"native": [native], **compilers}.items():
            got = subprocess.run(entry, input=padded, capture_output=True, check=True, pipesize=4096).stdout
            assert got == want, f"{name}: page-aligned stdin changed the module"
        print("page-aligned stdin: native/WASI/self-compiled/relinked identical")

        bad = Path(work) / "bad.wasm"
        bad.write_bytes(b"not wasm")
        out.write_bytes(b"previous output")
        result = subprocess.run([str(isolated), "-o", str(out), str(bad)], cwd=work, env=env, capture_output=True)
        assert result.returncode != 0 and b"aot exited" in result.stderr
        assert out.read_bytes() == b"previous output", "failed compilation replaced the output"
        assert not list(Path(work).glob(".vm-*")), "failed compilation retained temporary output"


if __name__ == "__main__":
    main()
