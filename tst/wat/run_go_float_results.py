#!/usr/bin/env python3
"""Preserve Go floating results when an internal tuple returns them elsewhere."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "dev"))

from exe import pack  # noqa: E402


def signed(value):
    return value if value < 1 << 63 else value - (1 << 64)


def bits(value):
    return int.from_bytes(struct.pack("<d", value), "little")


def program(template, reference=False):
    implementation = ('(func $difference (param $later i64) (param $earlier i64) (result f64) '
                      '(f64.sub (f64.convert_i64_s (local.get $later)) (f64.convert_i64_s (local.get $earlier))))') if reference else (
                      '(import "kernel" "difftime" (func $difference (param i64 i64) (result f64)))')
    template = template.replace(";; DIFFERENCE", implementation)
    checks, count = [], 0
    for first in (0, 1 << 63, bits(13.25), 0x7ff8000000000123):
        for later, earlier in ((0, 0), (17, -3), (-17, 3), ((1 << 63) - 1, -(1 << 63)),
                               (1 << 53, (1 << 53) + 1), (-(1 << 63), -(1 << 63) + 4096)):
            arguments = f"(f64.reinterpret_i64 (i64.const {signed(first)})) (i64.const {later}) (i64.const {earlier})"
            difference = bits(float(later) - float(earlier))
            calls = [(f"(call $relay {arguments})", difference)]
            for condition in (0, 1):
                other = 0xfff8000000000456
                call = (f"(call $choose (i32.const {condition}) (f64.reinterpret_i64 (i64.const {signed(first)})) "
                        f"(f64.reinterpret_i64 (i64.const {signed(other)})) (i64.const {later}) (i64.const {earlier}))")
                calls.append((call, other if condition else difference))
            for call, expected in calls:
                checks += [call, "(local.set $second)",
                           f"(if (i64.ne (i64.reinterpret_f64) (i64.const {signed(first)})) (then unreachable))",
                           f"(if (i64.ne (i64.reinterpret_f64 (local.get $second)) (i64.const {signed(expected)})) (then unreachable))"]
                count += 1
    return template.rstrip()[:-1] + '\n(func (export "_start") (local $second f64)\n' + "\n".join(checks) + "\n))\n", count


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    template = Path(fixture).read_text()
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-go-float-results-") as directory:
        work = Path(directory)
        source, module, compiled, embedded = (work / name for name in ("input.wat", "input.wasm", "native.o", "embedded.o"))
        for reference in (True, False):
            wat, checks = program(template, reference)
            source.write_text(wat)
            result = subprocess.run([wat2wasm, str(source), "-o", str(module)], capture_output=True, timeout=30)
            assert result.returncode == 0, result.stderr.decode()
            if reference:
                result = subprocess.run([wazero, "-interpreter", str(module)], capture_output=True, timeout=30)
                assert result.returncode == 0 and result.stdout == b"", result
            for strict in (False, True):
                env = dict(os.environ, W2X_MEMORY_TAIL="strict" if strict else "optimized", W2X_TAIL_PROFILE="")
                result = subprocess.run([native, *(["--strict"] if strict else [])], input=module.read_bytes(), capture_output=True, env=env, timeout=60)
                assert result.returncode == 0, result.stderr
                compiled.write_bytes(result.stdout)
                result = subprocess.run([vm, "-o", str(embedded), str(module)], capture_output=True, env=env, timeout=60)
                assert result.returncode == 0 and embedded.read_bytes() == compiled.read_bytes(), result
                executable = work / "input"
                executable.write_bytes(pack(module.read_bytes()))
                result = subprocess.run([vm, "run", "--", str(executable)], capture_output=True, env=env, timeout=30)
                assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (reference, strict, result)
    print(f"WAT Go float results: {checks} multi-result relays and branch merges preserve result positions, NaN payloads and signed zero with a model, wazero reference and both AOTs")


if __name__ == "__main__":
    main()
