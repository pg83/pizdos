#!/usr/bin/env python3
"""Keep loads and intervening arithmetic explicit in hand-written Wasm."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def signed(value, bits=32):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >= 1 << (bits - 1) else value


def program(template):
    statements, checks = [], 0
    comparisons = []
    for bits in (32, 64):
        values = (0, 1, 17, (1 << (bits - 1)) - 1, 1 << (bits - 1), (1 << bits) - 1)
        for relation in ("eq", "ne", "lt_s", "lt_u", "gt_s", "gt_u", "le_s", "le_u", "ge_s", "ge_u"):
            for reverse in (False, True):
                name = f"compare_{bits}_{relation}_{int(reverse)}"
                operands = [f"(i{bits}.load (local.get $address))", "(local.get $value)"]
                if reverse:
                    operands.reverse()
                comparisons.append(f"(func ${name} (param $address i32) (param $value i{bits}) (result i32) "
                                   f"(i{bits}.{relation} {' '.join(operands)}))")
                for left in values:
                    for right in values:
                        a, b = (right, left) if reverse else (left, right)
                        if relation.endswith("_s"):
                            a, b = signed(a, bits), signed(b, bits)
                        expected = {"eq": a == b, "ne": a != b, "lt": a < b, "gt": a > b,
                                    "le": a <= b, "ge": a >= b}[relation.split("_")[0]]
                        statements.extend((f"(i{bits}.store (i32.const 4096) (i{bits}.const {signed(left, bits)}))",
                                           f"(if (i32.ne (call ${name} (i32.const 4096) (i{bits}.const {signed(right, bits)})) "
                                           f"(i32.const {int(expected)})) (then unreachable))"))
                        checks += 1
    template = template.replace(";; COMPARISONS", "\n".join(comparisons))
    roundtrips = []
    for width in (8, 16, 32):
        name = f"unsigned_roundtrip_{width}"
        roundtrips.append(f"(func ${name} (param $address i32) (result i64) "
                          f"(i64.extend_i32_u (i32.wrap_i64 (i64.load{width}_u (local.get $address)))))")
        for raw in (0, 1, 0x7f, 0x80, 0xffff, 0x80000000, 0xffffffff, 0x8765432189abcdef):
            expected = raw & ((1 << width) - 1)
            statements.extend((f"(i64.store (i32.const 4096) (i64.const {signed(raw, 64)}))",
                               f"(if (i64.ne (call ${name} (i32.const 4096)) (i64.const {expected})) (then unreachable))"))
            checks += 1
    template = template.replace(";; UNSIGNED_ROUNDTRIPS", "\n".join(roundtrips))
    for bits in (32, 64):
        values = (0.0, -0.0, 1.0, -1.0, 13.25, 2.0 ** -40, 2.0 ** 40)
        for operation in ("add", "mul"):
            for left in values:
                for right in values:
                    expected = left + right if operation == "add" else left * right
                    raw = int.from_bytes(struct.pack("<f" if bits == 32 else "<d", expected), "little")
                    statements.extend((
                        f"(f{bits}.store (i32.const 4096) (f{bits}.const {left.hex()}))",
                        f"(if (i{bits}.ne (i{bits}.reinterpret_f{bits} (call ${operation}{bits} (i32.const 4096) (f{bits}.const {right.hex()}))) "
                        f"(i{bits}.const {signed(raw, bits)})) (then unreachable))"))
                    checks += 1
    for before in (0, 1, -1, -(1 << 31)):
        for numerator in (-(1 << 31), -77, -1, 0, 1, 77, (1 << 31) - 1):
            for denominator in (-7, -1, 1, 7):
                if numerator == -(1 << 31) and denominator == -1:
                    continue
                quotient = abs(numerator) // abs(denominator)
                if (numerator < 0) != (denominator < 0):
                    quotient = -quotient
                statements.extend((
                    f"(i32.store (i32.const 4112) (i32.const {before}))",
                    f"(if (i32.ne (call $divide_between (i32.const 4112) (i32.const {numerator}) (i32.const {denominator})) "
                    f"(i32.const {quotient})) (then unreachable))",
                    f"(if (i32.ne (i32.load (i32.const 4112)) (i32.const {signed(before + quotient)})) (then unreachable))"))
                checks += 1
    # Discarding the old page count must retain growth and its limit.
    for delta, size in ((1, 2), (1, 2), (0, 2)):
        statements.extend((f"(drop (memory.grow (i32.const {delta})))",
                           f"(if (i32.ne (memory.size) (i32.const {size})) (then unreachable))"))
        checks += 1
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(statements) + "\n))\n", checks


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    wat, checks = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-memory-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        source.write_text(wat)
        result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr.decode()
        result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
        assert result.returncode == 0 and result.stdout == b"", result
        for strict in (False, True):
            env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
            result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
            assert result.returncode == 0, result.stderr
            compiled.write_bytes(result.stdout)
            result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
            assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
            for obj in (compiled, embedded):
                result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), result
    print(f"WAT memory arithmetic: {checks} signed/unsigned comparisons, floating load operands and intervening signed divisions agree with numeric models, wazero and both AOTs")


if __name__ == "__main__":
    main()
