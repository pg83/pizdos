#!/usr/bin/env python3
"""Assemble memory updates and branches directly, without a C/C++ frontend."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def integer(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >= 1 << (bits - 1) else value


def program(template, operation, inverted):
    template = template.replace("i32.sub", "i32." + operation).replace("i64.sub", "i64." + operation)
    if inverted:
        template = template.replace("(if (local.get $value32)", "(if (i32.eqz (local.get $value32))")
        template = template.replace("(if (i64.eqz (local.get $value64))", "(if (i64.ne (local.get $value64) (i64.const 0))")
    statements, checks = [], 0
    for bits in (32, 64):
        values = (0, 1, 7, (1 << (bits - 1)) - 1, 1 << (bits - 1), (1 << bits) - 1)
        for before in values:
            for operand in values:
                expected = {"add": before + operand, "sub": before - operand, "and": before & operand,
                            "or": before | operand, "xor": before ^ operand}[operation] & ((1 << bits) - 1)
                nonzero = (bits == 32) != inverted
                branch = 10 if bool(expected) == nonzero else 13
                statements.extend((
                    f"(i{bits}.store (i32.const 4096) (i{bits}.const {integer(before, bits)}))",
                    f"(call $update{bits} (i32.const 4096) (i{bits}.const {integer(operand, bits)}))",
                    f"(if (i{bits}.ne (i{bits}.load (i32.const 4096)) (i{bits}.const {integer(expected, bits)})) (then unreachable))",
                    f"(if (i32.ne (global.get $branch) (i32.const {branch})) (then unreachable))"))
                checks += 1
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(statements) + "\n))\n", checks


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-updates-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for operation in ("add", "sub", "and", "or", "xor"):
            for inverted in (False, True):
                wat, checks = program(template, operation, inverted)
                source.write_text(wat)
                assembled = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
                assert assembled.returncode == 0, assembled.stderr.decode()
                reference = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
                assert reference.returncode == 0 and reference.stdout == b"", (operation, inverted, reference)
                for strict in (False, True):
                    env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                    result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                    assert result.returncode == 0, (operation, inverted, strict, result.stderr)
                    compiled.write_bytes(result.stdout)
                    result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                    assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (operation, inverted, strict, result)
                    for obj in (compiled, embedded):
                        result = subprocess.run([vm, "run", "--", str(obj)], capture_output=True, env=env, timeout=30)
                        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (operation, inverted, strict, result)
                checked += checks
    print(f"WAT updates: {checked} wrapping memory updates and zero/nonzero branches agree with the integer model, wazero and both AOTs")


if __name__ == "__main__":
    main()
