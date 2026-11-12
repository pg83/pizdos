#!/usr/bin/env python3
"""Keep scalar values live through loop-carried conditional copies and calls."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = 0xffffffff


def rotate(value, amount):
    amount &= 31
    return ((value << amount) | (value >> ((-amount) & 31))) & MASK


def mix(a, b, c):
    return (rotate(a, 5) ^ ((b + c) * 33)) & MASK


def model(seed, selector, rounds):
    values = [seed ^ (((index + 1) * 2654435761) & MASK) for index in range(24)]
    for _ in range(rounds):
        if (selector ^ values[22]) & 2:
            values[5] = values[8] if values[11] & 1 else values[3]
        else:
            values[9] = (values[2] - values[5]) & MASK
            values[12] = values[11] if values[6] & 1 else values[20]
            values[13] = values[22] if values[4] & 1 else values[3]
        if (selector ^ values[21]) & (1 << 29):
            values[0] = mix(values[10], values[16], values[10])
            values[14] = values[23] if values[6] & 1 else values[4]
            values[6] = (values[11] - values[5]) & MASK
            values[22] = mix(values[21], values[15], values[21])
            values[5] = rotate(values[23], values[6])
            values[9] = (values[9] + values[11]) & MASK
        else:
            values[0] = (values[12] - values[8]) & MASK
            values[1] = (values[6] - values[17]) & MASK
            values[6] = rotate(values[1], values[15])
    result = 0
    for value in values:
        result = rotate(result, 3) ^ value
    return result


def program(template):
    checks = []
    for seed in (0, 1, 17, 0x7fffffff, 0x80000000, MASK):
        for selector in (0, 1, 2, 64, 1 << 29, MASK):
            for rounds in (0, 1, 2, 5, 17):
                expected = model(seed, selector, rounds)
                checks.append(f"(if (i32.ne (call $diamonds (i32.const {seed}) (i32.const {selector}) "
                              f"(i32.const {rounds})) (i32.const {expected})) (then unreachable))")
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", len(checks)


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-diamond-lifetimes-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        source.write_text(text)
        result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr.decode()
        result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0 and result.stdout == b"", result
        for strict in (False, True):
            env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
            result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, (strict, result.stderr)
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (strict, result)
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
    print(f"WAT diamond lifetimes: {count} loop-carried selections, calls and variable rotations preserve 24 values with an integer model, wazero and both AOTs")


if __name__ == "__main__":
    main()
