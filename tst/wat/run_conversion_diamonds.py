#!/usr/bin/env python3
"""Carry integer and saturating float results through conditional loop updates."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = 0xffffffff


def model(seed, selector, rounds):
    values = [seed ^ (((index + 1) * 2654435761) & MASK) for index in range(10)]
    for _ in range(rounds):
        if (selector ^ values[5]) & (1 << 24):
            values[8] = (values[5] * values[7]) & MASK
            numerator = (values[8] << 32) | values[7]
            values[5] = (numerator // (values[9] | 1)) & MASK
        else:
            values[8] = 32 - values[5].bit_length()
        if not (selector ^ values[7]) & (1 << 15):
            # Unsigned 32-bit operands and their sum are exact in f64.
            values[7] = min(MASK, values[2] + values[0])
    result = 0
    for value in values:
        result = (((result << 3) | (result >> 29)) & MASK) ^ value
    return result


def program(template):
    checks = []
    seeds = (0, 1, 17, 0x12345678, 0x7fffffff, 0x80000000, MASK,
             3041712678, 4055616904, 774553834)
    for seed in seeds:
        for selector in (0, 1, 1 << 15, 1 << 24, (1 << 15) | (1 << 24), MASK):
            for rounds in (0, 1, 2, 5, 17):
                expected = model(seed, selector, rounds)
                checks.append(f"(if (i32.ne (call $conversions (i32.const {seed}) (i32.const {selector}) "
                              f"(i32.const {rounds})) (i32.const {expected})) (then unreachable))")
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", len(checks)


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-conversion-diamonds-") as directory:
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
    print(f"WAT conversion diamonds: {count} mixed-width divisions, leading-zero counts and saturating float updates agree with an integer model, wazero and both AOTs")


if __name__ == "__main__":
    main()
