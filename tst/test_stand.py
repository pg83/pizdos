#!/usr/bin/env python3
"""Exercise the stand CLI using a small, deterministic decoder program.

    test_stand.py <vm> <native-aot> <fixture.wasm>
"""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def call(command, **variables):
    return subprocess.run(command, capture_output=True, env=dict(os.environ, **variables), timeout=60)


def digest(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return f"{value:016x}"


def document(pages):
    geometry, pixels = bytearray(), bytearray()
    for page in range(min(pages, 16)):
        if page in (1, 2):
            geometry += bytes(4)
            pixels += bytes(4)
        else:
            geometry += struct.pack("<II", 1, 1)
            pixels += struct.pack("<II4B", 1, 1, 64, 32 if page == 0 else 64, page, 255)
    return f"{pages} {digest(geometry)} {digest(pixels)}"


def main():
    vm, native, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pizdos-stand-") as directory:
        work = Path(directory)
        files = []
        for name, data in (("ok", b"ok"), ("fail", b"fail"), ("trap", b"trap"), ("long", b"long"), ("empty", b"")):
            path = work / name
            path.write_bytes(data)
            files.append(str(path))
        reference = subprocess.run([native], input=Path(module).read_bytes(), capture_output=True, check=True).stdout
        image_digest = digest(struct.pack("<II", 2, 1) + bytes(range(1, 9)))
        for mode in ("optimized", "plain", "strict"):
            obj = work / (mode + ".o")
            compiled = call([vm, "-v", "-o", str(obj), module], W2X_MEMORY_TAIL=mode)
            assert compiled.returncode == 0 and b"bytes ->" in compiled.stderr, compiled
            if mode == "optimized":
                assert obj.read_bytes() == reference
            for kind in ("im", "pdf", "djvu", "svg", "magic"):
                result = call([vm, "stand", "--repeat", "2", kind, str(obj), *files])
                if kind == "im":
                    outcomes = [f"2x1 {image_digest}", "fail", "trap", f"2x1 {image_digest}", "fail"]
                elif kind == "magic":
                    outcomes = ["application/x-fixture", "fail", "trap", "application/x-fixture", "fail"]
                else:
                    outcomes = [document(4), "fail", "trap", document(18), "fail"]
                expected = "".join(f"{Path(path).name} {outcome}\n" for path, outcome in zip(files, outcomes)).encode()
                assert result.returncode == 0 and result.stdout == expected, (mode, kind, result, expected)
                assert re.fullmatch(rb"time [0-9]+\.[0-9]+\n", result.stderr), result.stderr

        obj = work / "optimized.o"
        for repeat in ("0", "-1", "10001", "bad"):
            result = call([vm, "stand", "--repeat", repeat, "magic", str(obj), files[0]])
            assert result.returncode != 0, (repeat, result)
        result = call([vm, "stand", "unknown", str(obj), files[0]])
        assert result.returncode != 0 and b"no work of kind unknown" in result.stderr, result
        too_large = work / "allocation-failure"
        too_large.write_bytes(bytes(9999))
        result = call([vm, "stand", "magic", str(obj), str(too_large)])
        assert result.returncode != 0 and b"malloc of 9999 bytes failed" in result.stderr, result
        short_allocation = work / "short-allocation"
        short_allocation.write_bytes(bytes(9998))
        result = call([vm, "stand", "magic", str(obj), str(short_allocation)])
        assert result.returncode == 1 and result.stdout == b"", result
        assert re.fullmatch(rb"vm: a copy of 9998 bytes into [0-9]+\n", result.stderr), result
        stateful = work / "stateful"
        stateful.write_bytes(b"stateful")
        result = call([vm, "stand", "--repeat", "2", "magic", str(obj), str(stateful)])
        assert result.returncode != 0 and b"result changed on repetition 2" in result.stderr, result

        profiled, profile = work / "profiled.o", work / "profile.jsonl"
        result = call([vm, "-o", str(profiled), module], W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="1")
        assert result.returncode == 0, result
        result = call([vm, "stand", "im", str(profiled), files[0]], W2O_TAIL_PROFILE=str(profile))
        assert result.returncode == 0, result
        records = [json.loads(line) for line in profile.read_text().splitlines()]
        assert records and any(row["functions"] for row in records), records
        assert any(function["guards"] + function["ordinary_checks"] > 0 for row in records for function in row["functions"]), records
        result = call([vm, "-o", str(work / "invalid.o"), module], W2X_MEMORY_TAIL="invalid")
        assert result.returncode != 0 and b"unknown memory tail mode" in result.stderr

    print("stand: all decoder protocols, repeats, traps, failures, strict/tail modes and profile output passed")


if __name__ == "__main__":
    main()
