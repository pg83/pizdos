#!/usr/bin/env python3
"""Verify compiler/runtime profiling artifacts through the public commands.

    test_profile_artifacts.py <vm> <native-aot>
"""
import gzip
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

MODULE = (b"\0asm\1\0\0\0\1\4\1\x60\0\0\3\2\1\0"
          b"\5\3\1\0\1\7\x0a\1\6_start\0\0\x0a\4\1\2\0\x0b")


def jitdump(path, pid):
    """The code loads of a jitdump, perf's format: their names and sizes."""
    data = path.read_bytes()
    magic, version, size, machine, _, owner = struct.unpack_from("<6I", data)
    assert (magic, version, size, machine, owner) == (0x4A695444, 1, 40, 62, pid), (magic, version, size, machine, owner)
    loads, at = [], size
    while at < len(data):
        kind, total = struct.unpack_from("<2I", data, at)
        assert total >= 16 and at + total <= len(data), (at, kind, total)
        if kind == 0:
            vma, code, length = struct.unpack_from("<3Q", data, at + 24)
            end = data.index(b"\0", at + 56)
            name = data[at + 56:end].decode()
            assert vma == code > 0 and end + 1 + length == at + total, (name, vma, code, length)
            loads.append((name, length))
        at += total
    return loads


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
            child = subprocess.Popen([vm, "run", "--", str(source)], env=dict(env, W2O_PERFMAP=directory),
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            out, err = child.communicate(timeout=30)
            assert (child.returncode, out, err) == (0, b"", b""), (child.returncode, out, err)
            return child.pid

        # The names go to the directory W2O_PERFMAP names, or to TMPDIR's:
        # the perf map and the jitdump, a module's code as module:region.
        for setting, destination in ((str(work), work), ("1", Path(os.environ.get("TMPDIR") or "/tmp"))):
            pid = perf(setting)
            path, dump = destination / f"perf-{pid}.map", destination / f"jit-{pid}.dump"
            if not destination.is_dir():
                # perf output is best effort, including hosts without the directory.
                assert not path.exists() and not dump.exists()
                continue
            try:
                rows = path.read_text().splitlines()
                assert rows and all(re.fullmatch(r"[0-9a-f]+ [0-9a-f]+ .+", row) for row in rows), rows
                assert any(row.split(maxsplit=2)[2] == "input:f0" for row in rows), rows
                assert all(int(row.split()[0], 16) > 0 and int(row.split()[1], 16) > 0 for row in rows), rows
                loads = jitdump(dump, pid)
                assert sorted(name for name, _ in loads) == sorted(row.split(maxsplit=2)[2] for row in rows), loads
                assert any(name == "input:f0" and length > 0 for name, length in loads), loads
            finally:
                path.unlink(missing_ok=True)
                dump.unlink(missing_ok=True)
        perf(str(work / "missing"))

        # vm run compiles with the tail's counters when W2X_TAIL_PROFILE asks,
        # as the native compiler does with --tail-profile.
        profiled, profile = work / "profiled.o", work / "tail.jsonl"
        counting = dict(env, W2X_TAIL_PROFILE="1")
        result = subprocess.run([native, "--tail-profile"], input=MODULE, capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result.stderr
        profiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(obj), str(source)], env=counting, capture_output=True, timeout=60)
        assert result.returncode == 0 and obj.read_bytes() == profiled.read_bytes(), result
        result = subprocess.run([vm, "run", "--", str(source)], env=dict(counting, W2O_TAIL_PROFILE=str(profile)),
                                capture_output=True, timeout=30)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result
        rows = [json.loads(line) for line in profile.read_text().splitlines()]
        assert len(rows) == 1 and rows[0]["module"] == str(source), rows
        result = subprocess.run([vm, "run", "--", str(source)], env=dict(counting, W2O_TAIL_PROFILE=str(work)),
                                capture_output=True, timeout=30)
        assert result.returncode != 0 and b"is a directory" in result.stderr, result
    print("profiles: CPU gzip data, perf symbols, tail JSON and output failures behave as expected")


if __name__ == "__main__":
    main()
