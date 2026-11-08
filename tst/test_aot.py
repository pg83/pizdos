#!/usr/bin/env python3
"""The standalone C++ compiler's WASI export and command match wasm2o."""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command, data):
    return subprocess.run(command, input=data, capture_output=True, check=True)


def main():
    aot, command, wazero, wasm2o, busybox, _links, selftest = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="")
    assert run([wazero, selftest], b"").stdout == b"aot primitives: ok\n"

    with tempfile.TemporaryDirectory(prefix="aot-") as directory:
        directory = Path(directory)
        empty = directory / "empty.wasm"
        empty.write_bytes(b"\0asm\1\0\0\0")
        # A malformed optional name subsection must not reject the module.
        names = directory / "optional-names.wasm"
        names.write_bytes(empty.read_bytes() + b"\0\x0b\x04name\x01\x04\x01\0\x01\xff")
        references = {}

        for source in (empty, names, Path(busybox), Path(aot), Path(command)):
            output = directory / "reference.o"
            subprocess.run([wasm2o, "-o", str(output), str(source)], env=env, capture_output=True, check=True)
            expected = output.read_bytes()
            references[source] = expected
            data = source.read_bytes()
            for entry in ([wazero, "-aot", aot], [wazero, command]):
                result = run(entry, data)
                assert result.stdout == expected, f"{entry}: {source.name} differs from wasm2o"
            print(f"aot export/command: {source.name} -> {len(expected)} bytes, identical")

        # Run the C++ compiler as its own x86 output, through the same Go
        # loader and stack/runtime ABI that ordinary compiled modules use.
        self_object = directory / "aot-command.o"
        self_object.write_bytes(references[Path(command)])
        for source in (Path(busybox), Path(command)):
            result = run([wasm2o, "run", str(self_object), "--"], source.read_bytes())
            assert result.stdout == references[source], f"self-compiled aot differs for {source.name}"
            print(f"self-compiled aot: {source.name}, identical")

        once = run([wazero, "-aot", aot], empty.read_bytes())
        repeated = run([wazero, "-aot", aot, "100"], empty.read_bytes())
        assert once.stdout == repeated.stdout
        memory = lambda r: float(re.search(rb"aot memory ([\d.]+) MiB", r.stderr)[1])
        assert memory(repeated) <= memory(once) + 2, "repeated compilation retains memory"
        print(f"aot export: 100 calls, {memory(repeated):.3f} MiB")

        for data in (b"", b"bad!\1\0\0\0", b"\0asm\1\0\0\0\1\1\x80"):
            result = subprocess.run([wazero, "-aot", aot], input=data, capture_output=True)
            assert result.returncode != 0, "malformed module compiled"


if __name__ == "__main__":
    main()
