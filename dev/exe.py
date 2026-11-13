#!/usr/bin/env python3
"""The executable of a program of the kernel: an 8-byte header, "PZD1" and
the length of the executable, header included, a 32-bit little-endian
number; the module compressed by zstd, one frame with its size and a
checksum; and after that length what is the program's own, which the
kernel never reads: python's standard library, a zip zipimport finds from
the end of the file.

    exe.py <module.wasm> <executable> [--tail FILE] [--level N]
"""
import argparse
from compression import zstd
import os
from pathlib import Path
import struct

MAGIC = b"PZD1"
HEADER = 8
WASM = b"\0asm\1\0\0\0"


def pack(module, tail=b"", level=19):
    """The executable of a module, and a tail of the program's own."""
    if not module.startswith(WASM):
        raise ValueError("not a WebAssembly module")

    options = {zstd.CompressionParameter.compression_level: level, zstd.CompressionParameter.checksum_flag: 1}
    frame = zstd.compress(module, options=options)
    length = HEADER + len(frame)

    if length >= 1 << 32:
        raise ValueError(f"an executable of {length} bytes: past 4 GiB")

    return MAGIC + struct.pack("<I", length) + frame + tail


def unpack(executable):
    """The module of an executable, and its tail."""
    if not executable.startswith(MAGIC):
        raise ValueError("not an executable of PZD1")

    (length,) = struct.unpack_from("<I", executable, len(MAGIC))

    return zstd.decompress(executable[HEADER:length]), executable[length:]


def main():
    parser = argparse.ArgumentParser(description="Pack a module into an executable of the kernel.")
    parser.add_argument("module")
    parser.add_argument("executable")
    parser.add_argument("--tail", help="a file the executable ends with, the program's own")
    parser.add_argument("--level", type=int, default=19, help="zstd's level")
    args = parser.parse_args()
    tail = Path(args.tail).read_bytes() if args.tail else b""
    output = Path(args.executable)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_bytes(pack(Path(args.module).read_bytes(), tail, args.level))
    temporary.chmod(0o755)
    os.replace(temporary, output)


if __name__ == "__main__":
    main()
