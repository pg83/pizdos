#!/usr/bin/env python3
"""Command usage, failed output transactions, bounded guest crash traces and
executables: the PZD1 header, the module compressed by zstd, a tail.

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

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "dev"))

from exe import pack  # noqa: E402


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


def kernel_module():
    """A module whose _start calls a function of the kernel: a program of the
    kernel."""
    body = b"\0\x10\0\x1a\x0b"
    return (b"\0asm\1\0\0\0" + section(1, b"\2\x60\0\1\x7f\x60\0\0") + section(2, b"\1" + text("kernel") + text("getpid") + b"\0\0")
            + section(3, b"\1\1") + section(7, b"\1" + text("_start") + b"\0\1") + section(10, b"\1" + leb(len(body)) + body))


def header(length):
    return b"PZD1" + struct.pack("<I", length)


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

        # An executable runs as its module does, whatever follows its
        # length; one whose length or frames are wrong is refused, with why,
        # and a bare frame is no executable.
        data = recursive_module(80)
        source.write_bytes(data)
        plain = run(["run", "--", source])
        executable = work / "input"
        packed = pack(data)
        for tail in (b"", b"PK\5\6 what the program has after its executable"):
            executable.write_bytes(packed + tail)
            result = run(["run", "--", executable])
            assert (result.returncode, result.stdout, result.stderr) == (plain.returncode, plain.stdout, plain.stderr), result
            assert result.returncode == 129, result
        frame = packed[8:]
        # A frame that declares more than vm unpacks, 1 GiB.
        huge = b"\x28\xb5\x2f\xfd\xc0\x50" + struct.pack("<Q", 1 << 31) + b"\1\0\0"
        text_frame = zstd.compress(b"previous successful output")
        for name, data, diagnostic in (("past the file", header(len(packed) + 1) + frame, b"PZD1: a length of"),
                                       ("no frames", header(8) + frame, b"PZD1: a length of 8"),
                                       ("truncated", header(len(packed) - 9) + frame[:-9], b"zstd: "),
                                       ("checksum", packed[:-1] + bytes([packed[-1] ^ 1]), b"zstd: "),
                                       ("huge", header(8 + len(huge)) + huge, b"zstd: decompressed size exceeds configured limit"),
                                       ("no module", header(8 + len(text_frame)) + text_frame, b"not a WebAssembly module"),
                                       ("bare frame", frame, b"not a WebAssembly module")):
            executable.write_bytes(data)
            result = run(["run", "--", executable])
            assert result.returncode == 1 and result.stdout == b"" and diagnostic in result.stderr, (name, result)
            assert b"panic:" not in result.stderr, (name, result)
        # A program of the kernel runs from an executable only.
        source.write_bytes(kernel_module())
        result = run(["run", "--", source])
        assert result.returncode == 1 and b"a program of the kernel, not in an executable of PZD1" in result.stderr, result
    print("CLI errors: usage, failed output transactions, ASAN restrictions, bounded recursive traces and executables passed")


if __name__ == "__main__":
    main()
