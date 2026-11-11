"""Turn the embedded reactor's LLVM 23 counters into source coverage.

The counter layout is exported by dev/coverage_wasm.c. LLVM's text profile
format lets llvm-profdata own the indexed format; no native raw-profile
header, pointer size or compiler-rt dependency leaks into the Wasm build.
"""
import array
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys


def uleb(data, offset):
    value, shift = 0, 0
    while offset < len(data) and shift < 64:
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
        shift += 7
    raise RuntimeError("invalid LLVM profile name length")


def names(data):
    result, offset = {}, 0
    while offset < len(data):
        size, offset = uleb(data, offset)
        compressed, offset = uleb(data, offset)
        if compressed:
            raise RuntimeError("Wasm coverage names must be built without compression")
        end = offset + size
        if end > len(data):
            raise RuntimeError("truncated LLVM profile names")
        for name in data[offset:end].split(b"\x01"):
            key = int.from_bytes(hashlib.md5(name).digest()[:8], "little")
            if key in result and result[key] != name:
                raise RuntimeError("LLVM profile name hash collision")
            result[key] = name
        offset = end
    return result


def counters(path):
    result = array.array("Q")
    result.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        result.byteswap()
    return result


def text_profile(metadata, counts, stream):
    layout = metadata["layout"]
    if layout[6] != 23 or layout[7:10] != [56, 16, 36]:
        raise RuntimeError(f"unsupported LLVM Wasm profile layout: {layout}")
    data = base64.b64decode(metadata["data"], validate=True)
    functions = names(base64.b64decode(metadata["names"], validate=True))
    stride, pointer, number = layout[7:10]
    if len(data) != layout[1] - layout[0] or len(data) % stride or len(counts) * 8 != layout[3] - layout[2]:
        raise RuntimeError("invalid LLVM Wasm profile section sizes")
    for offset in range(0, len(data), stride):
        name, cfg_hash = struct.unpack_from("<QQ", data, offset)
        relative = struct.unpack_from("<i", data, offset + pointer)[0]
        count = struct.unpack_from("<I", data, offset + number)[0]
        # CounterPtr is relative to the record, not to the pointer field.
        start = layout[0] + offset + relative - layout[2]
        if start < 0 or start % 8 or start + count * 8 > len(counts) * 8:
            raise RuntimeError("LLVM profile counter pointer is outside its section")
        function = functions[name].decode("utf-8")
        if "\n" in function or "\r" in function:
            raise RuntimeError("invalid LLVM profile function name")
        stream.write(f"{function}\n{cfg_hash}\n{count}\n")
        stream.writelines(f"{value}\n" for value in counts[start // 8:start // 8 + count])
        stream.write("\n")


def report(suite, raw, destination):
    groups = {False: [], True: []}
    for path in sorted(raw.glob("*.json")):
        metadata = json.loads(path.read_text())
        profiles = sorted(raw.glob(path.stem + "-*.counts"))
        if not profiles:
            raise RuntimeError(f"Wasm profile {path.name} has no counters")
        counts = counters(profiles[0])
        for profile in profiles[1:]:
            values = counters(profile)
            if len(values) != len(counts):
                raise RuntimeError("Wasm profile counter lengths differ")
            for index, value in enumerate(values):
                counts[index] += value
        groups[bool(metadata["layout"][10])].append((metadata, counts))
    if not groups[False]:
        raise RuntimeError("embedded Wasm AOT produced no LLVM profiles")
    lines = {}
    source = Path(suite["source_root"], "bin/aot/aot.cpp").resolve()
    for chaos, profiles in groups.items():
        stem = "aot-wasm-chaos" if chaos else "aot-wasm"
        text = destination / (stem + ".proftext")
        indexed = destination / (stem + ".profdata")
        lcov = destination / (stem + ".lcov")
        if not profiles:
            # A focused suite may not invoke the chaos compiler at all.
            text.write_text("")
            indexed.write_bytes(b"")
            lcov.write_text("")
            continue
        with text.open("w") as stream:
            for metadata, counts in profiles:
                text_profile(metadata, counts, stream)
        subprocess.run([suite["llvm_profdata"], "merge", "-sparse", str(text), "-o", str(indexed)], check=True)
        with lcov.open("w") as stream:
            subprocess.run([suite["llvm_cov"], "export", "-format=lcov",
                            suite["aot_wasm_chaos" if chaos else "aot_wasm"],
                            "-instr-profile=" + str(indexed), str(source)], stdout=stream, check=True)
        selected = False
        for line in lcov.read_text().splitlines():
            if line.startswith("SF:"):
                selected = Path(line[3:]).resolve() == source
            elif selected and line.startswith("DA:"):
                number, count, *_ = line[3:].split(",")
                number, count = int(number), int(count)
                lines[number] = lines.get(number, 0) + count
    if not lines:
        raise RuntimeError("LLVM Wasm profile contains no aot.cpp lines")
    return lines
