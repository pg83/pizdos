#!/usr/bin/env python3
"""Command usage, failed output transactions, bounded guest crash traces and
modules compressed by zstd.

    test_cli_errors.py <vm> <native-aot>
"""
from compression import zstd
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def leb(value):
    result = bytearray()
    while value >= 128:
        result.append(value & 127 | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def text(value):
    data = value.encode()
    return leb(len(data)) + data


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value):
    result = bytearray(b"\x41")
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and byte < 64:
            return bytes(result + bytes([byte]))
        result.append(byte | 128)


def recursive_module(depth):
    module = b"\0asm\1\0\0\0" + section(1, b"\2\x60\0\0\x60\1\x7f\0")
    module += section(3, b"\2\0\1") + section(6, b"\1\x7f\1" + constant(0) + b"\x0b")
    module += section(7, b"\1" + text("_start") + b"\0\0")
    bodies = [b"\0" + constant(depth) + b"\x10\1\x0b",
              b"\0\x20\0\x45\x04\x40\0\x0b\x20\0" + constant(1)
              + b"\x6b\x10\1\x23\0" + constant(1) + b"\x6a\x24\0\x0b"]
    module += section(10, b"\2" + b"".join(leb(len(body)) + body for body in bodies))
    names = b"\2\0" + text("entry") + b"\1" + text("recursive_guest")
    return module + section(0, text("name") + b"\1" + leb(len(names)) + names)


def compressed(data):
    options = {zstd.CompressionParameter.compression_level: 19, zstd.CompressionParameter.checksum_flag: 1}
    return zstd.compress(data, options=options)


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="strict", W2X_TAIL_PROFILE="")
    with tempfile.TemporaryDirectory(prefix="pizdos-cli-errors-") as directory:
        work = Path(directory)

        def run(arguments):
            return subprocess.run([vm, *map(str, arguments)], capture_output=True, env=env, timeout=30)

        usage_cases = [[], ["-o"], ["input.wasm"], ["-o", "out.o"], ["a.wasm", "b.wasm"],
                       ["run"], ["run", "--"], ["run", "--root"], ["run", "-a"],
                       ["run", "--unknown"], ["run", "--asan"], ["spec"], ["spec", "a", "b"],
                       ["stand"], ["stand", "magic"], ["stand", "magic", "object.o"]]
        for args in usage_cases:
            result = run(args)
            assert result.returncode == 1 and result.stdout == b"", (args, result)
            assert result.stderr.startswith(b"vm: usage: vm ") and b"panic:" not in result.stderr, (args, result)

        source, output = work / "input.wasm", work / "output.o"
        source.write_bytes(b"\0asm\1\0\0\0")
        output.write_bytes(b"previous successful output")
        absent = work / "absent"
        for args, diagnostic in ((["-o", output, absent], b"no such file or directory"),
                                 (["-o", absent / "object.o", source], b"no such file or directory"),
                                 (["-o", work, source], b"file exists"),
                                 (["run", "--", absent], b"no such file or directory"),
                                 (["run", "--", output], b"not a WebAssembly module"),
                                 (["run", "--asan", "--", output], b"not a WebAssembly module"),
                                 (["spec", absent], b"no such file or directory")):
            result = run(args)
            assert result.returncode == 1 and result.stdout == b"" and diagnostic in result.stderr, (args, result)
            assert output.read_bytes() == b"previous successful output"
            assert not list(work.glob(".vm-*")), (args, "temporary output retained")

        invalid = work / "invalid.json"
        invalid.write_text("{ invalid JSON")
        result = run(["spec", invalid])
        assert result.returncode == 1 and result.stdout == b"" and b"invalid character" in result.stderr, result

        for depth in (8, 80, 1200):
            data = recursive_module(depth)
            source.write_bytes(data)
            result = subprocess.run([native, "--strict"], input=data, capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, result
            output.write_bytes(result.stdout)
            embedded = work / "embedded.o"
            result = run(["-o", embedded, source])
            assert result.returncode == 0 and embedded.read_bytes() == output.read_bytes(), result
            result = run(["run", "--", source])
            assert result.returncode == 129 and result.stdout == b"", (depth, result)
            lines = result.stderr.decode().splitlines()
            assert lines[0] == "wasm trap: unreachable" and b"panic:" not in result.stderr, result
            frames = lines[1:]
            assert all(line.startswith("    ") for line in frames), frames
            gaps = [line for line in frames if re.fullmatch(r"    \.\.\. \d+ frames", line)]
            assert len(gaps) == (depth > 64), (depth, frames)
            assert len(frames) <= 64 and any("recursive_guest+0x" in line for line in frames), (depth, frames)
            if depth < 1024:
                assert frames[-1].startswith("    entry+0x"), frames
            result = run(["run", "--asan", "--", source])
            assert result.returncode == 1 and result.stderr == b"vm: --asan runs programs of the kernel\n", result

        # A module compressed by zstd runs as the module does; frames that
        # do not unpack to one are refused, with why.
        data = recursive_module(80)
        source.write_bytes(data)
        plain = run(["run", "--", source])
        packed = work / "packed.wasm"
        frame = compressed(data)
        packed.write_bytes(frame)
        result = run(["run", "--", packed])
        assert (result.returncode, result.stdout, result.stderr) == (plain.returncode, plain.stdout, plain.stderr), result
        assert result.returncode == 129, result
        # A frame that declares more than vm unpacks, 1 GiB.
        huge = b"\x28\xb5\x2f\xfd\xc0\x50" + struct.pack("<Q", 1 << 31) + b"\1\0\0"
        for name, data, diagnostic in (("truncated", frame[:-9], b"zstd: "),
                                       ("checksum", frame[:-1] + bytes([frame[-1] ^ 1]), b"zstd: "),
                                       ("huge", huge, b"zstd: decompressed size exceeds configured limit"),
                                       ("no module", compressed(b"previous successful output"), b"not a WebAssembly module")):
            packed.write_bytes(data)
            result = run(["run", "--", packed])
            assert result.returncode == 1 and result.stdout == b"" and diagnostic in result.stderr, (name, result)
            assert b"panic:" not in result.stderr, (name, result)
    print("CLI errors: usage, failed output transactions, ASAN restrictions, bounded recursive traces and compressed modules passed")


if __name__ == "__main__":
    main()
