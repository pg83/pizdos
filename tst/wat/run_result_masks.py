#!/usr/bin/env python3
"""Mask results returned in registers and stack slots by direct/indirect calls."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def program(template, count, width, indirect):
    values = [(index * 0x9e3779b1 + 0x87654321) & 0xffffffff for index in range(count)]
    mask = (1 << (width * 8)) - 1
    template = template.replace(";; RESULTS", " ".join(["i32"] * count))
    template = template.replace(";; VALUES", "\n".join(
        f"local.get $seed\ni32.const {value}\ni32.xor" for value in values))
    template = template.replace(";; LOCALS", "\n".join(f"(local $r{index} i32)" for index in range(count)))
    template = template.replace(";; CALL", "i32.const 0\ncall_indirect (type $tuple)" if indirect else "call $values")
    template = template.replace(";; CAPTURE", "\n".join(f"local.set $r{index}" for index in reversed(range(count))))
    template = template.replace(";; HASH", "\n".join(
        f"i64.const 33\ni64.mul\nlocal.get $r{index}\ni32.const {mask}\ni32.and\ni64.extend_i32_u\ni64.xor"
        for index in range(count)))
    checks = []
    for seed in (0, 0xffffffff, 0x12345678, 0x80000000, 0xffff0000, 0x0000ffff):
        expected = 0
        for value in values:
            expected = ((expected * 33) ^ ((value ^ seed) & mask)) & ((1 << 64) - 1)
        checks.append(f"(if (i64.ne (call $hash (i32.const {seed})) (i64.const {expected})) (then unreachable))")
    return template.rstrip()[:-1] + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", len(checks)


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    checked = 0
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-result-masks-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for count in (10, 17, 32, 65):
            for width in (1, 2):
                for indirect in (False, True):
                    wat, checks = program(template, count, width, indirect)
                    source.write_text(wat)
                    result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
                    assert result.returncode == 0, result.stderr.decode()
                    result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
                    assert result.returncode == 0 and result.stdout == b"", (count, width, indirect, result)
                    for strict in (False, True):
                        env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                        result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                        assert result.returncode == 0, (count, width, indirect, strict, result.stderr)
                        compiled.write_bytes(result.stdout)
                        result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                        assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), (count, width, indirect, strict, result)
                        result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
                        assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (count, width, indirect, strict, result)
                    checked += checks
    print(f"WAT result masks: {checked} direct/indirect calls returning 10/17/32/65 values preserve byte/halfword hashes with a model, wazero and both AOTs")


if __name__ == "__main__":
    main()
