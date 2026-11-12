#!/usr/bin/env python3
"""Reject incompatible runtime builds through the public VM command."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    vm, native, version, layout = sys.argv[1:]
    # A valid command module; rejection must happen before its entry executes.
    data = (b"\0asm\1\0\0\0\1\4\1\x60\0\0\3\2\1\0"
            b"\7\12\1\6_start\0\0\12\4\1\2\0\x0b")
    with tempfile.TemporaryDirectory(prefix="pizdos-runtime-abi-") as directory:
        work = Path(directory)
        obj = work / "command.o"
        result = subprocess.run([native, "--strict"], input=data, capture_output=True, timeout=60)
        assert result.returncode == 0, result
        obj.write_bytes(result.stdout)
        module, embedded = work / "command.wasm", work / "embedded.o"
        module.write_bytes(data)
        env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
        result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == obj.read_bytes(), result
        profiles = work / "profiles"
        profiles.mkdir()
        env["GOCOVERDIR"] = os.environ.get("GOCOVERDIR", str(profiles))
        for obj in (obj, embedded):
            result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result
            for incompatible, mode in ((version, "version"), (layout, "layout")):
                result = subprocess.run([incompatible, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert result.returncode == 1 and result.stdout == b"" and b"panic:" not in result.stderr, (mode, result)
                if mode == "version":
                    assert b"the loader is written for Go 1.25, not go1.24.99" in result.stderr, result
                else:
                    match = re.search(rb"g.instance is at (\d+), the code reads (\d+)", result.stderr)
                    assert match and int(match[1]) - int(match[2]) == 8, result
    print("runtime ABI: incompatible version identity and a shifted runtime field are rejected before guest execution")


if __name__ == "__main__":
    main()
