#!/usr/bin/env python3
"""Count a program's bounds checks and loops by site in the tail profile.

    test_tail_profile.py <vm> <native-aot>
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BASE, LENGTH, ROUNDS = 1024, 5, 3
DST, SRC, COUNT = 4096, 2048, 20
# The offset of each memory access and loop instruction in the bodies below:
# what a check and a loop's counts must be of. A loop the optimizer gives a
# header of its own is known by that header's first instruction: step.
ACCESSES = ("test", "load", "source", "gather", "store", "word", "byte")
LOOPS = ("outer", "inner", "copy", "four", "step")


def leb(value, signed=False):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if value == 0 and (not signed or byte < 64) or signed and value == -1 and byte >= 64:
            return bytes(out + bytes([byte]))
        out.append(byte | 128)


def section(kind, body):
    return bytes([kind]) + leb(len(body)) + body


def constant(value):
    return b"\x41" + leb(value, True)


class Body:
    """A function body that remembers where marked instructions begin."""

    def __init__(self, locals_=b"\0"):
        self.code = bytearray(locals_)
        self.marks = {}

    def __call__(self, data, mark=None):
        if mark:
            self.marks[mark] = len(self.code)
        self.code += data


def walk():
    # walk(base, n, rounds): rounds times, a byte at base tested in a branch,
    # and n words from base summed in an inner loop.
    body = Body(b"\1\3\x7f")
    body(b"\x03\x40", "outer")
    body(b"\x02\x40\x20\0")
    body(b"\x2d\0\0", "test")
    body(b"\x41\7\x47\x0d\0\x20\5\x41\1\x6a\x21\5\x0b")
    body(b"\x41\0\x21\3\x02\x40\x20\1\x45\x0d\0")
    body(b"\x03\x40", "inner")
    body(b"\x20\5\x20\0\x20\3\x41\2\x74\x6a")
    body(b"\x28\2\0", "load")
    body(b"\x6a\x21\5\x20\3\x41\1\x6a\x22\3\x20\1\x49\x0d\0\x0b\x0b")
    body(b"\x20\4\x41\1\x6a\x22\4\x20\2\x49\x0d\0\x0b\x20\5\x0b")
    return body


def copy():
    # copy(dst, src, n): dst[i] = the byte at src[i], a loop the compiler
    # versions: the gather keeps its check in the copy without the others.
    body = Body(b"\1\1\x7f")
    body(b"\x02\x40\x20\2\x45\x0d\0")
    body(b"\x03\x40", "copy")
    body(b"\x20\0\x20\3\x41\2\x74\x6a\x20\1\x20\3\x41\2\x74\x6a")
    body(b"\x28\2\0", "source")
    body(b"\x2d\0\0", "gather")
    body(b"\x36\2\0", "store")
    body(b"\x20\3\x41\1\x6a\x22\3\x20\2\x49\x0d\0\x0b\x0b\x0b")
    return body


def four():
    # four(src): the bytes at src[0..3] summed by a loop of four trips, which
    # the compiler may unroll into copies.
    body = Body(b"\1\2\x7f")
    body(b"\x03\x40", "four")
    body(b"\x20\2\x20\0\x20\1\x41\2\x74\x6a")
    body(b"\x28\2\0", "word")
    body(b"\x2d\0\0", "byte")
    body(b"\x6a\x21\2\x20\1\x41\1\x6a\x22\1\x41\4\x47\x0d\0\x0b\x20\2\x0b")
    return body


def rotated():
    # rotated(n): n trips of a loop whose first one alone clears a flag and
    # stores; the back edge, knowing the flag clear, goes past that branch
    # to a header after it.
    body = Body(b"\1\2\x7f")
    body(b"\x41\1\x21\2\x03\x40\x20\2\x04\x40\x41\0\x21\2\x41\x20\x41\1\x3a\0\0\x0b\x20\1\x41\1")
    body(b"\x6a", "step")
    body(b"\x22\1\x20\0\x49\x0d\0\x0b\x0b")
    return body


def module():
    start = Body()
    start(constant(BASE) + constant(LENGTH) + constant(ROUNDS) + b"\x10\1\x1a")
    start(constant(DST) + constant(SRC) + constant(COUNT) + b"\x10\2")
    start(constant(SRC) + b"\x10\3\x1a" + constant(LENGTH) + b"\x10\4\x0b")
    bodies = [start, walk(), copy(), four(), rotated()]
    types = b"\5\x60\0\0\x60\3\x7f\x7f\x7f\1\x7f\x60\3\x7f\x7f\x7f\0\x60\1\x7f\1\x7f\x60\1\x7f\0"
    head = b"\0asm\1\0\0\0" + section(1, types) + section(3, b"\5\0\1\2\3\4")
    head += section(5, b"\1\0\1") + section(7, b"\1\6_start\0\0")
    contents = leb(len(bodies))
    offsets = []
    for body in bodies:
        contents += leb(len(body.code))
        offsets.append(len(contents))
        contents += body.code
    code = section(10, contents)
    at = len(head) + len(code) - len(contents)
    marks = {name: (k, at + offsets[k] + place) for k, body in enumerate(bodies) for name, place in body.marks.items()}
    return head + code, marks


def main():
    vm, native = sys.argv[1:]
    env = dict(os.environ, W2X_MEMORY_TAIL="optimized", W2X_TAIL_PROFILE="1")
    with tempfile.TemporaryDirectory(prefix="pizdos-tail-profile-") as directory:
        work = Path(directory)
        source, compiled, embedded, profile = (work / name for name in ("input.wasm", "native.o", "embedded.o", "tail.jsonl"))
        wasm, marks = module()
        source.write_bytes(wasm)
        result = subprocess.run([native, "--tail-profile"], input=wasm, capture_output=True, timeout=60)
        assert result.returncode == 0, result.stderr
        compiled.write_bytes(result.stdout)
        result = subprocess.run([vm, "-o", str(embedded), str(source)], env=env, capture_output=True, timeout=60)
        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
        result = subprocess.run([vm, "run", "--", str(source)], env=dict(env, W2O_TAIL_PROFILE=str(profile)),
                                capture_output=True, timeout=30)
        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result

        rows = [json.loads(line) for line in profile.read_text().splitlines()]
        assert len(rows) == 1 and rows[0]["module"] == str(source), rows
        functions = {function["function"]: function for function in rows[0]["functions"]}
        assert set(functions) == {1, 2, 3, 4}, functions
        # Copies of code count as sites of their own: a site's count is the
        # sum of those of its kind at its instruction.
        sites = {}
        for function in functions.values():
            offsets = [site["offset"] for site in function["sites"]]
            assert offsets == sorted(offsets), function
            assert function["ordinary_checks"] == sum(site["count"] for site in function["sites"] if site["kind"] == "check")
            for site in function["sites"]:
                key = (function["function"], site["kind"], site["offset"])
                sites[key] = sites.get(key, 0) + site["count"]
        # Every check is of an access, every loop count of a loop instruction.
        accesses = {marks[name] for name in ACCESSES}
        loops = {marks[name] for name in LOOPS}
        for (function, kind, offset), count in sites.items():
            assert count > 0 and kind in ("check", "loop", "entry", "fast_loop", "fast_entry"), (function, kind, offset, count)
            assert (function, offset) in (accesses if kind == "check" else loops), (function, kind, offset, marks)

        def count(kind, name):
            function, offset = marks[name]
            return sites.get((function, kind, offset), 0)

        assert (count("loop", "outer"), count("entry", "outer"), count("check", "test")) == (ROUNDS, 1, ROUNDS), sites
        assert (count("loop", "inner"), count("entry", "inner")) == (ROUNDS * LENGTH, ROUNDS), sites
        assert count("check", "load") == ROUNDS * LENGTH and functions[1]["ordinary_checks"] == ROUNDS + ROUNDS * LENGTH, sites
        # The copy loop runs as its versioned copy, which its guard entered.
        assert (count("fast_loop", "copy"), count("fast_entry", "copy"), count("check", "gather")) == (COUNT, 1, COUNT), sites
        assert count("check", "byte") == 4, sites
        assert (count("loop", "step"), count("entry", "step")) == (LENGTH, 1), sites
        assert all((function["guards"], function["fallbacks"], function["omitted_checks"]) == (0, 0, 0) for function in functions.values())
    print("tail profile: checks, loop iterations and entries counted at their instructions")


if __name__ == "__main__":
    main()
