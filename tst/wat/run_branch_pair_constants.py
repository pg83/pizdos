#!/usr/bin/env python3
"""Check full-width constants and live payloads across paired branches."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MASK = (1 << 64) - 1


def program(template):
    prefix, rest = template.split(";; BEGIN_FUNCTION")
    body, suffix = rest.split(";; END_FUNCTION")
    functions, checks, count = [], [], 0
    for width in (0, 8, 16, 24, 48, 96):
        payload = [((i + 1) * 0x9e3779b97f4a7c15) & MASK for i in range(width)]
        parameters = " ".join("(param i64)" for _ in payload)
        arguments = " ".join(f"(i64.const {value})" for value in payload)
        result = "(i64.xor (global.get $tag) (local.get $y))"
        for i in range(width):
            result = f"(i64.xor (i64.rotl {result} (i64.const 3)) (local.get {i + 2}))"
        for constant in (0x100000000, 0x8000000000000000, 0x7fffffffffffffff, 0xffffffff00000000):
            for conjunction in (False, True):
                name = f"choose_{width}_{constant}_{int(conjunction)}"
                function = body.replace("$choose", "$" + name).replace(";; PARAMETERS", parameters)
                function = function.replace(";; BOOLEAN", "i32.and" if conjunction else "i32.or")
                functions.append(function.replace(";; CONSTANT\n", str(constant)).replace(";; RESULT", result))
                for x in (0, 16, 17, 0xffffffff):
                    for y in (constant, constant ^ 1):
                        branch = (x < 17 and y == constant) if conjunction else (x < 17 or y == constant)
                        expected = (123 if branch else 45) ^ y
                        for value in payload:
                            expected = (((expected << 3) | (expected >> 61)) & MASK) ^ value
                        checks.append(f"(if (i64.ne (call ${name} (i32.const {x}) (i64.const {y}) "
                                      f"{arguments}) (i64.const {expected})) (then unreachable))")
                        count += 1
    text = prefix + "\n".join(functions) + suffix.rstrip()[:-1]
    return text + '\n(func (export "_start")\n' + "\n".join(checks) + "\n))\n", count


def main():
    vm, native, wazero, wat2wasm, fixture = sys.argv[1:]
    text, count = program(Path(fixture).read_text())
    with tempfile.TemporaryDirectory(prefix="pizdos-wat-branch-constants-") as directory:
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
            result = subprocess.run([vm, "run", "--", str(module)], capture_output=True, env=env, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, b"", b""), (strict, result)
    print(f"WAT branch constants: {count} modeled full-width comparisons retain up to 96 ordered payloads, with wazero and both AOTs in strict/optimized modes")


if __name__ == "__main__":
    main()
