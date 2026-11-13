#!/usr/bin/env python3
"""Assemble eager selects of saturating float conversions and check their bits."""
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >= 1 << (bits - 1) else value


def program(template, floating, bits, unsigned):
    threshold = math.ldexp(1.0, bits - 1)
    template = template.replace("f64", "f" + str(floating))
    template = template.replace("(result i32)", f"(result i{bits})").replace("(i32.const 73)", f"(i{bits}.const 73)")
    template = template.replace("i32.trunc_sat_", f"i{bits}.trunc_sat_")
    template = template.replace("2147483648", threshold.hex())
    if unsigned:
        template = template.replace(f"trunc_sat_f{floating}_s", f"trunc_sat_f{floating}_u")
    samples = (-math.inf, -2 * threshold, -threshold, -threshold + 1024, -1.5, -0.0, 0.0, 0.5,
               1.0, 1.5, 42.75, threshold - 1024, threshold - 1, threshold, 2 * threshold - 1, 2 * threshold, math.inf, math.nan)
    checks, statements = 0, []
    lower, upper = (0, (1 << bits) - 1) if unsigned else (-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
    for sample in samples:
        value = struct.unpack("<f", struct.pack("<f", sample))[0] if floating == 32 else sample
        if math.isnan(value):
            converted = 0
        elif value <= lower:
            converted = lower
        elif value >= upper:
            converted = upper
        else:
            converted = math.trunc(value)
        selected = converted if 0 <= value < threshold else 73
        selected_false = 73 if value < 0 or value >= threshold else converted
        for function, expected in (("choose_true", selected), ("choose_false", selected_false), ("saturate", converted)):
            statements.append(f"(if (i{bits}.ne (call ${function} (f{floating}.const {value.hex()})) "
                              f"(i{bits}.const {signed(expected, bits)})) (then unreachable))")
            checks += 1
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(statements) + "\n))\n", checks


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-float-selects-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for floating in (32, 64):
            for bits in (32, 64):
                for unsigned in (False, True):
                    wat, checks = program(template, floating, bits, unsigned)
                    source.write_text(wat)
                    result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
                    assert result.returncode == 0, result.stderr.decode()
                    result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
                    assert result.returncode == 0 and result.stdout == b"", (floating, bits, unsigned, result)
                    for strict in (False, True):
                        env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                        result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                        assert result.returncode == 0, (floating, bits, unsigned, strict, result.stderr)
                        compiled.write_bytes(result.stdout)
                        result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (floating, bits, unsigned, strict, result)
                        result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
                        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (floating, bits, unsigned, strict, result)
                    checked += checks
    print(f"WAT float selects: {checked} signed/unsigned eager selects and saturations agree with the numeric model, wazero and both AOTs")


if __name__ == "__main__":
    main()
