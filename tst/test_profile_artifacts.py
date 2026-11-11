#!/usr/bin/env python3
"""Verify compiler/runtime profiling artifacts through the public commands.

    test_profile_artifacts.py <vm> <native-aot>
"""
import gzip
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

MODULE = (b"\0asm\1\0\0\0\1\4\1\x60\0\0\3\2\1\0"
          b"\5\3\1\0\1\7\x0a\1\6_start\0\0\x0a\4\1\2\0\x0b")


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="", W2O_PERFMAP="", W2X_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-profiles-") as directory:
        work = Path(directory)
        source, obj, cpu = work / "input.wasm", work / "input.o", work / "cpu.pprof"
        source.write_bytes(MODULE)
        expected = subprocess.run([native], input=MODULE, capture_output=True, env=env, check=True, timeout=60).stdout
        result = subprocess.run([vm, "-o", str(obj), str(source)], env=dict(env, W2X_PROFILE=str(cpu)), capture_output=True, timeout=60)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result
        assert obj.read_bytes() == expected
        assert cpu.read_bytes().startswith(b"\x1f\x8b") and len(gzip.decompress(cpu.read_bytes())) > 20

        obj.write_bytes(b"previous object")
        result = subprocess.run([vm, "-o", str(obj), str(source)], env=dict(env, W2X_PROFILE=str(work)), capture_output=True, timeout=60)
        assert result.returncode != 0 and b"is a directory" in result.stderr, result
        assert obj.read_bytes() == b"previous object" and not list(work.glob(".vm-*"))
        obj.write_bytes(expected)

        def perf(directory):
            child = subprocess.Popen([vm, "run", "--", str(obj)], env=dict(env, W2O_PERFMAP=directory),
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            out, err = child.communicate(timeout=30)
            assert (child.returncode, out, err) == (0, b"", b""), (child.returncode, out, err)
            return child.pid

        for setting, destination in ((str(work), work), ("1", Path("/tmp"))):
            pid = perf(setting)
            path = destination / f"perf-{pid}.map"
            if not destination.is_dir():
                # perf output is best effort, including hosts without /tmp.
                assert not path.exists()
                continue
            try:
                rows = path.read_text().splitlines()
                assert rows and all(re.fullmatch(r"[0-9a-f]+ [0-9a-f]+ .+", row) for row in rows), rows
                assert any(row.split(maxsplit=2)[2] == "f0" for row in rows), rows
                assert all(int(row.split()[0], 16) > 0 and int(row.split()[1], 16) > 0 for row in rows), rows
            finally:
                path.unlink(missing_ok=True)
        perf(str(work / "missing"))

        profiled, profile = work / "profiled.o", work / "tail.jsonl"
        result = subprocess.run([native, "--tail-profile"], input=MODULE, capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result.stderr
        profiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "run", "--", str(profiled)], env=dict(env, W2O_TAIL_PROFILE=str(profile)),
                                capture_output=True, timeout=30)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result
        rows = [json.loads(line) for line in profile.read_text().splitlines()]
        assert len(rows) == 1 and rows[0]["module"] == str(profiled), rows
        result = subprocess.run([vm, "run", "--", str(profiled)], env=dict(env, W2O_TAIL_PROFILE=str(work)),
                                capture_output=True, timeout=30)
        assert result.returncode != 0 and b"is a directory" in result.stderr, result
    print("profiles: CPU gzip data, perf symbols, tail JSON and output failures behave as expected")


if __name__ == "__main__":
    main()
